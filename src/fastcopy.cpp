#include "fastcopy.h"

#include "paths.h"

#include <winioctl.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace {

bool IsFsPath(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') return true;
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.';  // \\serveur\partage
}

bool Exists(const std::wstring& p, DWORD* attrs = nullptr) {
    const DWORD a = GetFileAttributesW(ToNtPath(p).c_str());
    if (attrs) *attrs = a;
    if (a != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD e = GetLastError();
    return !(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);  // autre erreur : prudence, « existe »
}

bool StartsWithDir(const std::wstring& p, const std::wstring& dir) {
    if (p.size() < dir.size() || _wcsnicmp(p.c_str(), dir.c_str(), dir.size()) != 0) return false;
    return p.size() == dir.size() || p[dir.size()] == L'\\' || (!dir.empty() && dir.back() == L'\\');
}

std::wstring Volume(const std::wstring& p) {
    wchar_t v[MAX_PATH] = {};
    return GetVolumePathNameW(p.c_str(), v, MAX_PATH) ? std::wstring(v) : std::wstring();
}

struct File {
    std::wstring src, dst;
    uint64_t size = 0;
    bool done = false;  // copié (écrit par un seul thread, lu après la fin des threads)
};

struct Top {
    std::wstring src, dst, name;
    bool dir = false;
    std::vector<std::pair<std::wstring, std::wstring>> dirs;  // sous-dossiers (parents d'abord)
    std::vector<File> files;
    bool topCreated = false;
    std::atomic<bool> failed{false};
};

// Inventaire d'un dossier. false : le Shell doit s'en charger (lien, jonction,
// fichier cloud, dossier illisible).
bool Inventory(Top& t, const FastProgress& prog) {
    std::vector<std::pair<std::wstring, std::wstring>> stack{{t.src, t.dst}};
    while (!stack.empty()) {
        auto [s, d] = std::move(stack.back());
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(ToNtPath(Join(s, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;  // dossier vide
            return false;
        }
        do {
            if (prog.cancel) {
                FindClose(h);
                return false;
            }
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                FindClose(h);
                return false;
            }
            std::wstring cs = Join(s, n), cd = Join(d, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                t.dirs.emplace_back(cs, cd);
                stack.emplace_back(std::move(cs), std::move(cd));
            } else {
                t.files.push_back({std::move(cs), std::move(cd),
                                   ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
            }
        } while (FindNextFileW(h, &fd));
        const DWORD e = GetLastError();
        FindClose(h);
        if (e != ERROR_NO_MORE_FILES) return false;
    }
    return true;
}

// Pause demandée (file d'attente des copies) : on attend, sans rien fermer.
void WaitWhilePaused(const FastProgress& p) {
    while (p.pause && !p.cancel) Sleep(50);
}

struct CopyCtx {
    FastProgress* prog;
    uint64_t last = 0;
};

DWORD CALLBACK OnProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE,
                          HANDLE, LPVOID p) {
    auto* c = (CopyCtx*)p;
    const uint64_t now = (uint64_t)done.QuadPart;
    c->prog->bytesDone += now - c->last;
    c->last = now;
    WaitWhilePaused(*c->prog);
    return c->prog->cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

void ClearReadOnly(const std::wstring& nt) {
    const DWORD a = GetFileAttributesW(nt.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(nt.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
}

// Retire ce que l'on a créé pour cet élément (il n'existait pas avant nous).
void Undo(Top& t) {
    for (auto& f : t.files)
        if (f.done) {
            const std::wstring nt = ToNtPath(f.dst);
            ClearReadOnly(nt);
            DeleteFileW(nt.c_str());
        }
    for (auto it = t.dirs.rbegin(); it != t.dirs.rend(); ++it) RemoveDirectoryW(ToNtPath(it->second).c_str());
    if (t.topCreated) RemoveDirectoryW(ToNtPath(t.dst).c_str());  // échoue s'il reste quelque chose : on garde
}

// Déplacement entre volumes : la source est supprimée une fois tout copié.
bool DeleteSource(Top& t) {
    bool ok = true;
    for (auto& f : t.files) {
        const std::wstring nt = ToNtPath(f.src);
        ClearReadOnly(nt);
        ok &= DeleteFileW(nt.c_str()) != FALSE;
    }
    for (auto it = t.dirs.rbegin(); it != t.dirs.rend(); ++it) ok &= RemoveDirectoryW(ToNtPath(it->first).c_str()) != FALSE;
    if (t.dir) {
        ok &= RemoveDirectoryW(ToNtPath(t.src).c_str()) != FALSE;
    } else {
        const std::wstring nt = ToNtPath(t.src);
        ClearReadOnly(nt);
        ok &= DeleteFileW(nt.c_str()) != FALSE;
    }
    return ok;
}

bool SeekPenalty(const std::wstring& path, bool& penalty) {
    const std::wstring vol = Volume(path);  // « C:\ »
    if (vol.size() < 2 || vol[1] != L':') return false;
    const std::wstring dev = L"\\\\.\\" + vol.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD n = 0;
    const bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &n, nullptr) &&
                    n >= sizeof d;
    CloseHandle(h);
    if (ok) penalty = d.IncursSeekPenalty != FALSE;
    return ok;
}

}  // namespace

int FastCopyThreads(const std::wstring& src, const std::wstring& dest) {
    auto remote = [](const std::wstring& p) {
        if (p.size() > 1 && p[0] == L'\\') return true;
        const std::wstring v = Volume(p);
        return !v.empty() && GetDriveTypeW(v.c_str()) == DRIVE_REMOTE;
    };
    if (remote(src) || remote(dest)) return 4;
    bool a = false, b = false;
    SeekPenalty(src, a);
    SeekPenalty(dest, b);
    return (a || b) ? 1 : 8;  // disque dur : des lectures parallèles font sauter la tête de lecture
}

bool FastTransfer(const std::vector<std::wstring>& sources, const std::wstring& dest, bool move, FastProgress& prog,
                  FastOutcome& out) {
    if (sources.empty() || !IsFsPath(dest)) return false;
    DWORD da = 0;
    if (!Exists(dest, &da) || da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) return false;

    // ── Vérifications : au moindre doute, tout passe par le Shell ──
    std::vector<std::unique_ptr<Top>> tops;
    for (const auto& s : sources) {
        std::wstring parent, name;
        if (!IsFsPath(s) || !ParentOf(s, parent, name) || name.empty()) return false;
        if (_wcsicmp(parent.c_str(), dest.c_str()) == 0) return false;  // même dossier : « - Copie » du Shell
        if (StartsWithDir(dest, s)) return false;                        // dossier copié dans lui-même
        DWORD a = 0;
        if (!Exists(s, &a) || a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        auto t = std::make_unique<Top>();
        t->src = s;
        t->name = name;
        t->dst = Join(dest, name.c_str());
        t->dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (Exists(t->dst)) return false;  // conflit : la fenêtre « Remplacer ou ignorer » du Shell
        for (const auto& o : tops)
            if (_wcsicmp(o->name.c_str(), name.c_str()) == 0) return false;  // deux sources de même nom
        tops.push_back(std::move(t));
    }
    const bool sameVolume = move && _wcsicmp(Volume(sources[0]).c_str(), Volume(dest).c_str()) == 0 &&
                            std::all_of(sources.begin(), sources.end(), [&](const std::wstring& s) {
                                return _wcsicmp(Volume(s).c_str(), Volume(sources[0]).c_str()) == 0;
                            });

    // ── Déplacement sur un même volume : un simple renommage par élément ──
    if (sameVolume) {
        prog.filesTotal = (uint32_t)tops.size();
        for (auto& t : tops) {
            WaitWhilePaused(prog);
            if (prog.cancel) {
                out.cancelled = true;
                break;
            }
            if (MoveFileExW(ToNtPath(t->src).c_str(), ToNtPath(t->dst).c_str(), 0)) out.created.push_back(t->name);
            else out.shellLeft.push_back(t->src);
            ++prog.filesDone;
        }
        return true;
    }

    // ── Copie (ou déplacement entre volumes) : inventaire complet d'abord ──
    for (auto& t : tops) {
        if (t->dir) {
            if (!Inventory(*t, prog)) {
                if (!prog.cancel) return false;
                out.cancelled = true;  // annulé pendant l'inventaire : rien n'a été écrit
                return true;
            }
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(ToNtPath(t->src).c_str(), GetFileExInfoStandard, &fa)) return false;
            t->files.push_back({t->src, t->dst, ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow});
        }
        for (auto& f : t->files) prog.bytesTotal += f.size;
        prog.filesTotal += (uint32_t)t->files.size();
    }
    // Pas assez de place : on n'écrit rien, le Shell le dit avant de commencer (comme l'Explorateur).
    ULARGE_INTEGER avail{};
    if (GetDiskFreeSpaceExW(dest.c_str(), &avail, nullptr, nullptr) && avail.QuadPart < prog.bytesTotal + (1ull << 20))
        return false;

    // Dossiers (parents d'abord), avec les attributs de la source (masqué, compressé…).
    for (auto& t : tops) {
        if (!t->dir) continue;
        if (!CreateDirectoryExW(ToNtPath(t->src).c_str(), ToNtPath(t->dst).c_str(), nullptr)) {
            t->failed = true;
            continue;
        }
        t->topCreated = true;
        for (auto& [s, d] : t->dirs)
            if (!CreateDirectoryExW(ToNtPath(s).c_str(), ToNtPath(d).c_str(), nullptr)) {
                t->failed = true;
                break;
            }
    }

    struct Job {
        Top* top;
        File* file;
    };
    std::vector<Job> jobs;
    for (auto& t : tops)
        for (auto& f : t->files) jobs.push_back({t.get(), &f});

    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i; (i = next++) < jobs.size();) {
            Job& j = jobs[i];
            WaitWhilePaused(prog);
            if (prog.cancel) return;
            if (j.top->failed) continue;
            CopyCtx ctx{&prog};
            // Échec : le fichier partiel est supprimé par CopyFileExW.
            if (CopyFileExW(ToNtPath(j.file->src).c_str(), ToNtPath(j.file->dst).c_str(), OnProgress, &ctx, nullptr,
                            COPY_FILE_FAIL_IF_EXISTS)) {
                j.file->done = true;
                ++prog.filesDone;
            } else if (!prog.cancel) {
                j.top->failed = true;
            }
        }
    };
    const int n = (int)std::min<size_t>(jobs.size(), (size_t)FastCopyThreads(sources[0], dest));
    std::vector<std::thread> pool;
    for (int k = 1; k < n; ++k) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();

    out.cancelled = prog.cancel;
    for (auto& t : tops) {
        const bool complete = !t->failed && std::all_of(t->files.begin(), t->files.end(), [](const File& f) { return f.done; });
        if (complete) {
            out.created.push_back(t->name);
            if (move && !DeleteSource(*t)) out.error = ERROR_SHARING_VIOLATION;
        } else if (out.cancelled) {
            if (t->topCreated || std::any_of(t->files.begin(), t->files.end(), [](const File& f) { return f.done; }))
                out.created.push_back(t->name);  // annulé : ce qui est copié reste, comme dans l'Explorateur
        } else {
            Undo(*t);
            out.shellLeft.push_back(t->src);
        }
    }
    return true;
}
