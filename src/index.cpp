#include "index.h"

#include "listing.h"
#include "paths.h"

#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "advapi32.lib")

namespace {
constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\FastExplorerIndex";
constexpr wchar_t kSvc[] = L"FastExplorerIndex";
constexpr wchar_t kSvcParams[] = L"SYSTEM\\CurrentControlSet\\Services\\FastExplorerIndex\\Parameters";
constexpr uint32_t kQueryMagic = 0x51494546;  // « FEIQ »
constexpr uint32_t kReplyMagic = 0x52494546;  // « FEIR »
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRequest = 64 + (kIndexMaxQuery + kIndexMaxScope) * 2;
}  // namespace

// ───────────────────────────── Index d'un volume ─────────────────────────────

uint16_t VolumeIndex::Flags(uint32_t attr) {
    uint16_t f = 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) f |= F_DIR;
    if (attr & FILE_ATTRIBUTE_HIDDEN) f |= F_HIDDEN;
    if (attr & FILE_ATTRIBUTE_SYSTEM) f |= F_SYSTEM;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) f |= F_REPARSE;
    return f;
}

bool VolumeIndex::Shown(uint16_t f, const IndexQuery& q) const {
    if (!(f & F_HIDDEN)) return true;
    if (f & F_SYSTEM) return q.showHidden && q.showSuperHidden;  // fichier protégé du système
    return q.showHidden;
}

void VolumeIndex::Clear() {
    nodes_.clear();
    pool_.clear();
    garbage_ = count_ = 0;
}

void VolumeIndex::Put(uint64_t frn, uint64_t parentFrn, const wchar_t* name, uint32_t n, uint32_t attr) {
    const uint64_t rec = Rec(frn);
    if (rec >= 0xFFFFFFF0ull || n > 0xFFFF) return;
    if (rec >= nodes_.size()) nodes_.resize(std::max<size_t>(rec + 1, nodes_.size() + nodes_.size() / 8 + 1024), Node{});
    Node& x = nodes_[rec];
    const uint32_t parent = (uint32_t)std::min<uint64_t>(Rec(parentFrn), 0xFFFFFFFFull);
    const uint16_t fl = Flags(attr) | F_USED;
    if (x.flags & F_USED) {
        if (x.nameLen == n && (n == 0 || wmemcmp(pool_.data() + x.nameOff, name, n) == 0)) {  // même nom : sur place
            x.parent = parent;
            x.flags = fl;
            return;
        }
        garbage_ += x.nameLen;
    } else {
        ++count_;
    }
    x.nameOff = (uint32_t)pool_.size();
    pool_.insert(pool_.end(), name, name + n);
    x.nameLen = (uint16_t)n;
    x.parent = parent;
    x.flags = fl;
}

void VolumeIndex::Remove(uint64_t frn) {
    const uint64_t rec = Rec(frn);
    if (rec >= nodes_.size() || !(nodes_[rec].flags & F_USED)) return;
    garbage_ += nodes_[rec].nameLen;
    nodes_[rec] = Node{};
    --count_;
}

void VolumeIndex::Apply(const USN_RECORD_V2& r) {
    if (r.Reason & USN_REASON_FILE_DELETE) {
        Remove(r.FileReferenceNumber);
        return;
    }
    // Ancien nom d'un renommage : le nouveau suit dans l'entrée suivante.
    if ((r.Reason & USN_REASON_RENAME_OLD_NAME) && !(r.Reason & USN_REASON_RENAME_NEW_NAME)) return;
    const auto* name = (const wchar_t*)((const BYTE*)&r + r.FileNameOffset);
    Put(r.FileReferenceNumber, r.ParentFileReferenceNumber, name, r.FileNameLength / sizeof(wchar_t), r.FileAttributes);
}

void VolumeIndex::Compact() {
    if (garbage_ < (1u << 20) || garbage_ * 2 < pool_.size()) return;
    std::vector<wchar_t> pool;
    pool.reserve(pool_.size() - garbage_ + 1024);
    for (Node& x : nodes_) {
        if (!(x.flags & F_USED)) continue;
        const uint32_t off = (uint32_t)pool.size();
        pool.insert(pool.end(), pool_.begin() + x.nameOff, pool_.begin() + x.nameOff + x.nameLen);
        x.nameOff = off;
    }
    pool_ = std::move(pool);
    garbage_ = 0;
}

