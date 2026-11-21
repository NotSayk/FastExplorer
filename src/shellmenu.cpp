#include "shellmenu.h"

#include "paths.h"

#include <shellapi.h>  // SEE_MASK_* (utilisés par CMIC_MASK_*)
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

Pidl ParsePath(const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    const HRESULT hr = path.empty() ? SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p)
                                    : SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr);
    return Pidl(SUCCEEDED(hr) ? (ITEMIDLIST*)p : nullptr);  // retire __unaligned (sans effet sur x64)
}

bool ShellObjectOf(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, REFIID riid,
                   void** out) {
    *out = nullptr;
    // Les éléments partagent le même parent : on demande au dossier parent
    // un seul objet pour toute la sélection.
    std::vector<Pidl> abs;
    abs.reserve(names.size());
    for (const auto& n : names) {
        Pidl p = ParsePath(Join(folder, n.c_str()));
        if (!p) return false;
        abs.push_back(std::move(p));
    }
    if (abs.empty()) return false;
    // Éléments de dossiers différents (résultats de recherche) : un objet de
    // données reste possible (copier, glisser) ; un menu contextuel, non.
    bool sameParent = true;
    {
        Pidl p0((ITEMIDLIST*)ILClone(abs[0].get()));
        ILRemoveLastID(p0.get());
        for (size_t i = 1; i < abs.size() && sameParent; ++i) {
            Pidl pi((ITEMIDLIST*)ILClone(abs[i].get()));
            ILRemoveLastID(pi.get());
            sameParent = ILIsEqual(p0.get(), pi.get()) != FALSE;
        }
    }
    if (!sameParent) {
        if (riid != IID_IDataObject) return false;
        std::vector<PCIDLIST_ABSOLUTE> list;
        for (const auto& p : abs) list.push_back(p.get());
        ComPtr<IShellItemArray> arr;
        if (FAILED(SHCreateShellItemArrayFromIDLists((UINT)list.size(), list.data(), &arr))) return false;
        return SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, riid, out)) && *out;
    }
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD first = nullptr;
    if (FAILED(SHBindToParent(abs[0].get(), IID_PPV_ARGS(&parent), &first))) return false;
    std::vector<PCUITEMID_CHILD> kids;
    kids.reserve(abs.size());
    for (const auto& p : abs) kids.push_back((PCUITEMID_CHILD)ILFindLastID(p.get()));
    return SUCCEEDED(parent->GetUIObjectOf(owner, (UINT)kids.size(), kids.data(), riid, nullptr, out)) && *out;
}

namespace {

constexpr UINT kFirst = 1, kLast = 0x7FFF;

ComPtr<IContextMenu> BuildMenu(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names) {
    ComPtr<IContextMenu> cm;
    if (names.empty()) {  // fond du dossier
        Pidl pf = ParsePath(folder);
        if (!pf) return cm;
        ComPtr<IShellFolder> sf;
        if (FAILED(SHBindToObject(nullptr, pf.get(), nullptr, IID_PPV_ARGS(&sf)))) return cm;
        sf->CreateViewObject(owner, IID_PPV_ARGS(&cm));
        return cm;
    }
    ShellObjectOf(owner, folder, names, IID_IContextMenu, (void**)cm.GetAddressOf());
    return cm;
}

bool IsSeparator(HMENU m, int pos) {
    MENUITEMINFOW mi{};
    mi.cbSize = sizeof mi;
    mi.fMask = MIIM_FTYPE;
    return GetMenuItemInfoW(m, pos, TRUE, &mi) && (mi.fType & MFT_SEPARATOR);
}

// Certaines extensions s'enregistrent deux fois (ex. PowerToys : gestionnaire
// moderne + classique) : on garde la première entrée de chaque libellé, puis
// on retire les séparateurs devenus orphelins (en tête, en fin, ou doublés).
void RemoveDuplicates(HMENU m) {
    std::unordered_set<std::wstring> seen;
    for (int i = 0; i < GetMenuItemCount(m);) {
        wchar_t text[256];
        std::wstring key;  // libellé sans les « & » d'accélérateur
        if (!IsSeparator(m, i) && GetMenuStringW(m, (UINT)i, text, _countof(text), MF_BYPOSITION) > 0) {
            for (const wchar_t* c = text; *c; ++c)
                if (*c != L'&') key += *c;
        }
        if (!key.empty() && !seen.insert(key).second) {
            DeleteMenu(m, (UINT)i, MF_BYPOSITION);
            continue;
        }
        ++i;
    }
    for (int i = GetMenuItemCount(m) - 1; i >= 0; --i) {
        const int n = GetMenuItemCount(m);
        if (IsSeparator(m, i) && (i == 0 || i == n - 1 || IsSeparator(m, i - 1))) DeleteMenu(m, (UINT)i, MF_BYPOSITION);
    }
}

bool Invoke(IContextMenu* cm, HWND owner, UINT offset, const char* verb, const std::wstring& folder, POINT pt,
            bool hasPt, bool ctrl, bool shift) {
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK | (hasPt ? CMIC_MASK_PTINVOKE : 0);
    if (ctrl) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (shift) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = owner;
    std::wstring verbW;
    if (verb) {
        ici.lpVerb = verb;
        for (const char* c = verb; *c; ++c) verbW += (wchar_t)*c;  // les verbes sont en ASCII
        ici.lpVerbW = verbW.c_str();
    } else {
        ici.lpVerb = MAKEINTRESOURCEA(offset);
        ici.lpVerbW = MAKEINTRESOURCEW(offset);
    }
    if (!folder.empty()) ici.lpDirectoryW = folder.c_str();
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    return SUCCEEDED(cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici));
}

