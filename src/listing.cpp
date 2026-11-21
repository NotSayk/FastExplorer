#include "listing.h"

#include "nt.h"
#include "paths.h"
#include "util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winnetwk.h>

#pragma comment(lib, "mpr")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "shlwapi")

#include <algorithm>
#include <cstring>
#include <execution>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <unordered_map>

namespace {

constexpr ULONG kEnumBuf = 256 * 1024;
constexpr size_t kParallelKeysMin = 4096;
constexpr size_t kParallelSortMin = 16384;
constexpr DWORD kKeyFlags = LCMAP_SORTKEY | LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS;

std::atomic<Parallel> g_parallel{Parallel::Auto};

bool UseParallel(size_t n, size_t threshold) {
    const Parallel p = g_parallel.load(std::memory_order_relaxed);
    return p == Parallel::Always || (p == Parallel::Auto && n >= threshold);
}

void AddEntry(DirData& d, const wchar_t* fn, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime) {
    Entry e{};
    e.nameOff = (uint32_t)d.pool.size();
    e.nameLen = n;
    e.attr = attr;
    e.icon = -1;
    e.mtime = mtime;
    const bool dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.size = dir ? 0 : size;
    e.extOff = n;
    if (!dir)
        for (uint32_t i = n; i-- > 0 && fn[i] != L'\\';)  // (résultats de recherche : chemin relatif)
            if (fn[i] == L'.') { e.extOff = i; break; }
    d.pool.insert(d.pool.end(), fn, fn + n);
    d.pool.push_back(L'\0');
    d.entries.push_back(e);
    if (dir) ++d.nDirs; else ++d.nFiles;
}

void EnumerateDir(DirData& d, const CancelToken& ct) {
    HANDLE h = CreateFileW(ToNtPath(d.path).c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        d.error = ct.cancelled() ? ERROR_CANCELLED : GetLastError();
        return;
    }
    // FILE_LIST_DIRECTORY == FILE_READ_DATA : CreateFileW ouvre aussi un fichier.
    // On le refuse explicitement, pour que l'appelant puisse l'ouvrir à la place.
    FILE_BASIC_INFO bi{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi) &&
        !(bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        d.error = ERROR_DIRECTORY;
        CloseHandle(h);
        return;
    }

    std::unique_ptr<BYTE[]> buf(new BYTE[kEnumBuf]);  // new garantit l'alignement 16 sur x64
    d.entries.reserve(256);
    d.pool.reserve(8192);
    IO_STATUS_BLOCK iosb{};
    BOOLEAN restart = TRUE;

    for (;;) {
        if (ct.cancelled()) { d.error = ERROR_CANCELLED; break; }
        const NTSTATUS st = pNtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf.get(), kEnumBuf,
                                                  FileDirectoryInformation, FALSE, nullptr, restart);
        restart = FALSE;
        if (st == STATUS_NO_MORE_FILES || st == STATUS_NO_SUCH_FILE) break;
        if (!NT_SUCCESS(st)) {
            d.error = ct.cancelled() ? ERROR_CANCELLED : pRtlNtStatusToDosError(st);
            break;
        }
        const BYTE* p = buf.get();
        for (;;) {
            auto* di = (const DirInfo*)p;
            const uint32_t n = di->FileNameLength / sizeof(WCHAR);
            const WCHAR* fn = di->FileName;
            const bool dot = (n == 1 && fn[0] == L'.') || (n == 2 && fn[0] == L'.' && fn[1] == L'.');
            if (!dot)
                AddEntry(d, fn, n, di->FileAttributes, (uint64_t)di->EndOfFile.QuadPart, di->LastWriteTime.QuadPart);
            if (!di->NextEntryOffset) break;
            p += di->NextEntryOffset;
        }
    }
    CloseHandle(h);
}

DriveInfo QueryDrive(const std::wstring& root) {
    DriveInfo di;
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE && root.size() >= 2) {  // « partage (\\serveur) », comme l'Explorateur
        wchar_t remote[512];
        DWORD n = _countof(remote);
        const std::wstring dev = root.substr(0, 2);
        if (WNetGetConnectionW(dev.c_str(), remote, &n) == NO_ERROR) {
            std::wstring r = remote, server, share;
            if (ParentOf(r, server, share) && !share.empty()) di.label = share + L" (" + server + L")";
        }
    }
    wchar_t label[MAX_PATH + 1] = {};
    // SEM_FAILCRITICALERRORS : pas de « Insérez un disque » pour un lecteur vide.
    const UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (di.label.empty() && GetVolumeInformationW(root.c_str(), label, _countof(label), nullptr, nullptr, nullptr,
                                                  nullptr, 0))
        di.label = label;
    ULARGE_INTEGER freeToUs{}, total{}, freeAll{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeToUs, &total, &freeAll)) {
        di.total = total.QuadPart;
        di.free = freeToUs.QuadPart;
    }
    SetErrorMode(old);
    return di;
}

