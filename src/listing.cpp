#include "listing.h"

#include "nt.h"
#include "paths.h"
#include "util.h"
#include "wsearch.h"

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
#include <unordered_set>

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

void AddEntry(DirData& d, const wchar_t* fn, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime, int64_t ctime = 0,
              int64_t atime = 0) {
    Entry e{};
    e.nameOff = (uint32_t)d.pool.size();
    e.nameLen = n;
    e.attr = attr;
    e.icon = -1;
    e.mtime = mtime;
    e.ctime = ctime;
    e.atime = atime;
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
                AddEntry(d, fn, n, di->FileAttributes, (uint64_t)di->EndOfFile.QuadPart, di->LastWriteTime.QuadPart,
                 di->CreationTime.QuadPart, di->LastAccessTime.QuadPart);
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

NameMatcher::NameMatcher(const std::wstring& q) {
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
                std::vector<std::wstring> al;
                std::vector<std::string> sf;
                if (wild.back()) {
                    const std::wstring& w = terms.back();
                    for (size_t b = 0; b <= w.size();) {
                        size_t e = w.find(L';', b);
                        if (e == std::wstring::npos) e = w.size();
                        if (e > b) {
                            std::wstring alt = w.substr(b, e - b);
                            std::string s;  // « *.ext » sans autre joker, en ASCII : ".ext"
                            if (alt.size() > 2 && alt[0] == L'*' && alt[1] == L'.' &&
                                alt.find_first_of(L"*?", 1) == std::wstring::npos)
                                for (size_t k = 1; k < alt.size(); ++k) {
                                    const wchar_t c = alt[k];
                                    if (c >= 0x80) { s.clear(); break; }
                                    s += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
                                }
                            al.push_back(std::move(alt));
                            sf.push_back(std::move(s));
                        }
                        b = e + 1;
                    }
                }
                alts.push_back(std::move(al));
                suffix.push_back(std::move(sf));
            }
            i = j;
        }
}

// Recherche sans casse en ASCII pur : le cas de presque tous les noms, et
// ~50 fois plus rapide que FindNLSStringEx. Renvoie -1 si le nom n'est pas
// en ASCII (il faut alors la comparaison linguistique, accents compris).
int NameMatcher::AsciiFind(const wchar_t* name, uint32_t n, const std::string& term) {
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

bool NameMatcher::operator()(const wchar_t* name, uint32_t n) const {
        if (terms.empty()) return false;
        std::wstring z;  // nom terminé par \0, pour les motifs
        for (size_t t = 0; t < terms.size(); ++t) {
            if (!wild[t] && !ascii[t].empty()) {
                const int r = AsciiFind(name, n, ascii[t]);
                if (r == 0) return false;
                if (r == 1) continue;
            }
            if (wild[t]) {
                bool any = false;
                for (size_t a = 0; a < alts[t].size() && !any; ++a) {
                    const std::string& s = suffix[t][a];
                    if (!s.empty()) {
                        if (s.size() > n) continue;
                        const wchar_t* p = name + n - s.size();
                        size_t k = 0;
                        for (; k < s.size(); ++k) {
                            wchar_t c = p[k];
                            if (c >= 'A' && c <= 'Z') c += 32;
                            if (c != (wchar_t)(unsigned char)s[k]) break;
                        }
                        any = k == s.size();
                    } else {
                        if (z.empty()) z.assign(name, n);
                        any = PathMatchSpecW(z.c_str(), alts[t][a].c_str()) != FALSE;
                    }
                }
                if (!any) return false;
            } else if (FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,
                                       FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, name, (int)n,
                                       terms[t].c_str(), (int)terms[t].size(), nullptr, nullptr, nullptr, 0) < 0) {
                return false;
            }
        }
        return true;
}

