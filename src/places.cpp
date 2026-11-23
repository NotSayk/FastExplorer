#include "places.h"

#include "fileops.h"
#include "navpane.h"
#include "paths.h"

#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace {

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int64_t FtToI(const FILETIME& f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime); }

void Add(DirData& d, const std::wstring& name, uint32_t attr, uint64_t size, int64_t mtime, const std::wstring& target,
         const std::wstring& origin, int64_t ctime = 0, int64_t atime = 0) {
    DirData_Add(d, name.c_str(), (uint32_t)name.size(), attr, size, mtime, ctime, atime);
    d.target.push_back(target);
    d.origin.push_back(origin);
}

std::wstring LeafOf(const std::wstring& p) {
    std::wstring parent, leaf;
    if (ParentOf(p, parent, leaf) && !leaf.empty()) return leaf;
    return p;
}

std::wstring ParentDir(const std::wstring& p) {
    std::wstring parent, leaf;
    return ParentOf(p, parent, leaf) ? parent : L"";
}

// Chemin local d'un raccourci, lu directement dans le fichier (format MS-SHLLINK :
// en-tête, liste d'ID facultative, puis LinkInfo et son LocalBasePath). Bien plus
// rapide que IShellLink::Load (qui charge les gestionnaires du Shell). false :
// pas de chemin local (partage réseau, élément virtuel) → passer par le Shell.
bool LnkLocalPath(const std::wstring& lnk, std::wstring& out) {
    HANDLE f = CreateFileW(lnk.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BYTE b[8192];
    DWORD n = 0;
    const BOOL ok = ReadFile(f, b, sizeof b, &n, nullptr);
    CloseHandle(f);
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= n ? (uint32_t)(b[o] | (b[o + 1] << 8)) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= n ? u16(o) | (u16(o + 2) << 16) : 0; };
    if (!ok || n < 0x4C || u32(0) != 0x4C) return false;
    const uint32_t flags = u32(0x14);
    size_t at = 0x4C;
    size_t idl = 0, idlSize = 0;  // liste d'ID (PIDL absolu), si présente
    if (flags & 0x1) {  // HasLinkTargetIDList
        idlSize = u16(at);
        idl = at + 2;
        at += 2 + idlSize;
    }
    // Sans LinkInfo locale : le chemin de la liste d'ID (élément du système de fichiers), sans gestionnaire du Shell.
    auto fromIdList = [&] {
        if (!idl || idlSize < 2 || idl + idlSize > n) return false;
        wchar_t p[MAX_PATH * 4];
        if (!SHGetPathFromIDListEx((PCIDLIST_ABSOLUTE)(b + idl), p, (DWORD)std::size(p), GPFIDL_DEFAULT) || !p[0])
            return false;
        out = p;
        return true;
    };
    if (!(flags & 0x2) || at + 0x1C > n) return fromIdList();  // HasLinkInfo
    const uint32_t size = u32(at), header = u32(at + 4), liFlags = u32(at + 8);
    if (at + size > n || size < 0x1C) return fromIdList();
    auto wide = [&](size_t o) {
        std::wstring s;
        for (; o + 1 < at + size; o += 2) {
            const wchar_t c = (wchar_t)u16(o);
            if (!c) break;
            s.push_back(c);
        }
        return s;
    };
    auto ansi = [&](size_t o) {
        size_t e = o;
        while (e < at + size && b[e]) ++e;
        std::wstring s;
        if (e > o) {
            const int m = MultiByteToWideChar(CP_ACP, 0, (const char*)b + o, (int)(e - o), nullptr, 0);
            s.resize(m);
            MultiByteToWideChar(CP_ACP, 0, (const char*)b + o, (int)(e - o), s.data(), m);
        }
        return s;
    };
    std::wstring base, suffix;
    if (!(liFlags & 0x1)) {  // pas de chemin local : partage réseau (CommonNetworkRelativeLink → NetName)
        const size_t cnrl = at + u32(at + 0x14);
        if (!(liFlags & 0x2) || !u32(at + 0x14) || cnrl + 0x14 > at + size) return fromIdList();
        base = ansi(cnrl + u32(cnrl + 8));
        if (u32(at + 0x18)) suffix = ansi(at + u32(at + 0x18));
    } else if (header >= 0x24 && u32(at + 0x1C)) {
        base = wide(at + u32(at + 0x1C));
        if (u32(at + 0x20)) suffix = wide(at + u32(at + 0x20));
    } else {
        base = ansi(at + u32(at + 0x10));
        if (u32(at + 0x18)) suffix = ansi(at + u32(at + 0x18));
    }
    if (base.empty()) return fromIdList();
    if (!suffix.empty()) base += (base.back() == L'\\' ? L"" : L"\\") + suffix;
    out = std::move(base);
    return true;
}

}  // namespace