void EnumerateDrives(DirData& d) {
    wchar_t buf[512];
    const DWORD n = GetLogicalDriveStringsW(_countof(buf), buf);
    if (!n || n > _countof(buf)) { d.error = GetLastError(); return; }
    for (const wchar_t* drv = buf; *drv; drv += wcslen(drv) + 1) {
        AddEntry(d, drv, (uint32_t)wcslen(drv), FILE_ATTRIBUTE_DIRECTORY, 0, 0);
        Entry& e = d.entries.back();
        e.extra = GetDriveTypeW(drv);
        // Vraie icône du lecteur : peut interroger le matériel ou le réseau,
        // c'est pour ça qu'on le fait ici et pas dans le thread de l'interface.
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(drv, 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) e.icon = sfi.iIcon;
    }
    std::vector<std::wstring> roots;
    for (const Entry& e : d.entries) roots.emplace_back(d.name(e));
    d.drives = QueryDrives(roots, 400);
}

// Ajoute la clé de tri de `nm` à `out`, renvoie sa longueur en octets.
uint32_t AppendKey(std::vector<BYTE>& out, const wchar_t* nm, int len) {
    BYTE tmp[2048];
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kKeyFlags, nm, len, (LPWSTR)tmp, sizeof tmp, nullptr, nullptr, 0);
    if (n > 0) {
        out.insert(out.end(), tmp, tmp + n);
        return (uint32_t)n;
    }
    n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kKeyFlags, nm, len, nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return 0;
    const size_t at = out.size();
    out.resize(at + n);
    n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, kKeyFlags, nm, len, (LPWSTR)(out.data() + at), n, nullptr, nullptr, 0);
    if (n <= 0) { out.resize(at); return 0; }
    out.resize(at + n);
    return (uint32_t)n;
}

// Clé linguistique + « naturelle » (fichier2 < fichier10), calculée une fois
// par entrée : chaque comparaison du tri n'est ensuite qu'un memcmp.
void BuildSortKeys(DirData& d) {
    const size_t n = d.entries.size();
    unsigned T = 1;
    if (UseParallel(n, kParallelKeysMin)) T = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (n < T) T = n ? (unsigned)n : 1;

    std::vector<std::vector<BYTE>> parts(T);
    auto work = [&](unsigned t) {
        const size_t b = n * t / T, e = n * (t + 1) / T;
        auto& out = parts[t];
        out.reserve((e - b) * 64);
        for (size_t i = b; i < e; ++i) {
            Entry& en = d.entries[i];
            en.keyOff = (uint32_t)out.size();
            en.keyLen = AppendKey(out, d.pool.data() + en.nameOff, (int)en.nameLen);
        }
    };
    if (T == 1) {
        work(0);
        d.keys = std::move(parts[0]);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(T - 1);
    for (unsigned t = 1; t < T; ++t) threads.emplace_back(work, t);
    work(0);
    for (auto& th : threads) th.join();

    size_t total = 0;
    for (auto& p : parts) total += p.size();
    d.keys.resize(total);
    size_t base = 0;
    for (unsigned t = 0; t < T; ++t) {
        if (!parts[t].empty()) memcpy(d.keys.data() + base, parts[t].data(), parts[t].size());
        if (base) {
            const size_t b = n * t / T, e = n * (t + 1) / T;
            for (size_t i = b; i < e; ++i) d.entries[i].keyOff += (uint32_t)base;
        }
        base += parts[t].size();
    }
}

}  // namespace

void SetParallel(Parallel p) { g_parallel.store(p); }