namespace {
using Matcher = NameMatcher;

bool PrefixI(const std::wstring& s, const wchar_t* p, std::wstring& rest) {
    const size_t n = wcslen(p);
    if (s.size() <= n || _wcsnicmp(s.c_str(), p, n) != 0) return false;
    rest = s.substr(n);
    return true;
}

// « 10Mo », « 1,5 Go », « 300 » (octets) ; base 1024 comme l'Explorateur.
bool ParseSize(std::wstring s, uint64_t& out) {
    for (auto& c : s) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || v < 0) return false;
    std::wstring u = end;
    for (auto& c : u) c = towlower(c);
    double mul = 1;
    if (u.empty() || u == L"o" || u == L"b") mul = 1;
    else if (u == L"ko" || u == L"kb" || u == L"k") mul = 1024.0;
    else if (u == L"mo" || u == L"mb" || u == L"m") mul = 1024.0 * 1024;
    else if (u == L"go" || u == L"gb" || u == L"g") mul = 1024.0 * 1024 * 1024;
    else if (u == L"to" || u == L"tb" || u == L"t") mul = 1024.0 * 1024 * 1024 * 1024;
    else return false;
    out = (uint64_t)(v * mul + 0.5);
    return true;
}

int64_t ToFileTime(const SYSTEMTIME& local) {
    SYSTEMTIME utc;
    FILETIME ft;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return INT64_MIN;
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

constexpr int64_t kDay = 864000000000ll;  // en unités de 100 ns

// Début (minuit local) du jour « AAAA-MM-JJ ».
bool ParseDay(const std::wstring& s, int64_t& out) {
    SYSTEMTIME st{};
    int y = 0, m = 0, d = 0;
    if (s.find(L'/') != std::wstring::npos) {  // JJ/MM/AAAA (Explorateur en français)
        if (swscanf_s(s.c_str(), L"%d/%d/%d", &d, &m, &y) != 3) return false;
    } else if (swscanf_s(s.c_str(), L"%d-%d-%d", &y, &m, &d) != 3) {
        return false;
    }
    if (y < 1601 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    st.wYear = (WORD)y;
    st.wMonth = (WORD)m;
    st.wDay = (WORD)d;
    out = ToFileTime(st);
    return out != INT64_MIN;
}

bool ParseDate(const std::wstring& s, int64_t now, SearchSpec& spec) {
    std::wstring v = s;
    for (auto& c : v) c = towlower(c);
    FILETIME fnow{(DWORD)now, (DWORD)((uint64_t)now >> 32)}, lnow;
    SYSTEMTIME today;
    FileTimeToLocalFileTime(&fnow, &lnow);
    FileTimeToSystemTime(&lnow, &today);
    today.wHour = today.wMinute = today.wSecond = today.wMilliseconds = 0;
    const int64_t midnight = ToFileTime(today);
    if (v == L"aujourd'hui" || v == L"aujourdhui" || v == L"today") { spec.minTime = midnight; return true; }
    if (v == L"hier" || v == L"yesterday") { spec.minTime = midnight - kDay; spec.maxTime = midnight; return true; }
    if (v == L"semaine" || v == L"week") { spec.minTime = now - 7 * kDay; return true; }
    if (v == L"mois" || v == L"month") { spec.minTime = now - 30 * kDay; return true; }
    if (v == L"année" || v == L"annee" || v == L"year") { spec.minTime = now - 365 * kDay; return true; }
    int64_t a = 0, b = 0;
    if (const size_t dots = v.find(L".."); dots != std::wstring::npos) {
        if (!ParseDay(v.substr(0, dots), a) || !ParseDay(v.substr(dots + 2), b)) return false;
        spec.minTime = a;
        spec.maxTime = b + kDay;  // jour de fin compris
        return true;
    }
    if (v[0] == L'>' || v[0] == L'<') {
        const bool after = v[0] == L'>';
        const size_t skip = v.size() > 1 && v[1] == L'=' ? 2 : 1;
        if (!ParseDay(v.substr(skip), a)) return false;
        if (after) spec.minTime = skip == 2 ? a : a + kDay;  // « >1er janvier » : à partir du 2
        else spec.maxTime = skip == 2 ? a + kDay : a;        // « <1er janvier » : jusqu'au 31 décembre
        return true;
    }
    if (!ParseDay(v, a)) return false;
    spec.minTime = a;
    spec.maxTime = a + kDay;
    return true;
}

bool ParseSizeFilter(const std::wstring& v, SearchSpec& spec) {
    uint64_t a = 0, b = 0;
    if (const size_t dots = v.find(L".."); dots != std::wstring::npos) {
        if (!ParseSize(v.substr(0, dots), a) || !ParseSize(v.substr(dots + 2), b)) return false;
        spec.minSize = a;
        spec.maxSize = b;
        return true;
    }
    if (v.empty()) return false;
    if (v[0] == L'>' || v[0] == L'<') {
        const bool orEq = v.size() > 1 && v[1] == L'=';
        if (!ParseSize(v.substr(orEq ? 2 : 1), a)) return false;
        if (v[0] == L'>') spec.minSize = orEq ? a : a + 1;
        else spec.maxSize = orEq ? a : (a ? a - 1 : 0);
        return true;
    }
    if (!ParseSize(v, a)) return false;  // « taille:10Mo » : au moins
    spec.minSize = a;
    return true;
}
int64_t NowFt() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

// Catégories de type:/kind: (comme l'Explorateur), en extensions.
const wchar_t* KindExts(const std::wstring& v) {
    static const struct { const wchar_t* names; const wchar_t* exts; } kKinds[] = {
        {L"|image|images|picture|pictures|photo|photos|", L"jpg;jpeg;png;gif;bmp;webp;heic;heif;tif;tiff;raw;cr2;nef;arw;dng;avif;jxl;ico;svg"},
        {L"|musique|music|audio|son|", L"mp3;flac;wav;m4a;aac;ogg;oga;wma;opus;aiff;alac;ape;mid;midi"},
        {L"|vidéo|vidéos|video|videos|film|films|movie|", L"mp4;mkv;avi;mov;wmv;webm;m4v;mpg;mpeg;flv;ts;m2ts;3gp"},
        {L"|document|documents|doc|", L"doc;docx;xls;xlsx;ppt;pptx;pdf;txt;odt;ods;odp;rtf;md;csv;epub"},
        {L"|archive|archives|compressé|compresse|compressed|zip|", L"zip;7z;rar;tar;gz;tgz;bz2;xz;cab;iso"},
        {L"|programme|programmes|program|programs|application|applications|app|", L"exe;msi;bat;cmd;ps1;com;appx;msix"},
        {L"|raccourci|raccourcis|link|shortcut|", L"lnk;url"},
    };
    const std::wstring key = L"|" + v + L"|";
    for (const auto& k : kKinds)
        if (wcsstr(k.names, key.c_str())) return k.exts;
    return nullptr;
}

// Tailles nommées de l'Explorateur (size:gigantic) : [min, max].
bool NamedSize(const std::wstring& v, SearchSpec& spec) {
    constexpr uint64_t K = 1024, M = K * K, G = M * K;
    static const struct { const wchar_t* names; uint64_t lo, hi; } kSizes[] = {
        {L"|vide|empty|", 0, 0},
        {L"|minuscule|tiny|", 1, 16 * K - 1},
        {L"|petit|petite|small|", 16 * K, M - 1},
        {L"|moyen|moyenne|medium|", M, 128 * M - 1},
        {L"|grand|grande|large|", 128 * M, G - 1},
        {L"|énorme|enorme|trèsgrand|tresgrand|huge|", G, 4 * G - 1},
        {L"|gigantesque|gigantic|", 4 * G, UINT64_MAX},
    };
    const std::wstring key = L"|" + v + L"|";
    for (const auto& s : kSizes)
        if (wcsstr(s.names, key.c_str())) {
            spec.minSize = s.lo;
            spec.maxSize = s.hi;
            return true;
        }
    return false;
}

// Un filtre (« ext:pdf », « taille:>1Mo »…) ajouté à spec ; false : ce n'en est pas un (ou illisible).
bool ParseFilterToken(const std::wstring& tok, int64_t now, SearchSpec& spec) {
        std::wstring v;
        bool ok = false;
        if (PrefixI(tok, L"ext:", v) || PrefixI(tok, L"extension:", v) || PrefixI(tok, L"fileext:", v)) {
            for (size_t a = 0; a <= v.size();) {
                size_t b = v.find_first_of(L";,|", a);
                if (b == std::wstring::npos) b = v.size();
                std::wstring e = v.substr(a, b - a);
                while (!e.empty() && e[0] == L'.') e.erase(0, 1);
                for (auto& c : e) c = towlower(c);
                if (!e.empty()) spec.exts.push_back(e);
                a = b + 1;
            }
            ok = !spec.exts.empty();
        } else if (PrefixI(tok, L"type:", v) || PrefixI(tok, L"kind:", v) || PrefixI(tok, L"genre:", v)) {
            for (auto& c : v) c = towlower(c);
            if (v == L"dossier" || v == L"dossiers" || v == L"folder" || v == L"d") { spec.kind = 1; ok = true; }
            if (v == L"fichier" || v == L"fichiers" || v == L"file" || v == L"f") { spec.kind = 2; ok = true; }
            if (!ok)
                if (const wchar_t* list = KindExts(v)) {
                    for (const wchar_t* a = list; *a;) {
                        const wchar_t* b = wcschr(a, L';');
                        const size_t len = b ? (size_t)(b - a) : wcslen(a);
                        spec.exts.emplace_back(a, len);
                        a += len + (b ? 1 : 0);
                    }
                    ok = true;
                }
        } else if (PrefixI(tok, L"taille:", v) || PrefixI(tok, L"size:", v)) {
            std::wstring low = v;
            for (auto& c : low) c = towlower(c);
            ok = NamedSize(low, spec) || ParseSizeFilter(v, spec);
        } else if (PrefixI(tok, L"date:", v) || PrefixI(tok, L"modifié:", v) || PrefixI(tok, L"modifie:", v) ||
                   PrefixI(tok, L"modified:", v) || PrefixI(tok, L"datemodified:", v) ||
                   PrefixI(tok, L"datemodification:", v)) {
            ok = ParseDate(v, now, spec);
        }
        return ok;
}

}  // namespace

SearchSpec ParseSearch(const std::wstring& q, int64_t now) {
    if (!now) now = NowFt();
    SearchSpec spec;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && iswspace(q[i])) ++i;
        size_t j = i;
        while (j < q.size() && !iswspace(q[j])) ++j;
        if (j == i) break;
        const std::wstring tok = q.substr(i, j - i);
        i = j;
        if (!ParseFilterToken(tok, now, spec)) {
            if (!spec.names.empty()) spec.names += L' ';
            spec.names += tok;
        }
    }
    return spec;
}

