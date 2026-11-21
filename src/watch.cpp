#include "watch.h"

#include "paths.h"

#include <dbt.h>

#include <memory>
#include <mutex>

namespace {

struct Args {
    HWND target;
    UINT msg;
    std::wstring path;
    uintptr_t id;
    HANDLE stop;  // copie propre au thread, fermée par lui
};

// ── Côté interface ──
// Chaque côté possède ses handles (DuplicateHandle) : aucun ne peut signaler
// un handle déjà fermé, ou réutilisé par Windows.
HANDLE g_stop = nullptr;
HANDLE g_thread = nullptr;
uintptr_t g_id = 0;
HANDLE g_dir = nullptr;        // copie du handle du dossier, inscrite pour l'éjection
HDEVNOTIFY g_notify = nullptr;

// ── Échange thread → interface : copie du handle du dossier, une fois ouvert ──
std::mutex g_offerLock;
uintptr_t g_offerId = 0;
HANDLE g_offer = nullptr;

void Offer(uintptr_t id, HANDLE dup) {
    std::lock_guard<std::mutex> l(g_offerLock);
    if (g_offer) CloseHandle(g_offer);  // jamais réclamée (surveillance déjà remplacée)
    g_offer = dup;
    g_offerId = id;
}

DWORD WINAPI Worker(void* p) {
    std::unique_ptr<Args> a(static_cast<Args*>(p));
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_LAST_WRITE;
    HANDLE dir = CreateFileW(ToNtPath(a->path).c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool gone = false;
    if (dir != INVALID_HANDLE_VALUE && ev) {
        // L'interface inscrit une copie du handle pour être prévenue avant une éjection.
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), dir, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            Offer(a->id, dup);
            PostMessageW(a->target, a->msg, a->id, kWatchOpened);
        }
        // Seul le fait qu'il y ait un changement compte : petit tampon ; un
        // débordement (0 octet renvoyé) est un changement comme un autre.
        alignas(DWORD) BYTE buf[4096];
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            if (!ReadDirectoryChangesW(dir, buf, sizeof buf, FALSE, kFilter, nullptr, &ov, nullptr)) {
                gone = true;
                break;
            }
            HANDLE hs[2] = {a->stop, ev};
            const DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            DWORD n = 0;
            if (w != WAIT_OBJECT_0 + 1) {  // arrêt demandé
                CancelIoEx(dir, &ov);
                GetOverlappedResult(dir, &ov, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(dir, &ov, &n, FALSE)) {  // dossier supprimé, média retiré, réseau perdu
                gone = true;
                break;
            }
            PostMessageW(a->target, a->msg, a->id, 0);
        }
    } else {
        gone = true;
    }
    if (ev) CloseHandle(ev);
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    {  // copie offerte jamais réclamée : elle garderait le lecteur ouvert
        std::lock_guard<std::mutex> l(g_offerLock);
        if (g_offer && g_offerId == a->id) {
            CloseHandle(g_offer);
            g_offer = nullptr;
        }
    }
    // Pas de « disparu » si l'arrêt a été demandé entre-temps.
    if (gone && WaitForSingleObject(a->stop, 0) != WAIT_OBJECT_0) PostMessageW(a->target, a->msg, a->id, kWatchGone);
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
    g_thread = th;
    g_stop = mine;
    g_id = id;
}

void Watch_Opened(HWND target, uintptr_t id) {
    HANDLE dup = nullptr;
    {
        std::lock_guard<std::mutex> l(g_offerLock);
        if (g_offerId == id) std::swap(dup, g_offer);
    }
    if (!dup) return;
    if (id != g_id || !g_stop || g_dir) {  // surveillance déjà arrêtée ou remplacée
        CloseHandle(dup);
        return;
    }
    g_dir = dup;
    DEV_BROADCAST_HANDLE dbh{};
    dbh.dbch_size = sizeof dbh;
    dbh.dbch_devicetype = DBT_DEVTYP_HANDLE;
    dbh.dbch_handle = dup;
    g_notify = RegisterDeviceNotificationW(target, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);  // échoue sur un partage réseau
}

void Watch_Stop(bool wait) {
    // Comme le recommande la documentation de l'éjection : fermer et désinscrire
    // sur le thread de la fenêtre, pendant le traitement de DBT_DEVICEQUERYREMOVE.
    if (g_notify) UnregisterDeviceNotification(g_notify);
    g_notify = nullptr;
    if (g_dir) CloseHandle(g_dir);
    g_dir = nullptr;
    if (!g_stop) return;
    SetEvent(g_stop);
    CloseHandle(g_stop);
    g_stop = nullptr;
    // Le lecteur n'est libre qu'une fois le handle du thread fermé lui aussi.
    // (Borné : un partage réseau peut tarder à annuler ; on n'attend pas plus.)
    if (wait) WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}
