#include "archive.h"

#include "deflate.h"
#include "paths.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

// ───────────────────────────── Outils ─────────────────────────────

uint16_t U16(const BYTE* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const BYTE* p) { return (uint32_t)U16(p) | ((uint32_t)U16(p + 2) << 16); }
uint64_t U64(const BYTE* p) { return (uint64_t)U32(p) | ((uint64_t)U32(p + 4) << 32); }

void Put16(std::vector<BYTE>& b, uint32_t v) {
    b.push_back((BYTE)v);
    b.push_back((BYTE)(v >> 8));
}
void Put32(std::vector<BYTE>& b, uint32_t v) {
    Put16(b, v & 0xFFFF);
    Put16(b, v >> 16);
}
void Put64(std::vector<BYTE>& b, uint64_t v) {
    Put32(b, (uint32_t)v);
    Put32(b, (uint32_t)(v >> 32));
}

int64_t FtToI(const FILETIME& f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime); }
FILETIME IToFt(int64_t t) { return FILETIME{(DWORD)t, (DWORD)((uint64_t)t >> 32)}; }

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::wstring Decode(const BYTE* p, size_t n, bool utf8) {
    if (!n) return {};
    const UINT cp = utf8 ? CP_UTF8 : CP_OEMCP;  // ancien ZIP : page de code OEM, comme l'Explorateur
    const int m = MultiByteToWideChar(cp, 0, (const char*)p, (int)n, nullptr, 0);
    std::wstring s(m, L'\0');
    MultiByteToWideChar(cp, 0, (const char*)p, (int)n, s.data(), m);
    return s;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int m = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), m, nullptr, nullptr);
    return u;
}

// Nom interne → chemin relatif propre ; false si dangereux (« .. », lecteur, absolu) ou vide.
bool NormalizeName(std::wstring& n, bool& dir) {
    std::replace(n.begin(), n.end(), L'/', L'\\');
    dir = !n.empty() && n.back() == L'\\';
    while (!n.empty() && n.back() == L'\\') n.pop_back();
    while (!n.empty() && n.front() == L'\\') n.erase(0, 1);
    while (n.size() >= 2 && n[0] == L'.' && n[1] == L'\\') n.erase(0, 2);
    if (n.empty() || n.find(L':') != std::wstring::npos) return false;
    size_t a = 0;
    while (a <= n.size()) {
        size_t b = n.find(L'\\', a);
        if (b == std::wstring::npos) b = n.size();
        const std::wstring part = n.substr(a, b - a);
        if (part.empty() || part == L"..") return false;
        a = b + 1;
    }
    return true;
}

// Un élément de chemin valable sous Windows (caractères interdits remplacés, noms réservés évités).
std::wstring SafePart(std::wstring p) {
    for (auto& c : p)
        if (c < 32 || wcschr(L"<>:\"/|?*", c)) c = L'_';
    while (!p.empty() && (p.back() == L' ' || p.back() == L'.')) p.pop_back();
    if (p.empty()) return L"_";
    std::wstring stem = Lower(p.substr(0, p.find(L'.')));
    static const wchar_t* kReserved[] = {L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3", L"com4", L"com5",
                                         L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5",
                                         L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
    for (const wchar_t* r : kReserved)
        if (stem == r) return L"_" + p;
    return p;
}

std::vector<std::wstring> Parts(const std::wstring& rel) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= rel.size()) {
        size_t b = rel.find(L'\\', a);
        if (b == std::wstring::npos) b = rel.size();
        v.push_back(rel.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(ToNtPath(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring UniqueIn(const std::wstring& dir, const std::wstring& name, bool isDir) {
    if (!Exists(Join(dir, name.c_str()))) return name;
    std::wstring stem = name, ext;
    if (!isDir) {
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = name.substr(0, dot);
            ext = name.substr(dot);
        }
    }
    for (int i = 2; i < 10000; ++i) {
        std::wstring n = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!Exists(Join(dir, n.c_str()))) return n;
    }
    return name;
}

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD n, DWORD& got) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    got = 0;
    if (!ReadFile(h, buf, n, &got, &ov)) return GetLastError() == ERROR_HANDLE_EOF;
    return true;
}

// ───────────────────────────── Index (répertoire central) ─────────────────────────────

void ParseExtra(const BYTE* x, size_t n, ZipEntry& e, bool needU, bool needC, bool needO, const BYTE* rawName,
                size_t rawLen, std::wstring& unicodeName) {
    for (size_t i = 0; i + 4 <= n;) {
        const uint16_t id = U16(x + i), sz = U16(x + i + 2);
        const BYTE* d = x + i + 4;
        if (i + 4 + sz > n) break;
        if (id == 0x0001) {  // ZIP64
            size_t k = 0;
            if (needU && k + 8 <= sz) e.usize = U64(d + k), k += 8;
            if (needC && k + 8 <= sz) e.csize = U64(d + k), k += 8;
            if (needO && k + 8 <= sz) e.offset = U64(d + k), k += 8;
        } else if (id == 0x000A && sz >= 32) {  // NTFS : dates exactes
            for (size_t k = 4; k + 4 <= sz;) {
                const uint16_t tag = U16(d + k), tsz = U16(d + k + 2);
                if (tag == 1 && tsz >= 24 && k + 4 + 24 <= sz) {
                    e.mtime = (int64_t)U64(d + k + 4);
                    e.atime = (int64_t)U64(d + k + 12);
                    e.ctime = (int64_t)U64(d + k + 20);
                }
                k += 4 + tsz;
            }
        } else if (id == 0x5455 && sz >= 5 && (d[0] & 1) && !e.mtime) {  // Unix : date de modification
            const int32_t t = (int32_t)U32(d + 1);
            e.mtime = ((int64_t)t + 11644473600LL) * 10000000LL;
        } else if (id == 0x7075 && sz > 5 && d[0] == 1) {  // Info-ZIP : nom Unicode
            if (U32(d + 1) == Crc32(0, rawName, rawLen)) unicodeName = Decode(d + 5, sz - 5, true);
        }
        i += 4 + sz;
    }
}

}  // namespace