bool SearchSpec::Accept(const wchar_t* name, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime) const {
    const bool dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (kind == 1 && !dir) return false;
    if (kind == 2 && dir) return false;
    if (!exts.empty()) {
        if (dir) return false;
        uint32_t dot = n;
        for (uint32_t k = n; k-- > 0;) {
            if (name[k] == L'\\') break;
            if (name[k] == L'.') { dot = k; break; }
        }
        if (dot == n) return false;
        const uint32_t len = n - dot - 1;
        bool any = false;
        for (const auto& e : exts)
            if (e.size() == len && _wcsnicmp(name + dot + 1, e.c_str(), len) == 0) { any = true; break; }
        if (!any) return false;
    }
    if (sized() && (dir || size < minSize || size > maxSize)) return false;  // dossiers : pas de taille
    if (dated() && (mtime < minTime || mtime >= maxTime)) return false;
    return true;
}

bool NameMatches(const std::wstring& query, const wchar_t* name, uint32_t n) { return Matcher(query)(name, n); }

// ───────────────────────────── Requêtes AQS ─────────────────────────────

struct SearchNode {
    enum Kind { And, Or, Not, Word, Filter, Content } kind = And;
    std::vector<SearchNode> kids;
    std::wstring text;  // Word : mot ou phrase ; Content : texte cherché
    bool phrase = false;
    std::shared_ptr<NameMatcher> matcher;  // Word sans espace
    SearchSpec spec;                       // Filter
};