namespace {
struct Matcher {
    std::vector<std::wstring> terms;
    std::vector<bool> wild;
    std::vector<std::string> ascii;  // mot en ASCII minuscule ("" : pas d'ASCII pur)
    explicit Matcher(const std::wstring& q) {
        size_t i = 0;
        while (i < q.size()) {
            while (i < q.size() && iswspace(q[i])) ++i;
            size_t j = i;
            while (j < q.size() && !iswspace(q[j])) ++j;
            if (j > i) {
                terms.push_back(q.substr(i, j - i));
                wild.push_back(terms.back().find_first_of(L"*?") != std::wstring::npos);
                std::string a;
                for (wchar_t c : terms.back()) {
                    if (c >= 0x80) { a.clear(); break; }
                    a += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
                }
                ascii.push_back(std::move(a));
            }
            i = j;
        }
    }
    // Recherche sans casse en ASCII pur : le cas de presque tous les noms, et
    // ~50 fois plus rapide que FindNLSStringEx. Renvoie -1 si le nom n'est pas
    // en ASCII (il faut alors la comparaison linguistique, accents compris).
    static int AsciiFind(const wchar_t* name, uint32_t n, const std::string& term) {
        const size_t m = term.size();
        for (uint32_t i = 0; i < n; ++i)
            if (name[i] >= 0x80) return -1;
        if (m > n) return 0;
        for (uint32_t i = 0; i + m <= n; ++i) {
            size_t k = 0;
            for (; k < m; ++k) {
                wchar_t c = name[i + k];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != (wchar_t)(unsigned char)term[k]) break;
            }
            if (k == m) return 1;
        }
        return 0;
    }

    bool operator()(const wchar_t* name, uint32_t n) const {
        if (terms.empty()) return false;
        std::wstring z;  // nom terminé par \0, pour les motifs
        for (size_t t = 0; t < terms.size(); ++t) {
            if (!wild[t] && !ascii[t].empty()) {
                const int r = AsciiFind(name, n, ascii[t]);
                if (r == 0) return false;
                if (r == 1) continue;
            }
            if (wild[t]) {
                if (z.empty()) z.assign(name, n);
                if (!PathMatchSpecW(z.c_str(), terms[t].c_str())) return false;
            } else if (FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,
                                       FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, name, (int)n,
                                       terms[t].c_str(), (int)terms[t].size(), nullptr, nullptr, nullptr, 0) < 0) {
                return false;
            }
        }
        return true;
    }
};
}  // namespace

bool NameMatches(const std::wstring& query, const wchar_t* name, uint32_t n) { return Matcher(query)(name, n); }

