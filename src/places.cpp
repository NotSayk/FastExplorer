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
    return L"";
}