struct Built {
    ComPtr<IContextMenu> cm;
    HMENU menu = nullptr;
};

Built Build(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, bool extended) {
    Built b;
    ComPtr<IContextMenu> cm = BuildMenu(owner, folder, names);
    if (!cm) return b;
    HMENU menu = CreatePopupMenu();
    if (!menu) return b;
    // CMF_CANRENAME : « Renommer » apparaît ; le verbe "rename" est intercepté par l'appli.
    const UINT flags = CMF_NORMAL | (extended ? CMF_EXTENDEDVERBS : 0) |
                       (!names.empty() && !folder.empty() ? CMF_CANRENAME : 0);
    if (FAILED(cm->QueryContextMenu(menu, 0, kFirst, kLast, flags))) {
        DestroyMenu(menu);
        return b;
    }
    RemoveDuplicates(menu);
    b.cm = std::move(cm);
    b.menu = menu;
    return b;
}

// ── Thread des menus ──
// Les IContextMenu sont créés, utilisés et libérés sur ce thread STA uniquement.
// Le HMENU, objet du processus, s'affiche sur le thread de l'interface.

constexpr UINT WM_MT_PREPARE = WM_APP + 1;  // posté : construire la dernière cible demandée
constexpr UINT WM_MT_HAS = WM_APP + 2;      // envoyé ; lParam = Target* : préparé ?
constexpr UINT WM_MT_TAKE = WM_APP + 3;     // envoyé ; lParam = Take*
constexpr UINT WM_MT_MENUMSG = WM_APP + 4;  // envoyé ; lParam = MenuMsg*
constexpr UINT WM_MT_VERB = WM_APP + 5;     // envoyé ; lParam = VerbReq*
constexpr UINT WM_MT_INVOKE = WM_APP + 6;   // posté ; lParam = InvokeReq* (libéré par le thread)
constexpr UINT WM_MT_RELEASE = WM_APP + 7;  // posté ; lParam = Shown* : menu refermé
constexpr UINT WM_MT_DISCARD = WM_APP + 8;  // posté ou envoyé

struct Target {
    HWND owner = nullptr;
    std::wstring folder;
    std::vector<std::wstring> names;
    bool extended = false;
};

bool Same(const Target& a, const Target& b) {
    return a.folder == b.folder && a.names == b.names && a.extended == b.extended;
}

// Menu remis à l'interface : vit jusqu'au WM_MT_RELEASE (après l'éventuelle commande).
struct Shown {
    Built b;
    ComPtr<IContextMenu2> cm2;
    ComPtr<IContextMenu3> cm3;
};

struct Take {
    Target t;
    Shown* shown = nullptr;
};

struct MenuMsg {
    Shown* s;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    LRESULT result = 0;
    bool handled = false;
};

struct VerbReq {
    Shown* s;
    UINT offset;
    std::wstring verb;
};

struct InvokeReq {
    Shown* s;
    HWND owner;
    UINT offset;
    std::wstring folder;
    POINT pt;
    bool ctrl, shift;
};