void SearchTree(DirData& d, const std::wstring& query, const ViewPrefs& prefs, const CancelToken& ct,
                const std::function<void(std::shared_ptr<DirData>)>& partial) {
    const double t0 = NowMs();
    d.query = query;
    d.searchDone = false;
    {  // la racine doit être un dossier lisible
        HANDLE h = CreateFileW(ToNtPath(d.path).c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) { d.error = GetLastError(); return; }
        CloseHandle(h);
    }
    const Matcher match(query);
    const std::wstring root = d.path;
    // Disque réseau : moins de requêtes en parallèle (le serveur sert aussi les autres).
    const bool remote = (root.size() > 1 && root[0] == L'\\') ||
                        (root.size() >= 2 && root[1] == L':' && GetDriveTypeW(root.substr(0, 3).c_str()) == DRIVE_REMOTE);
    const unsigned T = remote ? 4u : std::clamp(std::thread::hardware_concurrency(), 2u, 16u);

    std::mutex m;
    std::condition_variable cv;
    std::deque<std::wstring> queue{L""};  // dossiers à lire (relatifs à la racine)
    unsigned busy = 0;
    bool finished = false, changed = false;
    std::atomic<bool> stop{false};
    DirData acc;  // résultats (sous m)

    auto worker = [&] {
        std::unique_ptr<BYTE[]> buf(new BYTE[kEnumBuf]);
        struct Hit { std::wstring rel; uint32_t attr; uint64_t size; int64_t mtime; };
        for (;;) {
            std::wstring rel;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return !queue.empty() || busy == 0 || stop.load() || ct.cancelled(); });
                if (queue.empty() || stop.load() || ct.cancelled()) {
                    finished = true;
                    cv.notify_all();
                    return;
                }
                rel = std::move(queue.front());
                queue.pop_front();
                ++busy;
            }
            std::vector<Hit> hits;
            std::vector<std::wstring> subdirs;
            HANDLE h = CreateFileW(ToNtPath(rel.empty() ? root : Join(root, rel.c_str())).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h != INVALID_HANDLE_VALUE) {  // refusé (dossier protégé) : ignoré, comme l'Explorateur
                IO_STATUS_BLOCK iosb{};
                BOOLEAN restart = TRUE;
                while (!stop.load(std::memory_order_relaxed) && !ct.cancelled()) {
                    const NTSTATUS st = pNtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf.get(), kEnumBuf,
                                                              FileDirectoryInformation, FALSE, nullptr, restart);
                    restart = FALSE;
                    if (!NT_SUCCESS(st) || st == STATUS_NO_MORE_FILES) break;
                    const BYTE* p = buf.get();
                    for (;;) {
                        auto* di = (const DirInfo*)p;
                        const uint32_t n = di->FileNameLength / sizeof(WCHAR);
                        const WCHAR* fn = di->FileName;
                        const bool dot = (n == 1 && fn[0] == L'.') || (n == 2 && fn[0] == L'.' && fn[1] == L'.');
                        const DWORD attr = di->FileAttributes;
                        if (!dot && IsShown(attr, prefs)) {
                            // Pas de jonction ni de lien : pas de boucle, pas de doublon.
                            const bool descend = (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
                            const bool hit = match(fn, n);
                            if (hit || descend) {  // chemin construit seulement si utile
                                std::wstring child = rel.empty() ? std::wstring(fn, n) : rel + L"\\" + std::wstring(fn, n);
                                if (hit)
                                    hits.push_back({child, attr, (uint64_t)di->EndOfFile.QuadPart, di->LastWriteTime.QuadPart});
                                if (descend) subdirs.push_back(std::move(child));
                            }
                        }
                        if (!di->NextEntryOffset) break;
                        p += di->NextEntryOffset;
                    }
                }
                CloseHandle(h);
            }
            std::lock_guard lock(m);
            for (auto& s : subdirs) queue.push_back(std::move(s));
            for (const Hit& x : hits) {
                if (acc.entries.size() >= kSearchMaxResults) {
                    acc.truncated = true;
                    stop = true;
                    break;
                }
                AddEntry(acc, x.rel.c_str(), (uint32_t)x.rel.size(), x.attr, x.size, x.mtime);
            }
            ++acc.scanned;
            changed |= !hits.empty();
            --busy;
            cv.notify_all();
        }
    };

    std::vector<std::thread> threads;
    for (unsigned i = 0; i < T; ++i) threads.emplace_back(worker);
    // Copies partielles pendant la recherche : la liste se remplit au fil de l'eau.
    ULONGLONG nextAt = GetTickCount64() + 120;
    for (;;) {
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(40), [&] { return finished; });
        if (finished) break;
        if (!partial || GetTickCount64() < nextAt) continue;
        nextAt = GetTickCount64() + 250;
        auto snap = std::make_shared<DirData>();
        snap->path = root;
        snap->query = query;
        snap->searchDone = false;
        snap->scanned = acc.scanned;
        if (changed) {
            snap->entries = acc.entries;
            snap->pool = acc.pool;
            snap->nDirs = acc.nDirs;
            snap->nFiles = acc.nFiles;
            changed = false;
        } else {
            continue;  // rien de nouveau : pas de copie
        }
        lock.unlock();
        BuildSortKeys(*snap);
        snap->enumMs = NowMs() - t0;
        if (!ct.cancelled()) partial(std::move(snap));
    }
    for (auto& t : threads) t.join();
    if (ct.cancelled()) { d.error = ERROR_CANCELLED; return; }
    d.entries = std::move(acc.entries);
    d.pool = std::move(acc.pool);
    d.nDirs = acc.nDirs;
    d.nFiles = acc.nFiles;
    d.scanned = acc.scanned;
    d.truncated = acc.truncated;
    d.enumMs = NowMs() - t0;
    const double t1 = NowMs();
    BuildSortKeys(d);
    d.keyMs = NowMs() - t1;
    d.searchDone = true;
}

void Enumerate(DirData& d, const CancelToken& ct) {
    const double t0 = NowMs();
    if (d.isRoot()) EnumerateDrives(d); else EnumerateDir(d, ct);
    const double t1 = NowMs();
    d.enumMs = t1 - t0;
    if (!d.error && !ct.cancelled()) {
        BuildSortKeys(d);
        d.keyMs = NowMs() - t1;
    }
}