void Recycle_Enumerate(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    d.path = kBinPath;
    for (const auto& bin : Recycle_Dirs()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((bin + L"$I*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (ct.cancelled()) break;
            std::wstring orig;
            int64_t when = 0;
            uint64_t size = 0;
            if (!Recycle_ReadInfo(bin + fd.cFileName, orig, when, size)) continue;
            std::wstring r = bin + fd.cFileName;
            r[bin.size() + 1] = L'R';
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (!GetFileAttributesExW(r.c_str(), GetFileExInfoStandard, &a)) continue;  // $I orphelin
            Add(d, LeafOf(orig), a.dwFileAttributes, size, when, r, ParentDir(orig), FtToI(a.ftCreationTime),
                FtToI(a.ftLastAccessTime));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

void Home_EnumerateFrom(DirData& d, const std::wstring& recentDir, const std::vector<NavFolder>& pins,
                        const CancelToken& ct, size_t maxRecent) {
    const double t0 = Ms();
    d.path = kHomePath;
    std::unordered_set<std::wstring> seen;
    {
        for (const auto& f : pins) {
            if (f.path.empty() || !seen.insert(Lower(f.path)).second) continue;
            WIN32_FILE_ATTRIBUTE_DATA a{};
            const bool ok = f.path.size() > 1 && f.path[0] == L'\\' && f.path[1] == L'\\'
                                ? false  // partage réseau : pas d'accès disque ici
                                : GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &a) != 0;
            Add(d, f.text.empty() ? LeafOf(f.path) : f.text, ok ? a.dwFileAttributes : FILE_ATTRIBUTE_DIRECTORY, 0,
                ok ? FtToI(a.ftLastWriteTime) : 0, f.path, ParentDir(f.path));
        }
    }

    // Raccourcis du dossier Recent, du plus récent au plus ancien.
    struct Lnk {
        int64_t t;
        std::wstring name;
    };
    std::vector<Lnk> lnks;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((recentDir + L"\\*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) lnks.push_back({FtToI(fd.ftLastWriteTime), fd.cFileName});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(lnks.begin(), lnks.end(), [](const Lnk& a, const Lnk& b) { return a.t > b.t; });

    size_t kept = 0, read = 0;
    for (const auto& l : lnks) {
        if (kept >= maxRecent || ++read > maxRecent * 5 || ct.cancelled()) break;
        const std::wstring lnk = recentDir + L"\\" + l.name;
        std::wstring t;
        // Raccourci sans chemin de fichier (élément virtuel, cible effacée de la liste d'ID) : ignoré.
        if (!LnkLocalPath(lnk, t)) continue;
        if (!seen.insert(Lower(t)).second) continue;
        const bool unc = t.size() > 1 && t[0] == L'\\' && t[1] == L'\\';
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (unc) {
            a.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        } else if (!GetFileAttributesExW(t.c_str(), GetFileExInfoStandard, &a)) {
            continue;  // fichier disparu
        }
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // dossiers récents : pas dans l'Accueil
        Add(d, LeafOf(t), a.dwFileAttributes, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow, l.t, t, ParentDir(t),
            FtToI(a.ftCreationTime), FtToI(a.ftLastAccessTime));
        ++kept;
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

void Home_Enumerate(DirData& d, const std::vector<NavFolder>& pins, const CancelToken& ct, size_t maxRecent) {
    PWSTR recent = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY, nullptr, &recent))) dir = recent;
    CoTaskMemFree(recent);
    Home_EnumerateFrom(d, dir, pins, ct, maxRecent);
}

const wchar_t* Places_Title(const std::wstring& path) {
    if (path == kBinPath) return L"Corbeille";
    if (path == kHomePath) return L"Accueil";
    if (path == L"::net") return L"Réseau";
    if (path == kGalleryPath) return L"Galerie";
    if (path == kLinuxPath) return L"Linux";
    return L"";
}

std::wstring Places_FromAlias(const std::wstring& text) {
    const std::wstring t = Lower(text);
    if (t == L"::bin" || t == L"corbeille" || t == L"recycle bin" || t == L"shell:recyclebinfolder" ||
        t == L"shell:::{645ff040-5081-101b-9f08-00aa002f954e}")
        return kBinPath;
    if (t == L"::home" || t == L"accueil" || t == L"home" || t == L"shell:home" || t == L"accès rapide" ||
        t == L"shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}")
        return kHomePath;
    if (t == L"::net" || t == L"réseau" || t == L"network" || t == L"shell:networkplacesfolder" ||
        t == L"shell:::{f02c1a0d-be21-4350-88b0-7367fc96ef3c}")
        return L"::net";
    if (t == L"::gallery" || t == L"galerie" || t == L"gallery" || t == L"shell:::{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}")
        return kGalleryPath;
    if (t == L"::linux" || t == L"linux" || t == L"wsl" || t == L"\\\\wsl.localhost" || t == L"\\\\wsl$")
        return kLinuxPath;
    return L"";
}

int Places_ShellCommand(const std::wstring& text, std::wstring& path) {
    if (text.size() < 7 || _wcsnicmp(text.c_str(), L"shell:", 6) != 0) return 0;
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(text.c_str(), nullptr, &p, 0, nullptr)) || !p) return -1;
    std::wstring buf(32768, L'\0');
    const bool fs = SHGetPathFromIDListEx(p, buf.data(), (DWORD)buf.size(), GPFIDL_DEFAULT) != FALSE;
    CoTaskMemFree(p);
    if (!fs || !buf[0]) return -2;
    buf.resize(wcslen(buf.c_str()));
    path = std::move(buf);
    return 1;
}

namespace {
constexpr wchar_t kTypedKey[] = L"Software\\FastExplorer";
constexpr wchar_t kTypedValue[] = L"TypedPaths";
constexpr size_t kTypedMax = 25;
}  // namespace

std::vector<std::wstring> Places_Typed() {
    std::vector<std::wstring> out;
    DWORD type = 0, size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kTypedKey, kTypedValue, RRF_RT_REG_MULTI_SZ, &type, nullptr, &size) !=
            ERROR_SUCCESS || size < sizeof(wchar_t))
        return out;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kTypedKey, kTypedValue, RRF_RT_REG_MULTI_SZ, &type, buf.data(), &size) !=
        ERROR_SUCCESS)
        return out;
    for (const wchar_t* s = buf.c_str(); *s && out.size() < kTypedMax; s += wcslen(s) + 1) out.emplace_back(s);
    return out;
}

