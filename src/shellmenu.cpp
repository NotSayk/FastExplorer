#include "shellmenu.h"

#include "paths.h"

#include <shellapi.h>  // SEE_MASK_* (utilisés par CMIC_MASK_*)
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <memory>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFirst = 1, kLast = 0x7FFF;

// Interfaces du menu affiché, le temps de TrackPopupMenuEx (sous-menus dynamiques).
ComPtr<IContextMenu2> g_cm2;
ComPtr<IContextMenu3> g_cm3;

struct PidlFree {
    void operator()(ITEMIDLIST* p) const { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlFree>;

Pidl ParsePath(const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    const HRESULT hr = path.empty() ? SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p)
                                    : SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr);
    return Pidl(SUCCEEDED(hr) ? (ITEMIDLIST*)p : nullptr);  // retire __unaligned (sans effet sur x64)
}

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
    // Les éléments partagent le même parent : on demande au dossier parent
    // un seul menu pour toute la sélection.
    std::vector<Pidl> abs;
    abs.reserve(names.size());
    for (const auto& n : names) {
        Pidl p = ParsePath(Join(folder, n.c_str()));
        if (!p) return cm;
        abs.push_back(std::move(p));
    }
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD first = nullptr;
    if (FAILED(SHBindToParent(abs[0].get(), IID_PPV_ARGS(&parent), &first))) return cm;
    std::vector<PCUITEMID_CHILD> kids;
    kids.reserve(abs.size());
    for (const auto& p : abs) kids.push_back((PCUITEMID_CHILD)ILFindLastID(p.get()));
    parent->GetUIObjectOf(owner, (UINT)kids.size(), kids.data(), IID_IContextMenu, nullptr, (void**)cm.GetAddressOf());
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
            bool hasPt) {
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK | (hasPt ? CMIC_MASK_PTINVOKE : 0);
    if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
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

}  // namespace

bool ShellMenu_Show(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, POINT pt,
                    bool extended, const VerbFilter& filter) {
    ComPtr<IContextMenu> cm = BuildMenu(owner, folder, names);
    if (!cm) return false;
    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    const UINT flags = CMF_NORMAL | (extended ? CMF_EXTENDEDVERBS : 0);
    if (FAILED(cm->QueryContextMenu(menu, 0, kFirst, kLast, flags))) {
        DestroyMenu(menu);
        return false;
    }
    RemoveDuplicates(menu);
    cm.As(&g_cm3);
    if (!g_cm3) cm.As(&g_cm2);
    const UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    g_cm2.Reset();
    g_cm3.Reset();

    bool ok = true;
    if (cmd >= kFirst) {
        const UINT offset = cmd - kFirst;
        wchar_t verb[80] = {};
        if (FAILED(cm->GetCommandString(offset, GCS_VERBW, nullptr, (LPSTR)verb, _countof(verb)))) verb[0] = 0;
        verb[_countof(verb) - 1] = 0;
        if (!(filter && filter(verb))) ok = Invoke(cm.Get(), owner, offset, nullptr, folder, pt, true);
    }
    DestroyMenu(menu);
    return ok;
}

bool ShellMenu_Invoke(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names,
                      const char* verb) {
    ComPtr<IContextMenu> cm = BuildMenu(owner, folder, names);
    if (!cm) return false;
    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    // Certains gestionnaires exigent QueryContextMenu avant un verbe nommé.
    cm->QueryContextMenu(menu, 0, kFirst, kLast, CMF_NORMAL);
    const bool ok = Invoke(cm.Get(), owner, 0, verb, folder, POINT{}, false);
    DestroyMenu(menu);
    return ok;
}

bool ShellMenu_HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!g_cm2 && !g_cm3) return false;
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
    if (g_cm3) {
        LRESULT lr = 0;
        if (FAILED(g_cm3->HandleMenuMsg2(msg, wp, lp, &lr))) return false;
        *result = lr;
        return true;
    }
    if (msg == WM_MENUCHAR) return false;
    if (FAILED(g_cm2->HandleMenuMsg(msg, wp, lp))) return false;
    *result = (msg == WM_INITMENUPOPUP) ? 0 : TRUE;
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
