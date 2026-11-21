#include "listing.h"

#include "nt.h"
#include "paths.h"
#include "util.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <execution>
#include <memory>
#include <thread>

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
        for (uint32_t i = n; i-- > 0;)
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

std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc) {
    const uint32_t n = (uint32_t)d.entries.size();
    std::vector<uint32_t> order;
    order.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
        if (d.entries[i].isDir()) order.push_back(i);
    const size_t nDirs = order.size();
    for (uint32_t i = 0; i < n; ++i)
        if (!d.entries[i].isDir()) order.push_back(i);

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