// Au-delà, le menu peut ne plus refléter le disque (on l'oublie aussi à chaque
// rafraîchissement du dossier et à chaque changement du presse-papiers).
constexpr ULONGLONG kPrepMaxAgeMs = 10000;

// ── État propre au thread des menus ──
struct Prepared {
    Target t;
    ULONGLONG tick = 0;
    Built built;
} g_prep;

void DropPrepared() {
    if (g_prep.built.menu) DestroyMenu(g_prep.built.menu);
    g_prep = Prepared{};
}

bool PrepFresh(const Target& t) {
    return g_prep.built.menu && Same(g_prep.t, t) && GetTickCount64() - g_prep.tick < kPrepMaxAgeMs;
}

void PrepareOn(const Target& t) {
    if (PrepFresh(t)) return;
    DropPrepared();
    Built b = Build(t.owner, t.folder, t.names, t.extended);
    if (!b.menu) return;
    g_prep.t = t;
    g_prep.tick = GetTickCount64();
    g_prep.built = std::move(b);
}

// Dernière cible demandée par l'interface : les survols successifs se remplacent,
// le thread ne construit jamais un menu déjà dépassé.
std::mutex g_wantLock;
std::unique_ptr<Target> g_want;
bool g_wantPosted = false;

LRESULT CALLBACK MenuThreadProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MT_PREPARE: {
            std::unique_ptr<Target> t;
            {
                std::lock_guard<std::mutex> l(g_wantLock);
                t = std::move(g_want);
                g_wantPosted = false;
            }
            if (t) PrepareOn(*t);
            return 0;
        }
        case WM_MT_HAS: {
            const Target& t = *(const Target*)lp;
            PrepareOn(t);
            return PrepFresh(t);
        }
        case WM_MT_TAKE: {
            auto* tk = (Take*)lp;
            Built b;
            if (PrepFresh(tk->t)) {
                b = std::move(g_prep.built);
                g_prep.built = Built{};
            }
            DropPrepared();
            if (!b.menu) b = Build(tk->t.owner, tk->t.folder, tk->t.names, tk->t.extended);
            if (!b.menu) return 0;
            auto* s = new Shown{std::move(b)};
            s->b.cm.As(&s->cm3);
            if (!s->cm3) s->b.cm.As(&s->cm2);
            tk->shown = s;
            return 1;
        }
        case WM_MT_MENUMSG: {
            auto* m = (MenuMsg*)lp;
            if (m->s->cm3) {
                LRESULT lr = 0;
                if (SUCCEEDED(m->s->cm3->HandleMenuMsg2(m->msg, m->wp, m->lp, &lr))) {
                    m->result = lr;
                    m->handled = true;
                }
            } else if (m->s->cm2 && m->msg != WM_MENUCHAR &&
                       SUCCEEDED(m->s->cm2->HandleMenuMsg(m->msg, m->wp, m->lp))) {
                m->result = (m->msg == WM_INITMENUPOPUP) ? 0 : TRUE;
                m->handled = true;
            }
            return 0;
        }
        case WM_MT_VERB: {
            auto* v = (VerbReq*)lp;
            wchar_t verb[80] = {};
            if (FAILED(v->s->b.cm->GetCommandString(v->offset, GCS_VERBW, nullptr, (LPSTR)verb, _countof(verb))))
                verb[0] = 0;
            verb[_countof(verb) - 1] = 0;
            v->verb = verb;
            return 0;
        }
        case WM_MT_INVOKE: {
            std::unique_ptr<InvokeReq> r((InvokeReq*)lp);
            Invoke(r->s->b.cm.Get(), r->owner, r->offset, nullptr, r->folder, r->pt, true, r->ctrl, r->shift);
            return 0;
        }
        case WM_MT_RELEASE: {
            auto* s = (Shown*)lp;
            s->cm2.Reset();
            s->cm3.Reset();
            DestroyMenu(s->b.menu);
            delete s;
            return 0;
        }
        case WM_MT_DISCARD:
            DropPrepared();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::atomic<HWND> g_mtWnd{nullptr};