std::wstring SelectionText(size_t count, bool anyDir, uint64_t bytes, bool withSize) {
    if (!count) return {};
    wchar_t s[160];
    swprintf_s(s, count > 1 ? L"%zu éléments sélectionnés" : L"%zu élément sélectionné", count);
    std::wstring out = s;
    if (withSize && !anyDir) {
        wchar_t b[64];
        if (SUCCEEDED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, _countof(b))))
            out += std::wstring(L"  ") + b;
    }
    return out;
}

ViewPrefs ReadViewPrefs() {
    SHELLSTATEW ss{};
    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN | SSF_SHOWEXTENSIONS, FALSE);
    ViewPrefs p;
    p.showHidden = ss.fShowAllObjects != 0;
    p.showSuperHidden = ss.fShowSuperHidden != 0;
    p.showExtensions = ss.fShowExtensions != 0;
    return p;
}

void WriteViewPrefs(const ViewPrefs& p) {
    SHELLSTATEW ss{};
    ss.fShowAllObjects = p.showHidden;
    ss.fShowSuperHidden = p.showSuperHidden;
    ss.fShowExtensions = p.showExtensions;
    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN | SSF_SHOWEXTENSIONS, TRUE);
    // Les fenêtres de l'Explorateur se mettent à jour sur ce message ; une
    // application figée ne doit pas nous bloquer : diffusion sur un thread.
    std::thread([] {
        DWORD_PTR r = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ShellState", SMTO_ABORTIFHUNG, 1000, &r);
    }).detach();
}

bool IsShown(DWORD attr, const ViewPrefs& p) {
    if (!(attr & FILE_ATTRIBUTE_HIDDEN)) return true;
    if (!p.showHidden) return false;
    return !(attr & FILE_ATTRIBUTE_SYSTEM) || p.showSuperHidden;
}

namespace {
// Réglage d'un type, lu une fois dans le registre.
enum ExtRule : uint8_t { kExtUnknown, kExtKnown, kExtNever, kExtAlways };

bool HasValue(HKEY root, const std::wstring& key, const wchar_t* value) {
    return RegGetValueW(root, key.c_str(), value, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

ExtRule ReadExtRule(const std::wstring& ext) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return kExtUnknown;
    wchar_t progId[256] = {};
    DWORD cb = sizeof progId;
    const bool hasProg = RegGetValueW(k, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, progId, &cb) == ERROR_SUCCESS && progId[0];
    RegCloseKey(k);
    if (HasValue(HKEY_CLASSES_ROOT, ext, L"AlwaysShowExt")) return kExtAlways;
    if (hasProg) {
        if (HasValue(HKEY_CLASSES_ROOT, progId, L"AlwaysShowExt")) return kExtAlways;
        if (HasValue(HKEY_CLASSES_ROOT, progId, L"NeverShowExt")) return kExtNever;
    }
    return kExtKnown;
}

std::mutex g_extMutex;
std::unordered_map<std::wstring, ExtRule> g_extRules;
}  // namespace

uint32_t HiddenExtLen(const wchar_t* name, uint32_t nameLen, uint32_t extOff, bool showExtensions) {
    // Pas d'extension, ou nom qui n'est qu'une extension (« .gitignore ») : nom complet.
    if (extOff == 0 || extOff + 1 >= nameLen) return 0;
    std::wstring ext(name + extOff, nameLen - extOff);
    CharLowerBuffW(ext.data(), (DWORD)ext.size());
    ExtRule rule;
    {
        std::lock_guard lock(g_extMutex);
        auto it = g_extRules.find(ext);
        if (it != g_extRules.end()) {
            rule = it->second;
        } else {
            rule = ReadExtRule(ext);
            g_extRules.emplace(ext, rule);
        }
    }
    const bool hide = rule == kExtNever || (rule == kExtKnown && !showExtensions);
    return hide ? nameLen - extOff : 0;
}

std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc, const ViewPrefs& prefs) {
    const uint32_t n = (uint32_t)d.entries.size();
    std::vector<uint32_t> order;
    order.reserve(n);
    const bool filter = !d.isRoot();
    for (uint32_t i = 0; i < n; ++i)
        if (d.entries[i].isDir() && (!filter || IsShown(d.entries[i].attr, prefs))) order.push_back(i);
    const size_t nDirs = order.size();
    for (uint32_t i = 0; i < n; ++i)
        if (!d.entries[i].isDir() && (!filter || IsShown(d.entries[i].attr, prefs))) order.push_back(i);

    const Entry* E = d.entries.data();
    const BYTE* K = d.keys.data();
    const wchar_t* P = d.pool.data();
    auto cmp = [=](uint32_t ia, uint32_t ib) {
        const Entry& a = E[ia];
        const Entry& b = E[ib];
        int c = 0;
        switch (col) {
            case COL_SIZE: c = Cmp3(a.size, b.size); break;
            case COL_DATE: c = Cmp3(a.mtime, b.mtime); break;
            case COL_TYPE:
                c = CompareStringOrdinal(P + a.nameOff + a.extOff, (int)(a.nameLen - a.extOff), P + b.nameOff + b.extOff,
                                         (int)(b.nameLen - b.extOff), TRUE) - CSTR_EQUAL;
                break;
        }
        if (!c) {
            const uint32_t m = std::min(a.keyLen, b.keyLen);
            if (m) c = memcmp(K + a.keyOff, K + b.keyOff, m);
            if (!c) c = Cmp3(a.keyLen, b.keyLen);
        }
        if (!c) c = Cmp3(ia, ib);
        return asc ? c < 0 : c > 0;
    };

    const auto mid = order.begin() + (ptrdiff_t)nDirs;
    if (UseParallel(n, kParallelSortMin)) {
        std::sort(std::execution::par, order.begin(), mid, cmp);
        std::sort(std::execution::par, mid, order.end(), cmp);
    } else {
        std::sort(order.begin(), mid, cmp);
        std::sort(mid, order.end(), cmp);
    }
    return order;
}