namespace {

struct QToken {
    std::wstring text;
    bool quoted = false;  // entièrement entre guillemets (« "OR" » reste un mot)
    bool hasQuote = false;
};

std::vector<QToken> Tokenize(const std::wstring& q) {
    std::vector<QToken> out;
    size_t i = 0;
    while (i < q.size()) {
        if (iswspace(q[i])) { ++i; continue; }
        if (q[i] == L'(' || q[i] == L')') {
            out.push_back({std::wstring(1, q[i]), false, false});
            ++i;
            continue;
        }
        QToken t;
        const bool startQuote = q[i] == L'"';
        while (i < q.size() && !iswspace(q[i]) && q[i] != L'(' && q[i] != L')') {
            if (q[i] == L'"') {  // jusqu'au guillemet fermant (espaces compris)
                const size_t end = q.find(L'"', i + 1);
                const size_t stop = end == std::wstring::npos ? q.size() : end;
                t.text.append(q, i + 1, stop - i - 1);
                t.hasQuote = true;
                i = end == std::wstring::npos ? q.size() : end + 1;
                continue;
            }
            t.text += q[i++];
        }
        t.quoted = startQuote && (i >= q.size() || iswspace(q[i]) || q[i] == L'(' || q[i] == L')');
        if (!t.text.empty() || t.hasQuote) out.push_back(std::move(t));
    }
    return out;
}

class QParser {
public:
    QParser(std::vector<QToken> toks, int64_t now) : t_(std::move(toks)), now_(now) {}
    SearchNode Parse() {
        SearchNode n = ParseOr();
        while (pos_ < t_.size()) {  // « ) » en trop : ignorée, la suite est lue aussi
            ++pos_;
            SearchNode more = ParseOr();
            SearchNode all;
            all.kind = SearchNode::And;
            all.kids = {std::move(n), std::move(more)};
            n = std::move(all);
        }
        return n;
    }

private:
    bool Is(const wchar_t* op) const {
        return pos_ < t_.size() && !t_[pos_].quoted && !t_[pos_].hasQuote && t_[pos_].text == op;
    }
    SearchNode ParseOr() {
        SearchNode first = ParseAnd();
        if (!Is(L"OR") && !Is(L"OU") && !Is(L"|")) return first;
        SearchNode n;
        n.kind = SearchNode::Or;
        n.kids.push_back(std::move(first));
        while (Is(L"OR") || Is(L"OU") || Is(L"|")) {
            ++pos_;
            n.kids.push_back(ParseAnd());
        }
        return n;
    }
    SearchNode ParseAnd() {
        SearchNode n;
        n.kind = SearchNode::And;
        while (pos_ < t_.size() && !Is(L")") && !Is(L"OR") && !Is(L"OU") && !Is(L"|")) {
            if (Is(L"AND") || Is(L"ET")) {
                ++pos_;
                continue;
            }
            n.kids.push_back(ParseUnary());
        }
        // Termes de contenu en dernier : ils lisent les fichiers, on ne le fait que si le reste correspond.
        std::stable_partition(n.kids.begin(), n.kids.end(), [](const SearchNode& k) { return k.kind != SearchNode::Content; });
        if (n.kids.size() == 1) return std::move(n.kids[0]);
        return n;
    }
    SearchNode ParseUnary() {
        if (Is(L"NOT")) {
            ++pos_;
            SearchNode n;
            n.kind = SearchNode::Not;
            if (pos_ < t_.size()) n.kids.push_back(ParseUnary());
            return n;
        }
        if (Is(L"(")) {
            ++pos_;
            SearchNode inner = ParseOr();
            if (Is(L")")) ++pos_;
            return inner;
        }
        QToken tok = t_[pos_++];
        if (!tok.quoted && tok.text.size() > 1 && tok.text[0] == L'-') {  // « -mot » : sans ce mot
            tok.text.erase(0, 1);
            SearchNode n;
            n.kind = SearchNode::Not;
            n.kids.push_back(Atom(tok));
            return n;
        }
        return Atom(tok);
    }
    SearchNode Atom(const QToken& tok) {
        SearchNode n;
        std::wstring v;
        if (!tok.quoted) {
            if (PrefixI(tok.text, L"contenu:", v) || PrefixI(tok.text, L"content:", v) ||
                PrefixI(tok.text, L"contient:", v) || PrefixI(tok.text, L"contains:", v)) {
                n.kind = SearchNode::Content;
                n.text = v;
                return n;
            }
            if (PrefixI(tok.text, L"nom:", v) || PrefixI(tok.text, L"name:", v) || PrefixI(tok.text, L"filename:", v))
                return WordNode(v, tok.hasQuote);
            SearchSpec s;
            if (ParseFilterToken(tok.text, now_, s)) {
                n.kind = SearchNode::Filter;
                n.spec = std::move(s);
                return n;
            }
        }
        return WordNode(tok.text, tok.hasQuote);
    }
    static SearchNode WordNode(const std::wstring& w, bool quoted) {
        SearchNode n;
        n.kind = SearchNode::Word;
        n.text = w;
        n.phrase = quoted && w.find_first_of(L" \t") != std::wstring::npos;
        if (!n.phrase) n.matcher = std::make_shared<NameMatcher>(w);
        return n;
    }

