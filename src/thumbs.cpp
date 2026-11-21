#include "thumbs.h"

#include <objbase.h>
#include <shobjidl.h>

#include <atomic>
#include <vector>

namespace {

struct Request {
    uint64_t serial;
    uint32_t idx;
    int px;
    std::wstring path;
};

CRITICAL_SECTION g_cs;
HANDLE g_wake = nullptr;
std::vector<Request> g_queue;  // pile : la demande la plus récente d'abord
std::atomic<uint64_t> g_serial{0};
std::atomic<HWND> g_target{nullptr};
UINT g_msg = 0;

HBITMAP Extract(const std::wstring& path, int px) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP bmp = nullptr;
    // Miniature seulement (pas d'icône de secours : on a déjà l'icône du type).
    if (FAILED(f->GetImage(SIZE{px, px}, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT, &bmp))) bmp = nullptr;
    f->Release();
    return bmp;
}

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
            HBITMAP bmp = Extract(r.path, r.px);
            auto* res = new ThumbResult{r.serial, r.idx, r.px, bmp};
            HWND h = g_target.load();
            if (!h || !PostMessageW(h, g_msg, 0, (LPARAM)res)) {
                if (bmp) DeleteObject(bmp);
                delete res;
            }
        }
    }
}

}  // namespace

void Thumbs_Start(HWND target, UINT msg) {
    g_msg = msg;
    g_target.store(target);
    if (g_wake) return;
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Deux threads : un fichier lent (vidéo sur disque dur) ne bloque pas les autres.
    for (int i = 0; i < 2; ++i)
        if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(h);
        }
}

void Thumbs_Stop() { g_target.store(nullptr); }

void Thumbs_SetSerial(uint64_t serial) {
    g_serial.store(serial);
    if (!g_wake) return;
    EnterCriticalSection(&g_cs);
    g_queue.clear();
    LeaveCriticalSection(&g_cs);
}

void Thumbs_Request(uint64_t serial, uint32_t idx, std::wstring path, int px) {
    if (!g_wake || serial != g_serial.load()) return;
    EnterCriticalSection(&g_cs);
    g_queue.push_back(Request{serial, idx, px, std::move(path)});
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
    SetEvent(g_wake);  // réveille aussi le second thread
}

bool Thumbs_Wanted(uint32_t attr, uint64_t size) {
    constexpr uint32_t kNotLocal =
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    return !(attr & (FILE_ATTRIBUTE_DIRECTORY | kNotLocal)) && size > 0;
}