std::shared_ptr<const ZipIndex> Zip_ReadIndex(const std::wstring& zip) {
    auto idx = std::make_shared<ZipIndex>();
    idx->path = zip;
    HANDLE f = CreateFileW(ToNtPath(zip).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        idx->error = GetLastError();
        return idx;
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{f};
    BY_HANDLE_FILE_INFORMATION fi{};
    GetFileInformationByHandle(f, &fi);
    idx->writeTime = FtToI(fi.ftLastWriteTime);
    const uint64_t size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    idx->fileSize = size;
    idx->error = ERROR_BAD_FORMAT;
    if (size < 22) return idx;
    const DWORD tailLen = (DWORD)std::min<uint64_t>(size, 65535 + 22 + 20);
    std::vector<BYTE> tail(tailLen);
    DWORD got = 0;
    if (!ReadAt(f, size - tailLen, tail.data(), tailLen, got) || got != tailLen) return idx;
    int64_t at = -1;
    for (int64_t i = (int64_t)tailLen - 22; i >= 0; --i)
        if (U32(&tail[i]) == 0x06054b50) {
            at = i;
            break;
        }
    if (at < 0) return idx;
    const BYTE* eo = &tail[at];
    uint64_t count = U16(eo + 10), cdSize = U32(eo + 12), cdOff = U32(eo + 16);
    uint64_t cdEnd = size - tailLen + (uint64_t)at;  // où le répertoire central doit finir
    if ((count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOff == 0xFFFFFFFF) && at >= 20 && U32(eo - 20) == 0x07064b50) {
        const uint64_t locPos = size - tailLen + (uint64_t)at - 20;
        uint64_t recPos = U64(eo - 20 + 8);
        BYTE r[56];
        bool ok = ReadAt(f, recPos, r, 56, got) && got == 56 && U32(r) == 0x06064b50;
        if (!ok && locPos >= 56) {  // positions décalées (données ajoutées devant) : juste avant le localisateur
            recPos = locPos - 56;
            ok = ReadAt(f, recPos, r, 56, got) && got == 56 && U32(r) == 0x06064b50;
        }
        if (ok) {
            count = U64(r + 32);
            cdSize = U64(r + 40);
            cdOff = U64(r + 48);
            cdEnd = recPos;
        }
    }
    // Données ajoutées devant (auto-extractible) : décalage de toutes les positions.
    const int64_t shift = cdOff + cdSize <= cdEnd ? (int64_t)(cdEnd - (cdOff + cdSize)) : 0;
    cdOff += shift;
    if (cdSize > (512ull << 20) || cdOff + cdSize > size) return idx;
    std::vector<BYTE> cd((size_t)cdSize);
    if (cdSize && (!ReadAt(f, cdOff, cd.data(), (DWORD)cdSize, got) || got != cdSize)) return idx;
    idx->entries.reserve((size_t)std::min<uint64_t>(count, 1000000));
    for (size_t p = 0; p + 46 <= cd.size();) {
        const BYTE* h = &cd[p];
        if (U32(h) != 0x02014b50) break;
        const uint16_t nameLen = U16(h + 28), extraLen = U16(h + 30), commentLen = U16(h + 32);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) break;
        ZipEntry e;
        const uint16_t madeBy = U16(h + 4);
        e.flags = U16(h + 8);
        e.method = U16(h + 10);
        const uint16_t time = U16(h + 12), date = U16(h + 14);
        e.crc = U32(h + 16);
        e.csize = U32(h + 20);
        e.usize = U32(h + 24);
        const uint32_t ext = U32(h + 38);
        e.offset = U32(h + 42);
        const BYTE* raw = h + 46;
        std::wstring uname;
        ParseExtra(raw + nameLen, extraLen, e, e.usize == 0xFFFFFFFF, e.csize == 0xFFFFFFFF, e.offset == 0xFFFFFFFF,
                   raw, nameLen, uname);
        e.offset += shift;
        e.name = uname.empty() ? Decode(raw, nameLen, (e.flags & 0x800) != 0) : uname;
        const int host = madeBy >> 8;
        if (host == 0 || host == 10 || host == 11 || host == 14) e.attr = ext & 0xFFFF;
        else if (host == 3 && ((ext >> 16) & 0xF000) == 0x4000) e.attr = FILE_ATTRIBUTE_DIRECTORY;
        if (!e.mtime) {
            FILETIME local, utc;
            if (DosDateTimeToFileTime(date, time, &local) && LocalFileTimeToFileTime(&local, &utc)) e.mtime = FtToI(utc);
        }
        bool slash = false;
        if (NormalizeName(e.name, slash)) {
            e.dir = slash || (e.attr & FILE_ATTRIBUTE_DIRECTORY);
            if (e.dir) e.attr |= FILE_ATTRIBUTE_DIRECTORY;
            idx->entries.push_back(std::move(e));
        }
        p += 46 + nameLen + extraLen + commentLen;
    }
    idx->error = 0;
    return idx;
}

