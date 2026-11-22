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
    bool done = false;    // copié (écrit par un seul thread, lu après la fin des threads)
    bool failed = false;  // échec définitif : rendu au Shell
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

// Comparaison octet à octet ; counted : octets relus comptés dans prog.
bool VerifyCounting(const std::wstring& src, const std::wstring& dst, FastProgress* prog, uint64_t& counted) {
    HANDLE a = CreateFileW(ToNtPath(src).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (a == INVALID_HANDLE_VALUE) return false;
    // Sans le cache : on relit ce qui est sur le disque, pas ce qui vient d'être écrit en mémoire.
    HANDLE b = CreateFileW(ToNtPath(dst).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (b == INVALID_HANDLE_VALUE) {
        CloseHandle(a);
        return false;
    }
    LARGE_INTEGER sa{}, sb{};
    bool same = GetFileSizeEx(a, &sa) && GetFileSizeEx(b, &sb) && sa.QuadPart == sb.QuadPart;
    constexpr DWORD kChunk = 1 << 20;  // multiple de la taille de secteur (lecture sans cache)
    auto* bufA = (uint8_t*)VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto* bufB = (uint8_t*)VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    same = same && bufA && bufB;
    while (same) {
        if (prog) {
            WaitWhilePaused(*prog);
            if (prog->cancel) {
                same = false;
                break;
            }
        }
        DWORD na = 0, nb = 0;
        if (!ReadFile(a, bufA, kChunk, &na, nullptr) || !ReadFile(b, bufB, kChunk, &nb, nullptr) || na != nb) {
            same = false;
            break;
        }
        if (!na) break;
        if (memcmp(bufA, bufB, na) != 0) same = false;
        if (prog) prog->bytesDone += na;
        counted += na;
    }
    if (bufA) VirtualFree(bufA, 0, MEM_RELEASE);
    if (bufB) VirtualFree(bufB, 0, MEM_RELEASE);
    CloseHandle(a);
    CloseHandle(b);
    return same;
}

// Petite attente interruptible (annulation) entre deux tentatives.
void Backoff(const FastProgress& p, DWORD ms) {
    for (DWORD w = 0; w < ms && !p.cancel; w += 50) Sleep(50);
}

bool IsRemote(const std::wstring& p) {
    if (p.size() > 1 && p[0] == L'\\') return true;
    const std::wstring v = Volume(p);
    return !v.empty() && GetDriveTypeW(v.c_str()) == DRIVE_REMOTE;
}

// Copie d'un fichier, avec reprise : erreur passagère réessayée, gros fichier réseau repris où il s'était
// arrêté (COPY_FILE_RESTARTABLE), copie vérifiée si demandé. 0 : copié.
DWORD CopyOne(const File& f, FastProgress& prog, bool remote, bool& retried, bool& badVerify) {
    const std::wstring s = ToNtPath(f.src), d = ToNtPath(f.dst);
    const bool restartable = remote && f.size >= (256ull << 20);
    DWORD flags = COPY_FILE_FAIL_IF_EXISTS;
    if (prog.allowDecrypted) flags |= COPY_FILE_ALLOW_DECRYPTED_DESTINATION;
    DWORD e = 0;
    bool verifiedOnce = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        CopyCtx ctx{&prog};
        DWORD fl = flags;
        if (restartable) fl |= COPY_FILE_RESTARTABLE;
        if (restartable && attempt > 0) fl &= ~COPY_FILE_FAIL_IF_EXISTS;  // la copie partielle reprend
        if (CopyFileExW(s.c_str(), d.c_str(), OnProgress, &ctx, nullptr, fl)) {
            uint64_t checked = 0;
            if (!prog.verify || VerifyCounting(f.src, f.dst, &prog, checked)) {
                retried = attempt > 0;
                return 0;
            }
            // Copie différente de l'original : refaite une fois, puis abandonnée au Shell.
            ClearReadOnly(d);
            DeleteFileW(d.c_str());
            prog.bytesDone -= ctx.last + checked;
            if (verifiedOnce) {
                badVerify = true;
                return ERROR_CRC;
            }
            verifiedOnce = true;
            continue;
        }
        e = GetLastError();
        // Ce que cette tentative avait compté ne vaut plus (sauf reprise d'un gros fichier).
        if (!restartable) prog.bytesDone -= ctx.last;
        if (prog.cancel || !FastTransient(e)) return e;
        // Partage : un seul court essai de plus (un fichier ouvert longtemps ne se libérera pas) ; réseau : 1, 2, 4 s.
        const bool share = e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
        if (share && attempt > 0) return e;
        Backoff(prog, share ? 300 : 1000u << attempt);
        if (prog.cancel) return ERROR_CANCELLED;
        if (!restartable) {
            DWORD a = GetFileAttributesW(d.c_str());
            if (a != INVALID_FILE_ATTRIBUTES) {  // copie partielle restée (réseau coupé) : on repart de zéro
                ClearReadOnly(d);
                DeleteFileW(d.c_str());
            }
        }
    }
    return e;
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

// Déplacement entre volumes : la source est supprimée une fois copiée (fichiers en échec exceptés :
// le Shell les déplace ensuite, et leurs dossiers, non vides, restent).
bool DeleteSource(Top& t) {
    bool ok = true;
    bool partial = false;
    for (auto& f : t.files) {
        if (!f.done) {
            partial = true;
            continue;
        }
        const std::wstring nt = ToNtPath(f.src);
        ClearReadOnly(nt);
        ok &= DeleteFileW(nt.c_str()) != FALSE;
    }
    for (auto it = t.dirs.rbegin(); it != t.dirs.rend(); ++it)
        ok &= RemoveDirectoryW(ToNtPath(it->first).c_str()) != FALSE || partial;
    if (partial) {
        if (t.dir) RemoveDirectoryW(ToNtPath(t.src).c_str());
        return ok;
    }
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

bool FastTransient(DWORD e) {
    switch (e) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_NETNAME_DELETED:
        case ERROR_UNEXP_NET_ERR:
        case ERROR_SEM_TIMEOUT:
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_BAD_NET_RESP:
        case ERROR_BAD_NETPATH:
        case ERROR_NETWORK_BUSY:
        case ERROR_CONNECTION_ABORTED:
        case ERROR_VC_DISCONNECTED:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_REQ_NOT_ACCEP:
            return true;
        default: return false;
    }
}

bool FastVerify(const std::wstring& src, const std::wstring& dst, FastProgress* prog) {
    uint64_t n = 0;
    return VerifyCounting(src, dst, prog, n);
}

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
            else out.shellLeft.emplace_back(t->src, dest);
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
        for (auto& f : t->files) prog.bytesTotal += prog.verify ? 2 * f.size : f.size;
        prog.filesTotal += (uint32_t)t->files.size();
    }
    // Pas assez de place : on n'écrit rien, le Shell le dit avant de commencer (comme l'Explorateur).
    ULARGE_INTEGER avail{};
    if (GetDiskFreeSpaceExW(dest.c_str(), &avail, nullptr, nullptr) &&
        avail.QuadPart < (prog.verify ? prog.bytesTotal / 2 : (uint64_t)prog.bytesTotal) + (1ull << 20))
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

    const bool remote = IsRemote(sources[0]) || IsRemote(dest);
    std::atomic<uint32_t> retried{0}, badVerify{0};
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i; (i = next++) < jobs.size();) {
            Job& j = jobs[i];
            WaitWhilePaused(prog);
            if (prog.cancel) return;
            if (j.top->failed) continue;  // dossiers non créés : tout l'élément revient au Shell
            bool again = false, bad = false;
            const DWORD e = CopyOne(*j.file, prog, remote, again, bad);
            if (!e) {
                j.file->done = true;
                ++prog.filesDone;
                if (again) ++retried;
            } else if (!prog.cancel) {
                j.file->failed = true;  // seul ce fichier revient au Shell
                if (bad) ++badVerify;
            }
        }
    };
    const int n = (int)std::min<size_t>(jobs.size(), (size_t)FastCopyThreads(sources[0], dest));
    std::vector<std::thread> pool;
    for (int k = 1; k < n; ++k) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();

    out.cancelled = prog.cancel;
    out.retried = retried;
    out.verifyFailed = badVerify;
    for (auto& t : tops) {
        const bool complete = !t->failed && std::all_of(t->files.begin(), t->files.end(), [](const File& f) { return f.done; });
        const bool someDone = std::any_of(t->files.begin(), t->files.end(), [](const File& f) { return f.done; });
        if (complete) {
            out.created.push_back(t->name);
            if (move && !DeleteSource(*t)) out.error = ERROR_SHARING_VIOLATION;
        } else if (out.cancelled) {
            if (t->topCreated || someDone) out.created.push_back(t->name);  // annulé : ce qui est copié reste
        } else if (!t->failed && t->dir && someDone) {
            // Reprise : ce qui est copié reste ; seuls les fichiers en échec vont au Shell, vers leur dossier.
            out.created.push_back(t->name);
            for (const auto& f : t->files)
                if (!f.done) {
                    std::wstring parent, leaf;
                    if (ParentOf(f.dst, parent, leaf)) out.shellLeft.emplace_back(f.src, parent);
                }
            if (move && !DeleteSource(*t)) out.error = ERROR_SHARING_VIOLATION;
        } else {
            Undo(*t);
            out.shellLeft.emplace_back(t->src, dest);
        }
    }
    return true;
}