bool VolumeIndex::BuildPath(uint32_t rec, uint64_t stop, const IndexQuery* q, std::wstring& out) const {
    uint32_t chain[512];
    int depth = 0;
    for (uint32_t cur = rec;;) {
        if (cur >= nodes_.size() || !(nodes_[cur].flags & F_USED)) return false;  // parent inconnu
        if (stop != kNoScope && cur == stop) break;
        if (cur == kRootRec) {
            if (stop != kNoScope) return false;  // hors du dossier demandé
            break;
        }
        const Node& x = nodes_[cur];
        if (q && !Shown(x.flags, *q)) return false;  // masqué, ou dans un dossier masqué
        if (depth == (int)std::size(chain)) return false;   // boucle (index incohérent)
        chain[depth++] = cur;
        cur = x.parent;
    }
    out.clear();
    if (stop == kNoScope) {
        out += letter_;
        out += L":\\";
    }
    for (int i = depth - 1; i >= 0; --i) {
        const Node& x = nodes_[chain[i]];
        out.append(pool_.data() + x.nameOff, x.nameLen);
        if (i) out += L'\\';
    }
    return true;
}

std::wstring VolumeIndex::PathOf(uint64_t rec) const {
    std::wstring s;
    if (rec >= nodes_.size() || !BuildPath((uint32_t)rec, kNoScope, nullptr, s)) s.clear();
    return s;
}

void VolumeIndex::Search(const NameMatcher& m, const IndexQuery& q, uint64_t scopeRec, std::vector<IndexHit>& out,
                         bool& truncated) const {
    const size_t N = nodes_.size();
    unsigned T = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (N < 100000) T = 1;
    std::atomic<size_t> found{0};
    std::atomic<bool> full{false};
    std::vector<std::vector<IndexHit>> parts(T);
    auto work = [&](unsigned t) {
        const size_t b = N * t / T, e = N * (t + 1) / T;
        std::wstring path;
        for (size_t i = b; i < e; ++i) {
            const Node& x = nodes_[i];
            if (!(x.flags & F_USED) || !x.nameLen || !m(pool_.data() + x.nameOff, x.nameLen)) continue;
            if (!BuildPath((uint32_t)i, scopeRec, &q, path) || path.empty()) continue;
            if (found.fetch_add(1, std::memory_order_relaxed) >= q.max) {
                full = true;
                return;
            }
            if (full.load(std::memory_order_relaxed)) return;
            uint32_t attr = 0;
            if (x.flags & F_DIR) attr |= FILE_ATTRIBUTE_DIRECTORY;
            if (x.flags & F_HIDDEN) attr |= FILE_ATTRIBUTE_HIDDEN;
            if (x.flags & F_SYSTEM) attr |= FILE_ATTRIBUTE_SYSTEM;
            if (x.flags & F_REPARSE) attr |= FILE_ATTRIBUTE_REPARSE_POINT;
            parts[t].push_back({path, attr});
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < T; ++t) threads.emplace_back(work, t);
    work(0);
    for (auto& th : threads) th.join();
    for (auto& p : parts)
        for (auto& h : p) {
            if (out.size() >= q.max) {
                full = true;
                break;
            }
            out.push_back(std::move(h));
        }
    truncated = truncated || full.load();
}

// ───────────────────────────── Protocole ─────────────────────────────

namespace {
void PutU32(std::vector<BYTE>& v, uint32_t x) { v.insert(v.end(), (BYTE*)&x, (BYTE*)&x + 4); }
void PutStr(std::vector<BYTE>& v, const std::wstring& s) {
    v.insert(v.end(), (const BYTE*)s.data(), (const BYTE*)(s.data() + s.size()));
}
uint32_t GetU32(const BYTE* p) {
    uint32_t x;
    memcpy(&x, p, 4);
    return x;
}
bool ReadAll(HANDLE h, void* buf, DWORD n) {
    BYTE* p = (BYTE*)buf;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(h, p, n, &r, nullptr) || !r) return false;
        p += r;
        n -= r;
    }
    return true;
}
bool WriteAll(HANDLE h, const void* buf, size_t n) {
    const BYTE* p = (const BYTE*)buf;
    while (n) {
        DWORD w = 0;
        const DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 20);
        if (!WriteFile(h, p, chunk, &w, nullptr) || !w) return false;
        p += w;
        n -= w;
    }
    return true;
}
}  // namespace