std::shared_ptr<const ZipIndex> Zip_Index(const std::wstring& zip) {
    static std::mutex lock;
    static std::vector<std::shared_ptr<const ZipIndex>> cache;  // les plus récents à la fin
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(ToNtPath(zip).c_str(), GetFileExInfoStandard, &a)) {
        const uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        std::lock_guard<std::mutex> l(lock);
        for (size_t i = 0; i < cache.size(); ++i)
            if (_wcsicmp(cache[i]->path.c_str(), zip.c_str()) == 0) {
                auto c = cache[i];
                cache.erase(cache.begin() + i);
                if (c->writeTime == FtToI(a.ftLastWriteTime) && c->fileSize == size) {
                    cache.push_back(c);
                    return c;
                }
                break;
            }
    }
    auto idx = Zip_ReadIndex(zip);
    if (!idx->error) {
        std::lock_guard<std::mutex> l(lock);
        cache.push_back(idx);
        if (cache.size() > 8) cache.erase(cache.begin());
    }
    return idx;
}

bool Archive_IsZip(const std::wstring& name) { return EndsWithI(name, L".zip"); }

bool Archive_IsArchive(const std::wstring& name) {
    static const wchar_t* kExt[] = {L".zip", L".7z",  L".rar",  L".tar", L".gz",  L".tgz", L".bz2", L".tbz",
                                    L".tbz2", L".xz", L".txz",  L".zst", L".tzst", L".lzma", L".cab"};
    for (const wchar_t* e : kExt)
        if (EndsWithI(name, e)) return true;
    return false;
}

std::wstring Archive_Stem(const std::wstring& name) {
    std::wstring s = name;
    const size_t dot = s.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) s.resize(dot);
    if (EndsWithI(s, L".tar") && s.size() > 4) s.resize(s.size() - 4);
    return s.empty() ? name : s;
}

bool Archive_Split(const std::wstring& path, std::wstring& archive, std::wstring& inner) {
    for (size_t i = 4; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != L'\\') continue;
        if (_wcsnicmp(path.c_str() + i - 4, L".zip", 4) != 0) continue;
        const std::wstring prefix = path.substr(0, i);
        const DWORD a = GetFileAttributesW(ToNtPath(prefix).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        archive = prefix;
        inner = i < path.size() ? path.substr(i + 1) : std::wstring();
        while (!inner.empty() && inner.back() == L'\\') inner.pop_back();
        return true;
    }
    return false;
}