const wchar_t* DriveTypeText(UINT t) {
    switch (t) {
        case DRIVE_FIXED: return L"Disque local";
        case DRIVE_REMOVABLE: return L"Disque amovible";
        case DRIVE_REMOTE: return L"Lecteur réseau";
        case DRIVE_CDROM: return L"Lecteur CD/DVD";
        case DRIVE_RAMDISK: return L"Disque RAM";
        default: return L"Lecteur";
    }
}

std::wstring DriveDisplayName(const std::wstring& root, UINT type, const DriveInfo& info) {
    const std::wstring letter = root.size() >= 2 ? root.substr(0, 2) : root;
    return (info.label.empty() ? std::wstring(DriveTypeText(type)) : info.label) + L" (" + letter + L")";
}

std::vector<DriveInfo> QueryDrives(const std::vector<std::wstring>& roots, DWORD timeoutMs) {
    // Un thread par lecteur ; ceux qui dépassent le délai finissent seuls
    // (l'état est partagé, aucun ne référence la pile de l'appelant).
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::vector<DriveInfo> out;
        std::vector<bool> done;
        size_t left = 0;
    };
    if (roots.empty()) return {};
    auto s = std::make_shared<Shared>();
    s->out.resize(roots.size());
    s->done.resize(roots.size());
    s->left = roots.size();
    for (size_t i = 0; i < roots.size(); ++i) {
        std::thread([s, i, root = roots[i]] {
            DriveInfo di = QueryDrive(root);
            std::lock_guard<std::mutex> l(s->m);
            s->out[i] = std::move(di);
            s->done[i] = true;
            if (--s->left == 0) s->cv.notify_all();
        }).detach();
    }
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return s->left == 0; });
    std::vector<DriveInfo> r(roots.size());
    for (size_t i = 0; i < roots.size(); ++i)
        if (s->done[i]) r[i] = s->out[i];
    return r;
}
bool IsGoneError(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_READY:             // média retiré (CD, lecteur de cartes)
        case ERROR_DEVICE_NOT_CONNECTED:  // clé USB arrachée
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NETNAME_DELETED:
        case ERROR_UNRECOGNIZED_VOLUME:
        case ERROR_DIRECTORY:             // remplacé par un fichier
        case ERROR_DELETE_PENDING: return true;
        default: return false;
    }
}

void EnumerateNearest(DirData& d, const CancelToken& ct) {
    const std::wstring wanted = d.path;
    Enumerate(d, ct);
    std::wstring parent, child;
    while (d.error && IsGoneError(d.error) && !ct.cancelled() && ParentOf(d.path, parent, child)) {
        const std::wstring p = parent;
        d = DirData{};
        d.path = p;
        d.vanished = wanted;
        Enumerate(d, ct);
    }
}