std::vector<BYTE> Index_EncodeQuery(const IndexQuery& q) {
    std::vector<BYTE> v;
    PutU32(v, kQueryMagic);
    PutU32(v, kVersion);
    PutU32(v, (q.showHidden ? 1u : 0u) | (q.showSuperHidden ? 2u : 0u));
    PutU32(v, q.max);
    PutU32(v, (uint32_t)q.query.size());
    PutU32(v, (uint32_t)q.scope.size());
    PutStr(v, q.query);
    PutStr(v, q.scope);
    return v;
}

bool Index_DecodeQuery(const BYTE* p, size_t n, IndexQuery& q) {
    if (n < 24 || GetU32(p) != kQueryMagic || GetU32(p + 4) != kVersion) return false;
    const uint32_t flags = GetU32(p + 8), max = GetU32(p + 12), ql = GetU32(p + 16), sl = GetU32(p + 20);
    if (ql == 0 || ql > kIndexMaxQuery || sl > kIndexMaxScope) return false;
    if (n != 24 + ((size_t)ql + sl) * sizeof(wchar_t)) return false;
    const wchar_t* s = (const wchar_t*)(p + 24);
    q.query.assign(s, ql);
    q.scope.assign(s + ql, sl);
    q.showHidden = (flags & 1) != 0;
    q.showSuperHidden = (flags & 2) != 0;
    q.max = std::clamp<uint32_t>(max, 1, (uint32_t)kSearchMaxResults);
    return q.query.find(L'\0') == std::wstring::npos && q.scope.find(L'\0') == std::wstring::npos;
}

// ───────────────────────────── Service ─────────────────────────────

namespace {

struct Volume {
    explicit Volume(wchar_t l) : idx(l) {}
    VolumeIndex idx;
    DWORD serial = 0;
    std::atomic<bool> ready{false};
};
std::vector<std::unique_ptr<Volume>> g_vols;  // fixé au démarrage du service

// Lit la table des fichiers du volume, puis suit son journal. Recommence de
// zéro si le journal a été effacé ou a débordé.
void IndexVolume(Volume* v) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const wchar_t root[] = {v->idx.letter(), L':', L'\\', 0};
    const wchar_t dev[] = {L'\\', L'\\', L'.', L'\\', v->idx.letter(), L':', 0};
    std::vector<BYTE> buf(1u << 20);
    for (int attempt = 0;; ++attempt) {
        if (attempt) Sleep(std::min(60000, 1000 * attempt));
        HANDLE h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD br = 0;
        USN_JOURNAL_DATA_V0 jd{};
        if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr)) {
            CREATE_USN_JOURNAL_DATA c{32ull << 20, 4ull << 20};  // journal absent : on le crée (comme Everything)
            DeviceIoControl(h, FSCTL_CREATE_USN_JOURNAL, &c, sizeof c, nullptr, 0, &br, nullptr);
            if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof jd, &br, nullptr)) {
                CloseHandle(h);
                continue;
            }
        }
        GetVolumeInformationW(root, nullptr, 0, &v->serial, nullptr, nullptr, nullptr, 0);
        v->ready = false;
        {
            std::unique_lock lock(v->idx.lock());
            v->idx.Clear();
            MFT_ENUM_DATA_V0 med{0, 0, jd.NextUsn};
            while (DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &med, sizeof med, buf.data(), (DWORD)buf.size(), &br, nullptr) &&
                   br > sizeof(USN)) {
                for (DWORD off = sizeof(USN); off + sizeof(USN_RECORD_V2) <= br;) {
                    const auto* r = (const USN_RECORD_V2*)(buf.data() + off);
                    if (!r->RecordLength || off + r->RecordLength > br) break;
                    if (r->MajorVersion == 2) v->idx.Apply(*r);
                    off += r->RecordLength;
                }
                med.StartFileReferenceNumber = *(const DWORDLONG*)buf.data();
            }
            v->idx.Put(VolumeIndex::kRootRec, VolumeIndex::kRootRec, L"", 0, FILE_ATTRIBUTE_DIRECTORY);
        }
        v->ready = true;
        // Suivi des changements : chaque appel attend au moins une entrée.
        READ_USN_JOURNAL_DATA_V0 rd{jd.NextUsn, 0xFFFFFFFF, FALSE, 0, 1, jd.UsnJournalID};
        while (DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &rd, sizeof rd, buf.data(), (DWORD)buf.size(), &br, nullptr)) {
            if (br < sizeof(USN)) continue;
            {
                std::unique_lock lock(v->idx.lock());
                for (DWORD off = sizeof(USN); off + sizeof(USN_RECORD_V2) <= br;) {
                    const auto* r = (const USN_RECORD_V2*)(buf.data() + off);
                    if (!r->RecordLength || off + r->RecordLength > br) break;
                    if (r->MajorVersion == 2) v->idx.Apply(*r);
                    off += r->RecordLength;
                }
                v->idx.Compact();
            }
            rd.StartUsn = *(const USN*)buf.data();
        }
        CloseHandle(h);  // journal effacé, débordé, volume démonté… : on reconstruit
        attempt = 0;
    }
}

