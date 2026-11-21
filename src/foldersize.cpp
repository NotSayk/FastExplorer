#include "foldersize.h"

#include "paths.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <memory>
#include <mutex>

bool FolderSize_Compute(const std::wstring& dir, FolderSize& out, const std::function<bool()>& stop) {
    out = {};
    std::vector<std::wstring> todo{ToNtPath(dir)};
    WIN32_FIND_DATAW fd;
    unsigned checks = 0;
    while (!todo.empty()) {
        std::wstring cur = std::move(todo.back());
        todo.pop_back();
        if (stop && stop()) return false;
        HANDLE h = FindFirstFileExW(Join(cur, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) out.complete = false;  // accès refusé, disparu…
            continue;
        }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.' &&
                    (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // jonction, lien : pas suivi
                ++out.dirs;
                todo.push_back(Join(cur, fd.cFileName));
            } else {
                ++out.files;
                out.bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
            if ((++checks & 1023) == 0 && stop && stop()) {
                FindClose(h);
                return false;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

namespace {

struct Job {
    std::wstring path;
    std::shared_ptr<std::atomic<bool>> stop;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job> g_queue;
std::vector<Job> g_running;
std::atomic<uint64_t> g_ticket{0};
std::atomic<HWND> g_target{nullptr};
UINT g_msg = 0;
bool g_started = false;
constexpr int kWorkers = 3;

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

void Worker() {
    // E/S et processeur en priorité d'arrière-plan : la navigation passe avant.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(g_mu);
            g_cv.wait(lock, [] { return !g_queue.empty(); });
            job = std::move(g_queue.front());
            g_queue.pop_front();
            g_running.push_back(job);
        }
        FolderSize fs;
        const bool done = FolderSize_Compute(job.path, fs, [&] { return job.stop->load(std::memory_order_relaxed); });
        {
            std::lock_guard lock(g_mu);
            for (size_t i = 0; i < g_running.size(); ++i)
                if (g_running[i].stop == job.stop) {
                    g_running.erase(g_running.begin() + (ptrdiff_t)i);
                    break;
                }
        }
        if (!done) continue;
        auto* r = new DirSizeResult{g_ticket.load(), std::move(job.path), fs};
        HWND h = g_target.load();
        if (!h || !PostMessageW(h, g_msg, 0, (LPARAM)r)) delete r;
    }
}

}  // namespace

void DirSizes_Start(HWND target, UINT msg) {
    g_msg = msg;
    g_target.store(target);
    std::lock_guard lock(g_mu);
    if (g_started) return;
    g_started = true;
    for (int i = 0; i < kWorkers; ++i) std::thread(Worker).detach();
}

void DirSizes_Stop() {
    g_target.store(nullptr);
    std::lock_guard lock(g_mu);
    g_queue.clear();
    for (Job& j : g_running) j.stop->store(true);
}

void DirSizes_Request(uint64_t ticket, std::vector<std::wstring> dirs) {
    std::lock_guard lock(g_mu);
    g_ticket.store(ticket);
    g_queue.clear();
    // Calculs en cours : gardés s'ils sont encore demandés, arrêtés sinon.
    for (Job& j : g_running) {
        bool wanted = false;
        for (auto it = dirs.begin(); it != dirs.end(); ++it)
            if (SamePath(*it, j.path)) {
                wanted = true;
                dirs.erase(it);
                break;
            }
        if (!wanted) j.stop->store(true);
    }
    for (std::wstring& d : dirs) g_queue.push_back(Job{std::move(d), std::make_shared<std::atomic<bool>>(false)});
    g_cv.notify_all();
}