void Places_RememberTyped(const std::wstring& text) {
    if (text.empty() || text.size() > 2048) return;
    std::vector<std::wstring> all = Places_Typed();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const std::wstring& s) { return _wcsicmp(s.c_str(), text.c_str()) == 0; }),
              all.end());
    all.insert(all.begin(), text);
    if (all.size() > kTypedMax) all.resize(kTypedMax);
    std::wstring multi;
    for (const auto& s : all) multi += s + L'\0';
    multi += L'\0';
    RegSetKeyValueW(HKEY_CURRENT_USER, kTypedKey, kTypedValue, REG_MULTI_SZ, multi.data(),
                    (DWORD)(multi.size() * sizeof(wchar_t)));
}

// ───────────────────────────── Galerie ─────────────────────────────

bool Gallery_IsImage(const wchar_t* name) {
    static const wchar_t* kExt[] = {L".jpg",  L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif", L".tif",
                                    L".tiff", L".avif", L".jxl", L".dng", L".cr2", L".cr3",  L".nef",  L".arw",  L".raf",
                                    L".orf",  L".rw2"};
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    for (const wchar_t* e : kExt)
        if (!_wcsicmp(dot, e)) return true;
    return false;
}

void Gallery_EnumerateFrom(DirData& d, const std::vector<std::wstring>& roots, const CancelToken& ct, size_t maxItems) {
    const double t0 = Ms();
    std::vector<std::wstring> stack(roots.rbegin(), roots.rend());
    std::unordered_set<std::wstring> seen;  // racines imbriquées (Pellicule dans Images…) : une fois
    while (!stack.empty() && d.entries.size() < maxItems && !ct.cancelled()) {
        const std::wstring dir = stack.back();
        stack.pop_back();
        if (!seen.insert(Lower(dir)).second) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> subs;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subs.push_back(full);  // pas de jonctions
            } else if (Gallery_IsImage(fd.cFileName)) {
                Add(d, fd.cFileName, fd.dwFileAttributes, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                    FtToI(fd.ftLastWriteTime), full, dir, FtToI(fd.ftCreationTime), FtToI(fd.ftLastAccessTime));
                if (d.entries.size() >= maxItems) {
                    d.truncated = true;
                    break;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (auto it = subs.rbegin(); it != subs.rend(); ++it) stack.push_back(*it);
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

void Gallery_Enumerate(DirData& d, const CancelToken& ct) {
    d.path = kGalleryPath;
    std::vector<std::wstring> roots;
    for (const KNOWNFOLDERID& id : {FOLDERID_Pictures, FOLDERID_CameraRoll, FOLDERID_SavedPictures, FOLDERID_Screenshots}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p &&
            GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
            roots.push_back(p);
        CoTaskMemFree(p);
    }
    Gallery_EnumerateFrom(d, roots, ct, 20000);
}

// ───────────────────────────── Linux (WSL) ─────────────────────────────

std::vector<std::wstring> Linux_Distributions() {
    std::vector<std::wstring> v;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &k))
        return v;
    for (DWORD i = 0;; ++i) {
        wchar_t sub[128];
        DWORD n = 128;
        if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t name[256] = {};
        DWORD cb = sizeof name;
        if (RegGetValueW(k, sub, L"DistributionName", RRF_RT_REG_SZ, nullptr, name, &cb) == ERROR_SUCCESS && *name)
            v.push_back(name);
    }
    RegCloseKey(k);
    return v;
}

std::vector<std::wstring> Linux_ParseList(const std::string& raw) {
    std::wstring w;
    // wsl.exe écrit en UTF-16 (sans BOM) ; certaines versions en UTF-8 si WSL_UTF8=1.
    const bool utf16 = raw.size() >= 2 && raw.size() % 2 == 0 && raw[1] == '\0';
    if (utf16) w.assign((const wchar_t*)raw.data(), raw.size() / 2);
    else {
        w.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), w.data(), (int)w.size());
    }
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i <= w.size()) {
        size_t e = w.find(L'\n', i);
        if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(i, e - i);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == 0)) line.pop_back();
        while (!line.empty() && (line.front() == 0xFEFF || line.front() == L' ')) line.erase(0, 1);
        if (!line.empty()) v.push_back(line);
        i = e + 1;
    }
    return v;
}

namespace {
// Sortie de « wsl.exe --list --running --quiet » (vide si WSL absent ou trop lent).
std::string RunWslList(DWORD timeoutMs) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring exe = std::wstring(sys) + L"\\wsl.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return {};
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    std::wstring cmd = L"\"" + exe + L"\" --list --running --quiet";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(w);
    std::string out;
    if (ok) {
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        char buf[4096];
        DWORD got = 0, avail = 0;
        while (PeekNamedPipe(r, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(r, buf, sizeof buf, &got, nullptr) && got)
            out.append(buf, got);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(r);
    return out;
}
}  // namespace

void Linux_Enumerate(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    d.path = kLinuxPath;
    const auto distros = Linux_Distributions();
    std::vector<std::wstring> running;
    if (!distros.empty() && !ct.cancelled()) running = Linux_ParseList(RunWslList(3000));
    for (const auto& name : distros) {
        const bool on = std::any_of(running.begin(), running.end(), [&](const std::wstring& r) { return !_wcsicmp(r.c_str(), name.c_str()); });
        Add(d, name, FILE_ATTRIBUTE_DIRECTORY, 0, 0, L"\\\\wsl.localhost\\" + name, on ? L"Démarrée" : L"Arrêtée");
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}