void Archive_Enumerate(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    std::wstring arc, inner;
    if (!Archive_Split(d.path, arc, inner)) {
        d.error = ERROR_PATH_NOT_FOUND;
        return;
    }
    d.archive = arc;
    auto idx = Zip_Index(arc);
    if (idx->error) {
        d.error = idx->error;
        return;
    }
    const std::wstring prefix = inner.empty() ? std::wstring() : inner + L"\\";
    std::unordered_map<std::wstring, uint32_t> dirs;  // nom en minuscules → entrée
    bool found = inner.empty();
    for (const ZipEntry& e : idx->entries) {
        if (ct.cancelled()) return;
        if (!prefix.empty()) {
            if (e.dir && e.name.size() == inner.size() && _wcsicmp(e.name.c_str(), inner.c_str()) == 0) found = true;
            if (!StartsWithI(e.name, prefix)) continue;
            found = true;
        }
        const std::wstring rel = e.name.substr(prefix.size());
        const size_t sep = rel.find(L'\\');
        if (sep == std::wstring::npos && !e.dir) {
            DirData_Add(d, rel.c_str(), (uint32_t)rel.size(), e.attr ? e.attr : FILE_ATTRIBUTE_NORMAL, e.usize, e.mtime,
                        e.ctime, e.atime);
            continue;
        }
        const std::wstring name = sep == std::wstring::npos ? rel : rel.substr(0, sep);
        const std::wstring key = Lower(name);
        auto it = dirs.find(key);
        const bool self = sep == std::wstring::npos;  // l'entrée du dossier lui-même : ses dates
        if (it == dirs.end()) {
            dirs.emplace(key, (uint32_t)d.entries.size());
            DirData_Add(d, name.c_str(), (uint32_t)name.size(), self ? e.attr | FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_DIRECTORY,
                        0, self ? e.mtime : 0, self ? e.ctime : 0, self ? e.atime : 0);
        } else if (self) {
            Entry& x = d.entries[it->second];
            x.attr = e.attr | FILE_ATTRIBUTE_DIRECTORY;
            x.mtime = e.mtime;
            x.ctime = e.ctime;
            x.atime = e.atime;
        }
    }
    if (!found) {
        d.error = ERROR_PATH_NOT_FOUND;
        d.entries.clear();
        d.pool.clear();
        return;
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

// ───────────────────────────── Extraction ─────────────────────────────

namespace {

struct Pick {
    const ZipEntry* e;
    std::wstring rel;  // relatif à base, nettoyé
};

std::vector<Pick> Select(const ZipIndex& idx, const ExtractOptions& opt) {
    std::unordered_set<std::wstring> want;
    for (const auto& i : opt.items) want.insert(Lower(i));
    const std::wstring prefix = opt.base.empty() ? std::wstring() : opt.base + L"\\";
    std::vector<Pick> v;
    for (const ZipEntry& e : idx.entries) {
        if (!prefix.empty() && !StartsWithI(e.name, prefix)) continue;
        std::wstring rel = e.name.substr(prefix.size());
        if (rel.empty()) continue;
        if (!want.empty() && !want.count(Lower(rel.substr(0, rel.find(L'\\'))))) continue;
        std::wstring clean;
        for (const auto& part : Parts(rel)) clean += (clean.empty() ? L"" : L"\\") + SafePart(part);
        v.push_back({&e, std::move(clean)});
    }
    return v;
}

void SetTimes(const std::wstring& path, const ZipEntry& e, bool dir) {
    if (!e.mtime) return;
    HANDLE h = CreateFileW(ToNtPath(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, dir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const FILETIME m = IToFt(e.mtime), c = IToFt(e.ctime ? e.ctime : e.mtime), a = IToFt(e.atime ? e.atime : e.mtime);
    SetFileTime(h, &c, &a, &m);
    CloseHandle(h);
}

constexpr DWORD kKeepAttrs = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;

// Un fichier : données (stockées ou deflate) → `out`, CRC vérifié.
DWORD ExtractOne(HANDLE zip, const ZipEntry& e, const std::wstring& out, bool overwrite, FastProgress& prog,
                 std::wstring& why) {
    if (e.encrypted()) {
        why = L"protégé par mot de passe";
        return ERROR_NOT_SUPPORTED;
    }
    if (e.method != 0 && e.method != 8) {
        why = L"méthode de compression " + std::to_wstring(e.method) + L" non prise en charge";
        return ERROR_NOT_SUPPORTED;
    }
    BYTE lh[30];
    DWORD got = 0;
    if (!ReadAt(zip, e.offset, lh, 30, got) || got != 30 || U32(lh) != 0x04034b50) return ERROR_FILE_CORRUPT;
    uint64_t pos = e.offset + 30 + U16(lh + 26) + U16(lh + 28);
    uint64_t left = e.csize;
    const std::wstring nt = ToNtPath(out);
    if (overwrite) SetFileAttributesW(nt.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(nt.c_str(), GENERIC_WRITE, 0, nullptr, overwrite ? CREATE_ALWAYS : CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    uint32_t crc = 0;
    uint64_t total = 0;
    DWORD err = 0;
    auto read = [&](BYTE* b, size_t cap) -> size_t {
        const DWORD n = (DWORD)std::min<uint64_t>(cap, left);
        DWORD g = 0;
        if (!n || !ReadAt(zip, pos, b, n, g)) return 0;
        pos += g;
        left -= g;
        return g;
    };
    auto write = [&](const BYTE* b, size_t n) {
        if (prog.cancel) {
            err = ERROR_CANCELLED;
            return false;
        }
        while (prog.pause && !prog.cancel) Sleep(50);
        DWORD w = 0;
        if (!WriteFile(h, b, (DWORD)n, &w, nullptr) || w != n) {
            err = GetLastError();
            return false;
        }
        crc = Crc32(crc, b, n);
        total += n;
        prog.bytesDone += n;
        return true;
    };
    bool ok;
    if (e.method == 0) {
        std::vector<BYTE> buf(1 << 20);
        ok = true;
        for (size_t n; ok && (n = read(buf.data(), buf.size())) > 0;) ok = write(buf.data(), n);
    } else {
        ok = InflateStream(read, write);
    }
    if (ok && (crc != e.crc || total != e.usize)) {
        ok = false;
        err = ERROR_CRC;
        why = L"données endommagées";
    } else if (!ok && !err) {
        err = ERROR_FILE_CORRUPT;
        why = L"données endommagées";
    }
    if (ok && e.mtime) {
        const FILETIME m = IToFt(e.mtime), c = IToFt(e.ctime ? e.ctime : e.mtime), a = IToFt(e.atime ? e.atime : e.mtime);
        SetFileTime(h, &c, &a, &m);
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(nt.c_str());
        return err;
    }
    if (e.attr & kKeepAttrs) SetFileAttributesW(nt.c_str(), e.attr & (kKeepAttrs | FILE_ATTRIBUTE_ARCHIVE));
    return 0;
}

DWORD ZipExtract(const std::wstring& archive, const std::wstring& dest, const ExtractOptions& opt, FastProgress& prog,
                 std::vector<std::wstring>* created, std::wstring* detail) {
    auto idx = Zip_Index(archive);
    if (idx->error) return idx->error;
    std::vector<Pick> picks = Select(*idx, opt);
    if (picks.empty()) return ERROR_FILE_NOT_FOUND;
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    // Premier niveau : nom final (« nom (2) » si quelque chose existe déjà).
    std::unordered_map<std::wstring, std::wstring> top;
    std::vector<std::wstring> tops;
    for (auto& p : picks) {
        const size_t sep = p.rel.find(L'\\');
        const std::wstring t = p.rel.substr(0, sep);
        auto it = top.find(Lower(t));
        if (it == top.end()) {
            const bool isDir = sep != std::wstring::npos || p.e->dir;
            const std::wstring fin = opt.overwrite ? t : UniqueIn(dest, t, isDir);
            it = top.emplace(Lower(t), fin).first;
            tops.push_back(fin);
        }
        p.rel = it->second + (sep == std::wstring::npos ? std::wstring() : p.rel.substr(sep));
    }
    // Dossiers d'abord (ceux qui sont nommés et ceux qu'impliquent les fichiers).
    std::vector<std::wstring> mk;
    std::unordered_set<std::wstring> seen;
    std::vector<const Pick*> files, dirEntries;
    for (const auto& p : picks) {
        std::wstring dir = p.e->dir ? p.rel : p.rel.substr(0, p.rel.rfind(L'\\') == std::wstring::npos ? 0 : p.rel.rfind(L'\\'));
        while (!dir.empty() && seen.insert(Lower(dir)).second) {
            mk.push_back(dir);
            const size_t s = dir.rfind(L'\\');
            dir = s == std::wstring::npos ? std::wstring() : dir.substr(0, s);
        }
        (p.e->dir ? dirEntries : files).push_back(&p);
    }
    std::sort(mk.begin(), mk.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() < b.size(); });
    for (const auto& m : mk) CreateDirectoryW(ToNtPath(Join(dest, m.c_str())).c_str(), nullptr);

    uint64_t total = 0;
    for (const Pick* f : files) total += f->e->usize;
    prog.bytesTotal += total;
    prog.filesTotal += (uint32_t)files.size();

    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex errLock;
    DWORD firstErr = 0;
    std::wstring why;
    const int threads = (int)std::clamp<size_t>(std::min<size_t>(FastCopyThreads(archive, dest), files.size()), 1, 8);
    auto work = [&] {
        HANDLE z = CreateFileW(ToNtPath(archive).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (z == INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> l(errLock);
            if (!firstErr) firstErr = GetLastError();
            stop = true;
            return;
        }
        for (size_t i; !stop && !prog.cancel && (i = next++) < files.size();) {
            const Pick& p = *files[i];
            std::wstring w;
            const DWORD e = ExtractOne(z, *p.e, Join(dest, p.rel.c_str()), opt.overwrite, prog, w);
            if (e) {
                std::lock_guard<std::mutex> l(errLock);
                if (!firstErr) {
                    firstErr = e;
                    why = L"« " + p.e->name + L" »" + (w.empty() ? L"" : L" : " + w);
                }
                stop = true;
                break;
            }
            ++prog.filesDone;
        }
        CloseHandle(z);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    // Dates des dossiers à la fin (l'écriture des fichiers les a changées).
    for (const Pick* d : dirEntries) {
        SetTimes(Join(dest, d->rel.c_str()), *d->e, true);
        if (d->e->attr & kKeepAttrs)
            SetFileAttributesW(ToNtPath(Join(dest, d->rel.c_str())).c_str(), d->e->attr & (kKeepAttrs | FILE_ATTRIBUTE_DIRECTORY));
    }
    if (created)
        for (const auto& t : tops)
            if (Exists(Join(dest, t.c_str()))) created->push_back(t);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, dest.c_str(), nullptr);
    if (prog.cancel && !firstErr) return ERROR_CANCELLED;
    if (firstErr && detail) *detail = why;
    return firstErr;
}

// Chemin court (8.3) si le chemin a des caractères hors ASCII : tar.exe lit sa ligne de commande en ANSI.
std::wstring ForTar(const std::wstring& p) {
    if (std::all_of(p.begin(), p.end(), [](wchar_t c) { return c < 128; })) return p;
    wchar_t s[MAX_PATH * 2];
    const DWORD n = GetShortPathNameW(p.c_str(), s, (DWORD)std::size(s));
    return n && n < std::size(s) ? std::wstring(s) : p;
}

DWORD TarExtract(const std::wstring& archive, const std::wstring& dest, FastProgress& prog,
                 std::vector<std::wstring>* created, std::wstring* detail) {
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring exe = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + exe + L"\" -x -f \"" + ForTar(archive) + L"\" -C \"" + ForTar(dest) + L"\"";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &a))
        prog.bytesTotal += ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    ++prog.filesTotal;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return GetLastError();
    CloseHandle(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT)
        if (prog.cancel) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (prog.cancel) return ERROR_CANCELLED;
    prog.bytesDone += a.nFileSizeLow | ((uint64_t)a.nFileSizeHigh << 32);
    ++prog.filesDone;
    if (created) created->push_back(L"");
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, dest.c_str(), nullptr);
    if (code) {
        if (detail) *detail = L"archive illisible ou format non pris en charge";
        return ERROR_BAD_FORMAT;
    }
    return 0;
}

}  // namespace

DWORD Archive_Extract(const std::wstring& archive, const std::wstring& dest, const ExtractOptions& opt, FastProgress& prog,
                      std::vector<std::wstring>* created, std::wstring* detail) {
    if (!prog.start) prog.start = GetTickCount64();
    if (Archive_IsZip(archive)) return ZipExtract(archive, dest, opt, prog, created, detail);
    return TarExtract(archive, dest, prog, created, detail);
}

uint64_t Archive_SizeOf(const std::wstring& archive, const ExtractOptions& opt) {
    auto idx = Zip_Index(archive);
    uint64_t n = 0;
    if (!idx->error)
        for (const auto& p : Select(*idx, opt)) n += p.e->usize;
    return n;
}

// ───────────────────────────── Création ─────────────────────────────

namespace {

struct Src {
    std::wstring full;
    std::string name;  // UTF-8, séparateur '/', '/' final pour un dossier
    bool dir = false, store = false;
    uint64_t size = 0;
    uint32_t attr = 0;
    FILETIME c{}, a{}, m{};
};

bool AlreadyCompressed(const std::wstring& name) {
    static const wchar_t* kExt[] = {L".jpg", L".jpeg", L".png", L".gif", L".webp", L".heic", L".avif", L".mp3",
                                    L".m4a", L".aac",  L".ogg", L".opus", L".flac", L".mp4",  L".m4v",  L".mkv",
                                    L".webm", L".avi", L".mov", L".wmv", L".zip",  L".7z",   L".rar",  L".gz",
                                    L".tgz", L".bz2",  L".xz",  L".zst", L".cab",  L".docx", L".xlsx", L".pptx",
                                    L".odt", L".ods",  L".odp", L".epub", L".apk", L".jar",  L".msi",  L".jxl"};
    for (const wchar_t* e : kExt)
        if (EndsWithI(name, e)) return true;
    return false;
}

void Walk(const std::wstring& full, const std::wstring& rel, const WIN32_FIND_DATAW& fd, const std::wstring& skip,
          std::vector<Src>& out, const FastProgress& prog) {
    if (prog.cancel) return;
    if (_wcsicmp(full.c_str(), skip.c_str()) == 0) return;  // l'archive en cours d'écriture
    Src s;
    s.full = full;
    s.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.attr = fd.dwFileAttributes;
    s.c = fd.ftCreationTime;
    s.a = fd.ftLastAccessTime;
    s.m = fd.ftLastWriteTime;
    s.size = s.dir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    std::wstring n = rel;
    std::replace(n.begin(), n.end(), L'\\', L'/');
    s.name = Utf8(n) + (s.dir ? "/" : "");
    s.store = !s.dir && (s.size == 0 || AlreadyCompressed(rel));
    const bool descend = s.dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);  // jonction : pas suivie
    out.push_back(std::move(s));
    if (!descend) return;
    WIN32_FIND_DATAW c;
    HANDLE h = FindFirstFileExW(ToNtPath(Join(full, L"*")).c_str(), FindExInfoBasic, &c, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(c.cFileName, L".") || !wcscmp(c.cFileName, L"..")) continue;
        Walk(Join(full, c.cFileName), rel + L"\\" + c.cFileName, c, skip, out, prog);
    } while (FindNextFileW(h, &c));
    FindClose(h);
}

void DosTime(const FILETIME& utc, uint16_t& date, uint16_t& time) {
    FILETIME local;
    date = 0x21;  // 1er janvier 1980
    time = 0;
    if (FileTimeToLocalFileTime(&utc, &local)) FileTimeToDosDateTime(&local, &date, &time);
}

class Out {
public:
    explicit Out(HANDLE h) : h_(h) { buf_.reserve(kCap + (1 << 20)); }
    bool Write(const void* p, size_t n) {
        const BYTE* b = (const BYTE*)p;
        buf_.insert(buf_.end(), b, b + n);
        pos_ += n;
        return buf_.size() < kCap || Flush();
    }
    bool Flush() {
        DWORD w = 0;
        if (!buf_.empty() && (!WriteFile(h_, buf_.data(), (DWORD)buf_.size(), &w, nullptr) || w != buf_.size())) {
            err_ = GetLastError();
            return false;
        }
        flushed_ += buf_.size();
        buf_.clear();
        return true;
    }
    // Réécrit des octets déjà écrits (en-tête local : CRC et tailles connus après les données).
    bool Patch(uint64_t off, const void* p, size_t n) {
        if (off >= flushed_) {
            memcpy(buf_.data() + (off - flushed_), p, n);
            return true;
        }
        if (!Flush()) return false;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD w = 0;
        const bool ok = SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) && WriteFile(h_, p, (DWORD)n, &w, nullptr) && w == n;
        li.QuadPart = (LONGLONG)flushed_;
        SetFilePointerEx(h_, li, nullptr, FILE_BEGIN);
        if (!ok) err_ = GetLastError();
        return ok;
    }
    uint64_t pos() const { return pos_; }
    DWORD err() const { return err_ ? err_ : ERROR_WRITE_FAULT; }

private:
    static constexpr size_t kCap = 4 << 20;
    HANDLE h_;
    std::vector<BYTE> buf_;
    uint64_t pos_ = 0, flushed_ = 0;
    DWORD err_ = 0;
};

struct Central {
    size_t src;
    uint16_t method;
    uint32_t crc;
    uint64_t csize, usize, offset;
};

constexpr size_t kChunk = 1 << 20;

}  // namespace

