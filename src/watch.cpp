#include "watch.h"

#include "paths.h"

#include <memory>

namespace {

struct Args {
    HWND target;
    UINT msg;
    std::wstring path;
    uintptr_t id;
    HANDLE stop;  // copie propre au thread, fermée par lui
};

// Copie de l'interface. Chaque côté possède son handle (DuplicateHandle) :
// aucun ne peut signaler un handle déjà fermé, ou réutilisé par Windows.
HANDLE g_stop = nullptr;

DWORD WINAPI Worker(void* p) {
    std::unique_ptr<Args> a(static_cast<Args*>(p));
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_LAST_WRITE;
    HANDLE ch = FindFirstChangeNotificationW(ToNtPath(a->path).c_str(), FALSE, kFilter);
    if (ch != INVALID_HANDLE_VALUE) {
        HANDLE hs[2] = {a->stop, ch};
        while (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            PostMessageW(a->target, a->msg, a->id, 0);
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
    }
    CloseHandle(a->stop);
    return 0;
}

}  // namespace

void Watch_Start(HWND target, UINT msg, const std::wstring& path, uintptr_t id) {
    Watch_Stop();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return;
    HANDLE mine = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), stop, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(stop);
        return;
    }
    auto* a = new Args{target, msg, path, id, stop};
    HANDLE th = CreateThread(nullptr, 0, Worker, a, 0, nullptr);
    if (!th) {
        CloseHandle(stop);
        CloseHandle(mine);
        delete a;
        return;
    }
    CloseHandle(th);
    g_stop = mine;
}

void Watch_Stop() {
    if (!g_stop) return;
    SetEvent(g_stop);
    CloseHandle(g_stop);
    g_stop = nullptr;
}
