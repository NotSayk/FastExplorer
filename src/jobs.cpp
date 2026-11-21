#include "jobs.h"

#include <objbase.h>

#include <atomic>
#include <memory>

namespace {
std::atomic<uint64_t> g_gen{0};
HANDLE g_current = nullptr;  // touché uniquement par le thread de l'interface

DWORD WINAPI Thunk(void* p) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(p));
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (*fn)();
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}
}  // namespace

CancelToken Jobs_NewGeneration() {
    const uint64_t gen = ++g_gen;
    if (g_current) {
        CancelSynchronousIo(g_current);  // sans effet si le thread n'est pas en E/S ou est terminé
        CloseHandle(g_current);
        g_current = nullptr;
    }
    return CancelToken{&g_gen, gen};
}

uint64_t Jobs_Current() { return g_gen.load(); }

void Jobs_Spawn(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(nullptr, 0, Thunk, p, 0, nullptr);
    if (!h) {  // ressources épuisées : on exécute sur place plutôt que perdre la tâche
        (*p)();
        delete p;
        return;
    }
    if (g_current) CloseHandle(g_current);
    g_current = h;
}