// Fenêtre du thread des menus, créé à la première demande.
HWND MenuThread() {
    static const HWND w = [] {
        struct Start {
            HANDLE ready;
            HWND w = nullptr;
        } s{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!s.ready) return (HWND) nullptr;
        HANDLE th = CreateThread(
            nullptr, 0,
            [](void* p) -> DWORD {
                auto* s = (Start*)p;
                const bool ole = SUCCEEDED(OleInitialize(nullptr));
                WNDCLASSW wc{};
                wc.lpfnWndProc = MenuThreadProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"FastExplorerMenuThread";
                RegisterClassW(&wc);
                HWND wnd = ole ? CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                               wc.hInstance, nullptr)
                               : nullptr;
                s->w = wnd;
                SetEvent(s->ready);  // `s` appartient à l'appelant : plus d'accès après ceci
                if (!wnd) {
                    if (ole) OleUninitialize();
                    return 0;
                }
                MSG m;
                while (GetMessageW(&m, nullptr, 0, 0) > 0) {  // fenêtres créées par les extensions
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                return 0;
            },
            &s, 0, nullptr);
        if (th) {
            WaitForSingleObject(s.ready, INFINITE);
            CloseHandle(th);
        }
        CloseHandle(s.ready);
        g_mtWnd = s.w;
        return s.w;
    }();
    return w;
}

// ── Côté interface : menu en cours d'affichage ──
Shown* g_showing = nullptr;

}  // namespace

bool ShellMenu_Prepare(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, bool extended,
                       bool wait) {
    HWND mt = MenuThread();
    if (!mt) return false;
    if (wait) {
        Target t{owner, folder, names, extended};
        return SendMessageW(mt, WM_MT_HAS, 0, (LPARAM)&t) != 0;
    }
    bool post = false;
    {
        std::lock_guard<std::mutex> l(g_wantLock);
        g_want = std::make_unique<Target>(Target{owner, folder, names, extended});
        post = !g_wantPosted;
        g_wantPosted = true;
    }
    if (post && !PostMessageW(mt, WM_MT_PREPARE, 0, 0)) {
        std::lock_guard<std::mutex> l(g_wantLock);
        g_wantPosted = false;
        return false;
    }
    return true;
}

void ShellMenu_Discard(bool wait) {
    HWND mt = g_mtWnd;
    if (!mt) return;  // thread jamais créé : rien à libérer
    {
        std::lock_guard<std::mutex> l(g_wantLock);
        g_want.reset();
    }
    if (wait) SendMessageW(mt, WM_MT_DISCARD, 0, 0);
    else PostMessageW(mt, WM_MT_DISCARD, 0, 0);
}
void ShellMenu_WarmUp(const std::wstring& folder, const std::vector<std::wstring>& names) {
    static bool done = false;
    if (done) return;
    done = true;
    struct Args {
        std::wstring folder;
        std::vector<std::wstring> names;
    };
    auto* a = new Args{folder, names};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            std::unique_ptr<Args> args((Args*)p);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            if (FAILED(OleInitialize(nullptr))) return 0;
            HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            std::vector<std::vector<std::wstring>> targets{{}};  // fond du dossier d'abord (Nouveau ▸)
            for (const auto& n : args->names) targets.push_back({n});
            for (const auto& t : targets) {
                Built b = Build(w, args->folder, t, false);
                if (!b.menu) continue;
                // Sous-menus remplis à l'ouverture (Nouveau ▸, Ouvrir avec ▸) : les plus lents à froid.
                ComPtr<IContextMenu3> cm3;
                b.cm.As(&cm3);
                for (int i = 0; cm3 && i < GetMenuItemCount(b.menu); ++i) {
                    if (HMENU s = GetSubMenu(b.menu, i)) {
                        LRESULT lr = 0;
                        cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)s, (LPARAM)i, &lr);
                    }
                }
                cm3.Reset();
                DestroyMenu(b.menu);
            }
            if (w) DestroyWindow(w);
            // Les DLL chargées restent en mémoire : le thread de l'interface en profite.
            OleUninitialize();
            return 0;
        },
        a, 0, nullptr);
    if (th) CloseHandle(th);
    else delete a;
}