    std::vector<QToken> t_;
    size_t pos_ = 0;
    int64_t now_;
};

void Walk(const SearchNode& n, bool& stat, std::vector<std::wstring>& content) {
    if (n.kind == SearchNode::Filter && (n.spec.sized() || n.spec.dated())) stat = true;
    if (n.kind == SearchNode::Content && !n.text.empty() &&
        std::find(content.begin(), content.end(), n.text) == content.end())
        content.push_back(n.text);
    for (const auto& k : n.kids) Walk(k, stat, content);
}

bool Eval(const SearchNode& n, const wchar_t* name, uint32_t len, uint32_t leaf, uint32_t attr, uint64_t size,
          int64_t mtime, const ContentFn* content) {
    switch (n.kind) {
        case SearchNode::And:
            for (const auto& k : n.kids)
                if (!Eval(k, name, len, leaf, attr, size, mtime, content)) return false;
            return true;
        case SearchNode::Or:
            for (const auto& k : n.kids)
                if (Eval(k, name, len, leaf, attr, size, mtime, content)) return true;
            return false;
        case SearchNode::Not:
            return n.kids.empty() || !Eval(n.kids[0], name, len, leaf, attr, size, mtime, content);
        case SearchNode::Word:
            if (n.text.empty()) return true;
            if (n.matcher) return n.matcher->terms.empty() || (*n.matcher)(name + leaf, len - leaf);
            return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                                   name + leaf, (int)(len - leaf), n.text.c_str(), (int)n.text.size(), nullptr, nullptr,
                                   nullptr, 0) >= 0;
        case SearchNode::Filter:
            return n.spec.Accept(name, len, attr, size, mtime);
        case SearchNode::Content:
            if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
            return !content || n.text.empty() || (*content)(n.text);
    }
    return false;
}