// Volumes NTFS locaux (fixes) présents au démarrage.
void StartIndexing() {
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[32] = {};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 32) || wcscmp(fs, L"NTFS") != 0)
            continue;
        g_vols.push_back(std::make_unique<Volume>(root[0]));
    }
    for (auto& v : g_vols) std::thread(IndexVolume, v.get()).detach();
}

IndexStatus RunQuery(const IndexQuery& q, std::vector<IndexHit>& hits, bool& truncated) {
    const NameMatcher m(q.query);
    if (m.terms.empty()) return IndexStatus::BadRequest;
    if (q.scope.empty()) {  // tout le PC
        bool any = false, building = false;
        for (auto& v : g_vols) {
            if (!v->ready) { building = true; continue; }
            std::shared_lock lock(v->idx.lock());
            v->idx.Search(m, q, VolumeIndex::kNoScope, hits, truncated);
            any = true;
        }
        return any ? IndexStatus::Ok : building ? IndexStatus::Building : IndexStatus::NotIndexed;
    }
    if (q.scope.size() < 2 || q.scope[1] != L':') return IndexStatus::NotIndexed;
    Volume* vol = nullptr;
    for (auto& v : g_vols)
        if (v->idx.letter() == towupper(q.scope[0])) vol = v.get();
    if (!vol) return IndexStatus::NotIndexed;
    if (!vol->ready) return IndexStatus::Building;
    // Enregistrement du dossier : on l'ouvre (il peut être une jonction vers un autre volume).
    HANDLE h = CreateFileW(ToNtPath(q.scope).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return IndexStatus::NotIndexed;
    BY_HANDLE_FILE_INFORMATION fi{};
    const bool ok = GetFileInformationByHandle(h, &fi) != 0;
    CloseHandle(h);
    if (!ok || fi.dwVolumeSerialNumber != vol->serial || !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return IndexStatus::NotIndexed;
    const uint64_t rec = VolumeIndex::Rec(((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow);
    std::shared_lock lock(vol->idx.lock());
    vol->idx.Search(m, q, rec, hits, truncated);
    return IndexStatus::Ok;
}

void Serve(HANDLE pipe) {
    uint32_t len = 0;
    std::vector<BYTE> req;
    IndexQuery q;
    std::vector<IndexHit> hits;
    bool truncated = false;
    IndexStatus st = IndexStatus::BadRequest;
    if (ReadAll(pipe, &len, 4) && len <= kMaxRequest) {
        req.resize(len);
        if (ReadAll(pipe, req.data(), len) && Index_DecodeQuery(req.data(), len, q)) st = RunQuery(q, hits, truncated);
    }
    std::vector<BYTE> out;
    size_t chars = 0;
    for (auto& h : hits) chars += h.path.size();
    out.reserve(16 + hits.size() * 8 + chars * 2);
    PutU32(out, kReplyMagic);
    PutU32(out, (uint32_t)st);
    PutU32(out, truncated ? 1 : 0);
    PutU32(out, (uint32_t)hits.size());
    for (auto& h : hits) {
        PutU32(out, h.attr);
        PutU32(out, (uint32_t)h.path.size());
        PutStr(out, h.path);
    }
    WriteAll(pipe, out.data(), out.size());
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

std::wstring AllowedSid() {
    wchar_t sid[256] = {};
    DWORD cb = sizeof sid;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSvcParams, L"AllowedSid", RRF_RT_REG_SZ, nullptr, sid, &cb) != ERROR_SUCCESS)
        return L"";
    return sid;
}

// Canal réservé à l'utilisateur qui a installé le service (lecture + écriture
// de données, sans droit de créer une instance du canal), plus SYSTEM et les
// administrateurs ; refuse les clients distants.
void PipeServer() {
    const std::wstring sid = AllowedSid();
    PSID check = nullptr;
    if (sid.empty() || !ConvertStringSidToSidW(sid.c_str(), &check)) return;
    LocalFree(check);
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0012008B;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return;
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    bool first = true;
    for (;;) {
        HANDLE p = CreateNamedPipeW(kPipe, PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1u << 16, 1u << 16, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {  // nom déjà pris par un autre process : on attend qu'il le libère
            Sleep(1000);
            continue;
        }
        first = false;
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) std::thread(Serve, p).detach();
        else CloseHandle(p);
    }
}

SERVICE_STATUS_HANDLE g_ssh = nullptr;
SERVICE_STATUS g_ss{SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 0, 0};

void WINAPI ServiceHandler(DWORD ctl) {
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) {
        g_ss.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_ssh, &g_ss);
        ExitProcess(0);  // rien à enregistrer : l'index est en mémoire
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_ssh = RegisterServiceCtrlHandlerW(kSvc, ServiceHandler);
    if (!g_ssh) return;
    g_ss.dwCurrentState = SERVICE_RUNNING;
    g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_ssh, &g_ss);
    StartIndexing();
    PipeServer();  // ne rend la main qu'en cas d'erreur de configuration
    g_ss.dwCurrentState = SERVICE_STOPPED;
    g_ss.dwWin32ExitCode = ERROR_INVALID_DATA;
    SetServiceStatus(g_ssh, &g_ss);
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    ScHandle(SC_HANDLE x) : h(x) {}
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    operator SC_HANDLE() const { return h; }
};

void StopAndWait(SC_HANDLE svc) {
    SERVICE_STATUS s{};
    ControlService(svc, SERVICE_CONTROL_STOP, &s);
    for (int i = 0; i < 100; ++i) {
        if (!QueryServiceStatus(svc, &s) || s.dwCurrentState == SERVICE_STOPPED) return;
        Sleep(100);
    }
}

std::wstring InstallDir() {
    PWSTR pf = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf))) dir = Join(pf, L"FastExplorer");
    CoTaskMemFree(pf);
    return dir;
}