namespace {
constexpr UINT kExtraFirst = kLast + 1;  // identifiants des MenuExtra, après ceux du Shell

// Ajoute les commandes de l'application en tête de `menu` (suivies d'un séparateur).
void AddExtras(HMENU menu, const std::vector<MenuExtra>& extras, bool shell) {
    UINT pos = 0;
    for (size_t i = 0; i < extras.size(); ++i) {
        if (shell && extras[i].onlyWithoutShell) continue;
        InsertMenuW(menu, pos++, MF_BYPOSITION | MF_STRING | (extras[i].checked ? MF_CHECKED : 0), kExtraFirst + (UINT)i,
                    extras[i].text.c_str());
    }
    if (pos && shell && GetMenuItemCount(menu) > (int)pos) InsertMenuW(menu, pos, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
}

bool RunExtra(UINT cmd, const std::vector<MenuExtra>& extras) {
    if (cmd < kExtraFirst || cmd - kExtraFirst >= extras.size()) return false;
    if (extras[cmd - kExtraFirst].run) extras[cmd - kExtraFirst].run();
    return true;
}
}  // namespace

bool ShellMenu_Show(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, POINT pt,
                    bool extended, const VerbFilter& filter, const std::vector<MenuExtra>& extras) {
    HWND mt = MenuThread();
    // Menu préparé au survol ou au bouton enfoncé : remis aussitôt (sinon construit maintenant).
    Take tk{Target{owner, folder, names, extended}};
    if (!mt || !SendMessageW(mt, WM_MT_TAKE, 0, (LPARAM)&tk) || !tk.shown) {
        // Pas de menu du Shell (emplacement inconnu de lui, Shell indisponible) : les nôtres seuls.
        HMENU own = extras.empty() ? nullptr : CreatePopupMenu();
        if (!own) return false;
        AddExtras(own, extras, false);
        const UINT cmd = (UINT)TrackPopupMenuEx(own, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
        DestroyMenu(own);
        RunExtra(cmd, extras);
        return true;
    }
    Shown* s = tk.shown;
    AddExtras(s->b.menu, extras, true);  // le menu est détruit après usage : on peut le modifier
    g_showing = s;
    const UINT cmd = (UINT)TrackPopupMenuEx(s->b.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    g_showing = nullptr;
    if (!RunExtra(cmd, extras) && cmd >= kFirst) {
        VerbReq v{s, cmd - kFirst};
        SendMessageW(mt, WM_MT_VERB, 0, (LPARAM)&v);
        if (!(filter && filter(v.verb))) {
            // Exécutée sur le thread des menus : l'interface ne gèle pas pendant le lancement.
            auto* r = new InvokeReq{s, owner, cmd - kFirst, folder, pt, GetKeyState(VK_CONTROL) < 0,
                                    GetKeyState(VK_SHIFT) < 0};
            if (!PostMessageW(mt, WM_MT_INVOKE, 0, (LPARAM)r)) delete r;
        }
    }
    PostMessageW(mt, WM_MT_RELEASE, 0, (LPARAM)s);
    return true;
}
bool ShellMenu_Invoke(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names,
                      const char* verb) {
    ComPtr<IContextMenu> cm = BuildMenu(owner, folder, names);
    if (!cm) return false;
    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    // Certains gestionnaires exigent QueryContextMenu avant un verbe nommé.
    cm->QueryContextMenu(menu, 0, kFirst, kLast, CMF_NORMAL);
    const bool ok = Invoke(cm.Get(), owner, 0, verb, folder, POINT{}, false, GetKeyState(VK_CONTROL) < 0,
                           GetKeyState(VK_SHIFT) < 0);
    DestroyMenu(menu);
    return ok;
}

bool ShellMenu_HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!g_showing || (!g_showing->cm3 && !g_showing->cm2)) return false;
    switch (msg) {
        case WM_INITMENUPOPUP:
        case WM_MENUCHAR:
            break;
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
            if (wp != 0) return false;  // message d'un contrôle, pas du menu
            break;
        default: return false;
    }
    MenuMsg m{g_showing, msg, wp, lp};
    SendMessageW(MenuThread(), WM_MT_MENUMSG, 0, (LPARAM)&m);
    if (!m.handled) return false;
    *result = m.result;
    return true;
}
bool ResolveShortcut(const std::wstring& lnk, std::wstring& target) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t buf[2048] = {};
    // GetPath lit la cible enregistrée ; contrairement à Resolve, il ne
    // cherche pas un fichier déplacé et n'interroge pas le réseau.
    if (sl->GetPath(buf, _countof(buf), nullptr, 0) != S_OK || !buf[0]) return false;
    target = buf;
    return true;
}