DWORD Zip_Create(const std::wstring& zip, const std::vector<std::wstring>& sources, FastProgress& prog,
                 std::wstring* detail) {
    if (!prog.start) prog.start = GetTickCount64();
    std::vector<Src> src;
    for (const auto& s : sources) {
        WIN32_FIND_DATAW fd{};
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(ToNtPath(s).c_str(), GetFileExInfoStandard, &a)) {
            if (detail) *detail = L"« " + s + L" »";
            return GetLastError();
        }
        fd.dwFileAttributes = a.dwFileAttributes;
        fd.ftCreationTime = a.ftCreationTime;
        fd.ftLastAccessTime = a.ftLastAccessTime;
        fd.ftLastWriteTime = a.ftLastWriteTime;
        fd.nFileSizeHigh = a.nFileSizeHigh;
        fd.nFileSizeLow = a.nFileSizeLow;
        std::wstring parent, leaf;
        if (!ParentOf(s, parent, leaf) || leaf.empty()) leaf = s.substr(0, 1);  // lecteur : sa lettre
        Walk(s, leaf, fd, zip, src, prog);
    }
    if (prog.cancel) return ERROR_CANCELLED;

    struct Item {
        size_t src;
        uint64_t off;
        uint32_t len;
        bool last;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i].dir) continue;
        prog.bytesTotal += src[i].size;
        ++prog.filesTotal;
        const uint64_t n = std::max<uint64_t>(1, (src[i].size + kChunk - 1) / kChunk);
        for (uint64_t k = 0; k < n; ++k)
            items.push_back({i, k * kChunk, (uint32_t)std::min<uint64_t>(kChunk, src[i].size - std::min(src[i].size, k * kChunk)),
                             k + 1 == n});
    }

    HANDLE h = CreateFileW(ToNtPath(zip).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    Out out(h);

    // Morceaux compressés en parallèle, écrits dans l'ordre par ce thread.
    struct Slot {
        std::vector<BYTE> data;
        uint32_t crc = 0;
        uint64_t raw = 0;
        DWORD err = 0;
        bool ready = false;
    };
    const int threads = (int)std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 16);
    const size_t window = (size_t)threads * 2 + 2;
    std::vector<Slot> slots(window);
    std::mutex m;
    std::condition_variable cv;
    size_t written = 0;
    bool stop = false;
    std::atomic<size_t> next{0};
    auto work = [&] {
        std::vector<BYTE> buf;
        for (;;) {
            const size_t i = next++;
            if (i >= items.size()) return;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return stop || i < written + window; });
                if (stop) return;
            }
            while (prog.pause && !prog.cancel) Sleep(50);
            const Item& it = items[i];
            const Src& s = src[it.src];
            Slot r;
            const size_t dict = s.store ? 0 : (size_t)std::min<uint64_t>(it.off, 32768);
            HANDLE f = CreateFileW(ToNtPath(s.full).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                r.err = GetLastError();
            } else {
                buf.resize(dict + it.len);
                size_t have = 0;
                while (have < buf.size()) {
                    DWORD g = 0;
                    if (!ReadAt(f, it.off - dict + have, buf.data() + have, (DWORD)(buf.size() - have), g)) {
                        r.err = GetLastError();
                        break;
                    }
                    if (!g) break;
                    have += g;
                }
                CloseHandle(f);
                const size_t raw = have > dict ? have - dict : 0;
                r.raw = raw;
                r.crc = Crc32(0, buf.data() + dict, raw);
                if (s.store) r.data.assign(buf.begin() + dict, buf.begin() + dict + raw);
                else DeflateChunk(buf.data(), dict, raw, it.last, r.data);
                prog.bytesDone += raw;
            }
            r.ready = true;
            std::lock_guard<std::mutex> l(m);
            slots[i % window] = std::move(r);
            cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads && t < (int)items.size(); ++t) pool.emplace_back(work);

    DWORD err = 0;
    std::wstring failed;
    std::vector<Central> central;
    size_t ii = 0;  // prochain morceau à écrire
    for (size_t i = 0; i < src.size() && !err; ++i) {
        if (prog.cancel) {
            err = ERROR_CANCELLED;
            break;
        }
        const Src& s = src[i];
        const uint16_t method = s.dir || s.store ? 0 : 8;
        const bool z64 = s.size >= 0xFFFF0000ull;
        uint16_t date, time;
        DosTime(s.m, date, time);
        std::vector<BYTE> hd;
        Put32(hd, 0x04034b50);
        Put16(hd, z64 ? 45 : 20);
        Put16(hd, 0x0800);  // noms en UTF-8
        Put16(hd, method);
        Put16(hd, time);
        Put16(hd, date);
        Put32(hd, 0);
        Put32(hd, z64 ? 0xFFFFFFFF : 0);
        Put32(hd, z64 ? 0xFFFFFFFF : 0);
        Put16(hd, (uint32_t)s.name.size());
        Put16(hd, z64 ? 20 : 0);
        hd.insert(hd.end(), s.name.begin(), s.name.end());
        if (z64) {
            Put16(hd, 1);
            Put16(hd, 16);
            Put64(hd, 0);
            Put64(hd, 0);
        }
        const uint64_t at = out.pos();
        if (!out.Write(hd.data(), hd.size())) {
            err = out.err();
            break;
        }
        Central c{i, method, 0, 0, 0, at};
        if (!s.dir) {
            bool first = true;
            for (;;) {
                Slot r;
                {
                    std::unique_lock<std::mutex> l(m);
                    cv.wait(l, [&] { return slots[ii % window].ready || prog.cancel.load(); });
                    if (!slots[ii % window].ready) {
                        err = ERROR_CANCELLED;
                        break;
                    }
                    r = std::move(slots[ii % window]);
                    slots[ii % window].ready = false;
                    ++written;
                    cv.notify_all();
                }
                const bool last = items[ii].last;
                ++ii;
                if (prog.cancel) {
                    err = ERROR_CANCELLED;
                    break;
                }
                if (r.err) {
                    err = r.err;
                    failed = s.full;
                    break;
                }
                if (!out.Write(r.data.data(), r.data.size())) {
                    err = out.err();
                    break;
                }
                c.crc = first ? r.crc : Crc32Combine(c.crc, r.crc, r.raw);
                first = false;
                c.usize += r.raw;
                c.csize += r.data.size();
                if (last) break;
            }
            if (err) break;
            BYTE p[16];
            memcpy(p, &c.crc, 4);
            bool ok = out.Patch(at + 14, p, 4);
            if (z64) {
                memcpy(p, &c.usize, 8);
                memcpy(p + 8, &c.csize, 8);
                ok = ok && out.Patch(at + 30 + s.name.size() + 4, p, 16);
            } else {
                const uint32_t cs = (uint32_t)c.csize, us = (uint32_t)c.usize;
                memcpy(p, &cs, 4);
                memcpy(p + 4, &us, 4);
                ok = ok && out.Patch(at + 18, p, 8);
            }
            if (!ok) {
                err = out.err();
                break;
            }
            ++prog.filesDone;
        }
        central.push_back(c);
    }
    {
        std::lock_guard<std::mutex> l(m);
        stop = true;
        cv.notify_all();
    }
    for (auto& t : pool) t.join();

    if (!err) {  // répertoire central
        const uint64_t cdOff = out.pos();
        std::vector<BYTE> b;
        for (const Central& c : central) {
            const Src& s = src[c.src];
            uint16_t date, time;
            DosTime(s.m, date, time);
            std::vector<BYTE> x;  // champs supplémentaires
            const bool bu = c.usize >= 0xFFFFFFFF, bc = c.csize >= 0xFFFFFFFF, bo = c.offset >= 0xFFFFFFFF;
            if (bu || bc || bo) {
                Put16(x, 1);
                Put16(x, 8 * (bu + bc + bo));
                if (bu) Put64(x, c.usize);
                if (bc) Put64(x, c.csize);
                if (bo) Put64(x, c.offset);
            }
            Put16(x, 0x000A);  // NTFS : dates exactes
            Put16(x, 32);
            Put32(x, 0);
            Put16(x, 1);
            Put16(x, 24);
            Put64(x, (uint64_t)FtToI(s.m));
            Put64(x, (uint64_t)FtToI(s.a));
            Put64(x, (uint64_t)FtToI(s.c));
            b.clear();
            Put32(b, 0x02014b50);
            Put16(b, 0x003F);  // créé par : MS-DOS / Windows, version 6.3
            Put16(b, (bu || bc || bo) ? 45 : 20);
            Put16(b, 0x0800);
            Put16(b, c.method);
            Put16(b, time);
            Put16(b, date);
            Put32(b, c.crc);
            Put32(b, bc ? 0xFFFFFFFF : (uint32_t)c.csize);
            Put32(b, bu ? 0xFFFFFFFF : (uint32_t)c.usize);
            Put16(b, (uint32_t)s.name.size());
            Put16(b, (uint32_t)x.size());
            Put16(b, 0);
            Put16(b, 0);
            Put16(b, 0);
            Put32(b, s.attr & 0xFFFF);
            Put32(b, bo ? 0xFFFFFFFF : (uint32_t)c.offset);
            b.insert(b.end(), s.name.begin(), s.name.end());
            b.insert(b.end(), x.begin(), x.end());
            if (!out.Write(b.data(), b.size())) {
                err = out.err();
                break;
            }
        }
        const uint64_t cdSize = out.pos() - cdOff, n = central.size();
        b.clear();
        if (n >= 0xFFFF || cdOff >= 0xFFFFFFFF || cdSize >= 0xFFFFFFFF) {
            const uint64_t z64 = out.pos();
            Put32(b, 0x06064b50);
            Put64(b, 44);
            Put16(b, 0x003F);
            Put16(b, 45);
            Put32(b, 0);
            Put32(b, 0);
            Put64(b, n);
            Put64(b, n);
            Put64(b, cdSize);
            Put64(b, cdOff);
            Put32(b, 0x07064b50);
            Put32(b, 0);
            Put64(b, z64);
            Put32(b, 1);
        }
        Put32(b, 0x06054b50);
        Put16(b, 0);
        Put16(b, 0);
        Put16(b, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
        Put16(b, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
        Put32(b, (uint32_t)std::min<uint64_t>(cdSize, 0xFFFFFFFF));
        Put32(b, (uint32_t)std::min<uint64_t>(cdOff, 0xFFFFFFFF));
        Put16(b, 0);
        if (!err && !(out.Write(b.data(), b.size()) && out.Flush())) err = out.err();
    }
    CloseHandle(h);
    if (err) {
        DeleteFileW(ToNtPath(zip).c_str());
        if (detail && !failed.empty()) *detail = L"« " + failed + L" »";
        return err;
    }
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, zip.c_str(), nullptr);
    return 0;
}