DWORD ServicePid() {
    ScHandle scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    ScHandle svc = OpenServiceW(scm, kSvc, SERVICE_QUERY_STATUS);
    if (!svc) return 0;
    SERVICE_STATUS_PROCESS sp{};
    DWORD n = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE*)&sp, sizeof sp, &n)) return 0;
    return sp.dwCurrentState == SERVICE_RUNNING ? sp.dwProcessId : 0;
}

}  // namespace

int IndexService_Run() {
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)kSvc, ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : (int)GetLastError();
}

int IndexService_Install(const std::wstring& userSid) {
    PSID check = nullptr;
    if (!ConvertStringSidToSidW(userSid.c_str(), &check)) return ERROR_INVALID_SID;
    LocalFree(check);
    ScHandle scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return (int)GetLastError();
    ScHandle existing = OpenServiceW(scm, kSvc, SERVICE_ALL_ACCESS);
    if (existing) StopAndWait(existing);  // mise à jour : le fichier doit être libre

    // Copie dans Program Files (modifiable seulement par les administrateurs).
    const std::wstring dir = InstallDir();
    if (dir.empty()) return ERROR_PATH_NOT_FOUND;
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return (int)GetLastError();
    const std::wstring exe = Join(dir, L"FastExplorer.exe");
    if (_wcsicmp(self, exe.c_str()) != 0) {
        BOOL copied = FALSE;
        for (int i = 0; i < 50 && !(copied = CopyFileW(self, exe.c_str(), FALSE)); ++i) Sleep(100);
        if (!copied) return (int)GetLastError();
        CopyFileW((std::wstring(self) + L".manifest").c_str(), (exe + L".manifest").c_str(), FALSE);
    }

    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSvcParams, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return ERROR_CANTWRITE;
    RegSetValueExW(k, L"AllowedSid", 0, REG_SZ, (const BYTE*)userSid.c_str(), (DWORD)((userSid.size() + 1) * 2));
    RegCloseKey(k);

    const std::wstring cmd = L"\"" + exe + L"\" --service";
    SC_HANDLE svc = existing;
    ScHandle created = nullptr;
    if (svc) {
        if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return (int)GetLastError();
    } else {
        created.h = CreateServiceW(scm, kSvc, L"FastExplorer — index de recherche", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                   nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!created) return (int)GetLastError();
        svc = created;
    }
    SERVICE_DESCRIPTIONW desc{(LPWSTR)L"Tient à jour la liste des fichiers des disques NTFS pour la recherche "
                                       L"instantanée de FastExplorer. Ne modifie aucun fichier."};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION restart[2] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW fa{86400, nullptr, nullptr, 2, restart};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return (int)GetLastError();
    return 0;
}