// Branches OU de premier niveau, chacune réduite à ce que l'index des noms sait chercher.
bool BranchAsks(const SearchNode& b, std::vector<std::wstring>& out) {
    std::wstring words;
    std::vector<std::wstring> exts;
    auto take = [&](const SearchNode& k) {
        if (k.kind == SearchNode::Word && !k.text.empty()) words += (words.empty() ? L"" : L" ") + k.text;
        if (k.kind == SearchNode::Filter && !k.spec.exts.empty() && exts.empty()) exts = k.spec.exts;
    };
    if (b.kind == SearchNode::And)
        for (const auto& k : b.kids) take(k);
    else
        take(b);
    if (!words.empty()) {
        out.push_back(words);
        return true;
    }
    if (exts.empty()) return false;
    std::wstring ask;  // « *.jpg;*.png;… » : une seule passe sur l'index
    for (const auto& e : exts) {
        if (ask.size() + e.size() + 3 > 1000) {
            out.push_back(std::move(ask));
            ask.clear();
        }
        ask += (ask.empty() ? L"*." : L";*.") + e;
    }
    if (!ask.empty()) out.push_back(std::move(ask));
    return true;
}

}  // namespace

SearchQuery::SearchQuery(const std::wstring& q, int64_t now) {
    auto root = std::make_shared<SearchNode>(QParser(Tokenize(q), now ? now : NowFt()).Parse());
    empty_ = root->kind == SearchNode::And && root->kids.empty();
    Walk(*root, stat_, content_);
    root_ = std::move(root);
}

bool SearchQuery::Match(const wchar_t* name, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime,
                        const ContentFn* content) const {
    if (empty_) return true;
    uint32_t leaf = n;
    while (leaf > 0 && name[leaf - 1] != L'\\') --leaf;
    return Eval(*root_, name, n, leaf, attr, size, mtime, content);
}

std::vector<std::wstring> SearchQuery::NameAsks() const {
    std::vector<std::wstring> out;
    if (empty_) return out;
    if (root_->kind == SearchNode::Or) {
        for (const auto& b : root_->kids)
            if (!BranchAsks(b, out)) return {};
        return out;
    }
    if (!BranchAsks(*root_, out)) return {};
    return out;
}

