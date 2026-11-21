#include "icons.h"

#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <vector>

namespace {

struct Request {
    uint64_t serial;
    uint32_t idx;
    std::wstring path;
};

CRITICAL_SECTION g_cs;
HANDLE g_wake = nullptr;
std::vector<Request> g_queue;  // pile : on sert la demande la plus récente
std::atomic<uint64_t> g_serial{0};
std::atomic<HWND> g_target{nullptr};
UINT g_msg = 0;

DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            Request r;
            EnterCriticalSection(&g_cs);
            if (g_queue.empty()) {
                LeaveCriticalSection(&g_cs);
                break;
            }
            r = std::move(g_queue.back());
            g_queue.pop_back();
            LeaveCriticalSection(&g_cs);
            if (r.serial != g_serial.load()) continue;

            SHFILEINFOW sfi{};
            // SHGFI_ICON est requis pour obtenir l'index de surimpression.
            if (!SHGetFileInfoW(r.path.c_str(), 0, &sfi, sizeof sfi,
                                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_ICON | SHGFI_OVERLAYINDEX))
                continue;
            if (sfi.hIcon) DestroyIcon(sfi.hIcon);
            auto* res = new IconResult{r.serial, r.idx, sfi.iIcon & 0x00FFFFFF, (sfi.iIcon >> 24) & 0xFF};
            HWND h = g_target.load();
            if (!h || !PostMessageW(h, g_msg, 0, (LPARAM)res)) delete res;
        }
    }
}

bool ExtIs(const wchar_t* ext, uint32_t len, const wchar_t* want) {
    const size_t n = wcslen(want);
    return n == len && _wcsnicmp(ext, want, n) == 0;
}

}  // namespace

bool NeedsOwnIcon(uint32_t attr, const wchar_t* ext, uint32_t extLen) {
    constexpr uint32_t kNotLocal =
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    if (attr & kNotLocal) return false;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return true;  // liens, fichiers cloud locaux : surimpression
    if (attr & FILE_ATTRIBUTE_DIRECTORY)                   // desktop.ini possible (icône personnalisée)
        return (attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) != 0;
    static const wchar_t* const kOwn[] = {L".exe", L".lnk", L".ico", L".url", L".cur",
                                          L".ani", L".scr", L".cpl", L".appref-ms"};
    for (const wchar_t* k : kOwn)
        if (ExtIs(ext, extLen, k)) return true;
    return false;
}

void Icons_Start(HWND target, UINT msg) {
    g_msg = msg;
    g_target.store(target);
    if (g_wake) return;
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
    }
}

void Icons_Stop() { g_target.store(nullptr); }

void Icons_SetSerial(uint64_t serial) {
    g_serial.store(serial);
    if (!g_wake) return;
    EnterCriticalSection(&g_cs);
    g_queue.clear();
    LeaveCriticalSection(&g_cs);
}

void Icons_Request(uint64_t serial, uint32_t idx, std::wstring path) {
    if (!g_wake || serial != g_serial.load()) return;
    EnterCriticalSection(&g_cs);
    g_queue.push_back(Request{serial, idx, std::move(path)});
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
}