int IndexService_Uninstall() {
    ScHandle scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return (int)GetLastError();
    ScHandle svc = OpenServiceW(scm, kSvc, SERVICE_ALL_ACCESS);
    if (svc) {
        StopAndWait(svc);
        if (!DeleteService(svc) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) return (int)GetLastError();
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSvcParams);
    const std::wstring dir = InstallDir();
    if (!dir.empty()) {
        for (const wchar_t* f : {L"FastExplorer.exe", L"FastExplorer.exe.manifest"}) {
            const std::wstring p = Join(dir, f);
            if (!DeleteFileW(p.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        RemoveDirectoryW(dir.c_str());
    }
    return 0;
}

// ───────────────────────────── Client ─────────────────────────────

bool Index_ServiceInstalled() {
    ScHandle scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    ScHandle svc = OpenServiceW(scm, kSvc, SERVICE_QUERY_STATUS);
    return svc.h != nullptr;
}

std::wstring Index_CurrentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    BYTE buf[256];
    DWORD n = 0;
    std::wstring s;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n)) {
        LPWSTR str = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &str)) {
            s = str;
            LocalFree(str);
        }
    }
    CloseHandle(tok);
    return s;
}

IndexStatus Index_Search(const IndexQuery& q, std::vector<IndexHit>& hits, bool& truncated) {
    hits.clear();
    truncated = false;
    if (q.query.empty() || q.query.size() > kIndexMaxQuery || q.scope.size() > kIndexMaxScope) return IndexStatus::BadRequest;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Identification seulement : un faux serveur ne pourrait pas agir en notre nom.
        h = CreateFileW(kPipe, GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING,
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipe, 1000)) break;
    }
    if (h == INVALID_HANDLE_VALUE) return IndexStatus::Unavailable;
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{h};
    // Le canal doit être tenu par notre service, pas par un process qui aurait pris le nom.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid) || pid != ServicePid()) return IndexStatus::Unavailable;
    std::vector<BYTE> req = Index_EncodeQuery(q);
    const uint32_t len = (uint32_t)req.size();
    if (!WriteAll(h, &len, 4) || !WriteAll(h, req.data(), req.size())) return IndexStatus::Unavailable;
    uint32_t head[4];
    if (!ReadAll(h, head, sizeof head) || head[0] != kReplyMagic) return IndexStatus::Unavailable;
    const auto st = (IndexStatus)head[1];
    if (st != IndexStatus::Ok) return st;
    truncated = head[2] != 0;
    const uint32_t count = head[3];
    if (count > q.max) return IndexStatus::Unavailable;
    hits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ah[2];
        if (!ReadAll(h, ah, sizeof ah) || ah[1] == 0 || ah[1] > kIndexMaxScope) return IndexStatus::Unavailable;
        IndexHit hit;
        hit.attr = ah[0];
        hit.path.resize(ah[1]);
        if (!ReadAll(h, hit.path.data(), ah[1] * sizeof(wchar_t))) return IndexStatus::Unavailable;
        hits.push_back(std::move(hit));
    }
    return IndexStatus::Ok;
}