bool FileContainsText(const std::wstring& path, const std::wstring& word) {
    if (word.empty()) return true;
    HANDLE h = CreateFileW(ToNtPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (32ll << 20)) {
        CloseHandle(h);
        return false;
    }
    std::string bytes((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    const bool ok = ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    bytes.resize(got);
    std::wstring text;
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE) {  // UTF-16 LE
        text.assign((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else {
        // Binaire (NUL dans les premiers Ko) : pas du texte.
        if (memchr(bytes.data(), 0, std::min<size_t>(bytes.size(), 4096))) return false;
        size_t off = bytes.size() >= 3 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB && (uint8_t)bytes[2] == 0xBF ? 3 : 0;
        const char* src = bytes.data() + off;
        const int n = (int)(bytes.size() - off);
        UINT cp = CP_UTF8;
        int wn = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, src, n, nullptr, 0);
        if (wn <= 0) {
            cp = CP_ACP;
            wn = MultiByteToWideChar(cp, 0, src, n, nullptr, 0);
        }
        if (wn <= 0) return false;
        text.resize(wn);
        MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, src, n, text.data(), wn);
    }
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                           text.c_str(), (int)text.size(), word.c_str(), (int)word.size(), nullptr, nullptr, nullptr, 0) >= 0;
}

void DirData_Add(DirData& d, const wchar_t* name, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime, int64_t ctime,
                 int64_t atime) {
    AddEntry(d, name, n, attr, size, mtime, ctime, atime);
}

void DirData_BuildKeys(DirData& d) { BuildSortKeys(d); }

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
    const SearchQuery sq(query);
    if (sq.empty()) { d.error = ERROR_INVALID_PARAMETER; return; }
    const std::wstring root = d.path;
    // contenu: : l'index de Windows Search s'il couvre le dossier, sinon lecture des fichiers texte.
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> indexed;
    if (sq.hasContent()) {
        d.contentIndexed = WSearch_Indexed(root) == 1;
        if (d.contentIndexed)
            for (const auto& w : sq.contentWords()) {
                std::unordered_set<std::wstring> set;
                if (WSearch_Contains(root, w, set)) indexed.emplace(w, std::move(set));
                else d.contentIndexed = false;
            }
    }
    auto match = [&](const std::wstring& rel, const wchar_t* fn, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime) {
        if (!sq.hasContent()) return sq.Match(fn, n, attr, size, mtime);
        if (!sq.Match(fn, n, attr, size, mtime)) return false;  // contenu tenu pour vrai : le reste d'abord
        const std::wstring full = Join(rel.empty() ? root : Join(root, rel.c_str()), std::wstring(fn, n).c_str());
        std::wstring low;
        const ContentFn cf = [&](const std::wstring& w) {
            if (auto it = indexed.find(w); it != indexed.end()) {
                if (low.empty()) {
                    low = full;
                    CharLowerBuffW(low.data(), (DWORD)low.size());
                }
                return it->second.count(low) > 0;
            }
            return FileContainsText(full, w);
        };
        return sq.Match(fn, n, attr, size, mtime, &cf);
    };
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
        struct Hit { std::wstring rel; uint32_t attr; uint64_t size; int64_t mtime, ctime, atime; };
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
                            const bool hit = match(rel, fn, n, attr, (uint64_t)di->EndOfFile.QuadPart, di->LastWriteTime.QuadPart);
                            if (hit || descend) {  // chemin construit seulement si utile
                                std::wstring child = rel.empty() ? std::wstring(fn, n) : rel + L"\\" + std::wstring(fn, n);
                                if (hit)
                                    hits.push_back({child, attr, (uint64_t)di->EndOfFile.QuadPart, di->LastWriteTime.QuadPart,
                                    di->CreationTime.QuadPart, di->LastAccessTime.QuadPart});
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
                AddEntry(acc, x.rel.c_str(), (uint32_t)x.rel.size(), x.attr, x.size, x.mtime, x.ctime, x.atime);
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

std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc, const ViewPrefs& prefs, const std::wstring& nameFilter,
                                const std::vector<uint64_t>* dirSizes, const PropValues* prop) {
    const uint32_t n = (uint32_t)d.entries.size();
    std::vector<uint32_t> order;
    order.reserve(n);
    const bool filter = !d.isRoot();
    const SearchQuery query(nameFilter);  // le filtre instantané comprend aussi ext:, taille:, OR, NOT…
    const bool byQuery = !query.empty();
    auto keep = [&](uint32_t i) {
        const Entry& e = d.entries[i];
        return (!filter || IsShown(e.attr, prefs)) &&
               (!byQuery || query.Match(d.name(e), e.nameLen, e.attr, e.size, e.mtime));
    };
    for (uint32_t i = 0; i < n; ++i)
        if (d.entries[i].isDir() && keep(i)) order.push_back(i);
    const size_t nDirs = order.size();
    for (uint32_t i = 0; i < n; ++i)
        if (!d.entries[i].isDir() && keep(i)) order.push_back(i);

    const Entry* E = d.entries.data();
    const BYTE* K = d.keys.data();
    const wchar_t* P = d.pool.data();
    const uint64_t* DS = dirSizes && dirSizes->size() == n ? dirSizes->data() : nullptr;
    const PropValues* PV = prop && prop->text.size() == n && prop->num.size() == n ? prop : nullptr;
    const std::wstring* OR = d.origin.size() == n ? d.origin.data() : nullptr;
    auto cmp = [=](uint32_t ia, uint32_t ib) {
        const Entry& a = E[ia];
        const Entry& b = E[ib];
        int c = 0;
        switch (col) {
            case COL_SIZE:
                c = DS && a.isDir() ? Cmp3(DS[ia], DS[ib]) : Cmp3(a.size, b.size);
                break;
            case COL_DATE: c = Cmp3(a.mtime, b.mtime); break;
            case COL_CTIME: c = Cmp3(a.ctime, b.ctime); break;
            case COL_ATIME: c = Cmp3(a.atime, b.atime); break;
            case COL_ATTR: c = Cmp3(a.attr & kShownAttrs, b.attr & kShownAttrs); break;
            case COL_ORIGIN:
                if (OR)
                    c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                        OR[ia].c_str(), (int)OR[ia].size(), OR[ib].c_str(), (int)OR[ib].size(), nullptr,
                                        nullptr, 0) - CSTR_EQUAL;
                break;
            case COL_PROP:
                if (PV) {
                    // Sans valeur : en tête (comme l'Explorateur) ; nombres et dates : par valeur ; sinon texte naturel.
                    const std::wstring &ta = PV->text[ia], &tb = PV->text[ib];
                    const double na = PV->num[ia], nb = PV->num[ib];
                    if (ta.empty() || tb.empty()) c = Cmp3(!ta.empty(), !tb.empty());
                    else if (na == na && nb == nb) c = Cmp3(na, nb);
                    else c = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                                             ta.c_str(), (int)ta.size(), tb.c_str(), (int)tb.size(), nullptr, nullptr, 0) -
                             CSTR_EQUAL;
                }
                break;
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

std::wstring AttrText(uint32_t attr) {
    static const struct { uint32_t bit; wchar_t c; } kLetters[] = {
        {FILE_ATTRIBUTE_READONLY, L'R'},  {FILE_ATTRIBUTE_HIDDEN, L'H'},     {FILE_ATTRIBUTE_SYSTEM, L'S'},
        {FILE_ATTRIBUTE_ARCHIVE, L'A'},   {FILE_ATTRIBUTE_DIRECTORY, L'D'},  {FILE_ATTRIBUTE_COMPRESSED, L'C'},
        {FILE_ATTRIBUTE_ENCRYPTED, L'E'}, {FILE_ATTRIBUTE_TEMPORARY, L'T'},  {FILE_ATTRIBUTE_OFFLINE, L'O'},
        {FILE_ATTRIBUTE_REPARSE_POINT, L'L'}, {FILE_ATTRIBUTE_PINNED, L'P'}, {FILE_ATTRIBUTE_UNPINNED, L'U'},
        {FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, L'M'}};
    std::wstring s;
    for (const auto& l : kLetters)
        if (attr & l.bit) s += l.c;
    return s;
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