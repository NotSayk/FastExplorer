#include "navpane.h"

#include "listing.h"
#include "paths.h"

#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "propsys")
#pragma comment(lib, "uxtheme")

using Microsoft::WRL::ComPtr;

namespace {

bool IsProtected(DWORD attr) {
    return (attr & FILE_ATTRIBUTE_HIDDEN) && (attr & FILE_ATTRIBUTE_SYSTEM);
}

// Un sous-dossier visible existe-t-il ? (lecteurs locaux seulement : ailleurs
// la flèche reste affichée et disparaît à l'ouverture si le dossier est vide)
bool HasSubfolder(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':') return true;
    const std::wstring root = path.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(ToNtPath(Join(path, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    int n = 0;
    do {
        const wchar_t* c = fd.cFileName;
        const bool dots = c[0] == L'.' && (!c[1] || (c[1] == L'.' && !c[2]));
        if (!dots && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !IsProtected(fd.dwFileAttributes)) {
            any = true;
            break;
        }
        if (++n > 4096) {  // dossier de fichiers énorme : inutile d'aller au bout
            any = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

// Sous-dossiers d'un contenu lu, dans l'ordre du nom (clés de tri de la liste).
std::vector<uint32_t> SortedDirs(const DirData& d) {
    std::vector<uint32_t> dirs;
    for (uint32_t i = 0; i < d.entries.size(); ++i)
        if (d.entries[i].isDir() && (d.isRoot() || !IsProtected(d.entries[i].attr))) dirs.push_back(i);
    std::sort(dirs.begin(), dirs.end(), [&](uint32_t a, uint32_t b) {
        const Entry &x = d.entries[a], &y = d.entries[b];
        const int c = memcmp(d.keys.data() + x.keyOff, d.keys.data() + y.keyOff, std::min(x.keyLen, y.keyLen));
        return c ? c < 0 : x.keyLen < y.keyLen;
    });
    return dirs;
}

std::vector<NavFolder> FoldersOf(const DirData& d, bool probeChildren) {
    std::vector<NavFolder> out;
    for (uint32_t i : SortedDirs(d)) {
        const Entry& e = d.entries[i];
        NavFolder f;
        if (d.isRoot()) {
            f.path = d.name(e);
            f.text = DriveDisplayName(f.path, e.extra,
                                      i < d.drives.size() ? d.drives[i] : DriveInfo{});
            f.icon = e.icon;
        } else {
            f.text = d.name(e);
            f.path = Join(d.path, d.name(e));
            if (probeChildren) f.hasChildren = HasSubfolder(f.path);
        }
        out.push_back(std::move(f));
    }
    return out;
}

// ── État du volet (thread de l'interface) ──

enum class Kind { Quick, Pinned, Pc, Drive, Folder };

struct Node {
    Kind kind;
    std::wstring path;
    HTREEITEM h = nullptr;
    bool loaded = false, loading = false, expandWhenLoaded = false;
};

HWND g_tree = nullptr, g_parent = nullptr;
UINT g_msg = 0;
NavHost g_host;
std::unordered_map<uintptr_t, Node> g_nodes;
uintptr_t g_nextId = 1;
HTREEITEM g_quick = nullptr, g_pc = nullptr;
int g_folderIcon = 0;
bool g_syncing = false;  // sélection faite par le programme : pas de navigation
ULONGLONG g_pinnedAt = 0;
bool g_pinnedLoading = false;

struct Result {
    enum Type { Children, Pinned } type;
    uintptr_t id = 0;  // Children : nœud visé
    std::vector<NavFolder> folders;
    int rootIcon = -1;  // icône du nœud racine (Accès rapide, Ce PC)
};

Node* NodeOf(HTREEITEM h) {
    if (!h) return nullptr;
    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = h;
    if (!TreeView_GetItem(g_tree, &it)) return nullptr;
    auto f = g_nodes.find((uintptr_t)it.lParam);
    return f == g_nodes.end() ? nullptr : &f->second;
}

uintptr_t IdOf(HTREEITEM h) {
    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = h;
    return TreeView_GetItem(g_tree, &it) ? (uintptr_t)it.lParam : 0;
}

HTREEITEM Insert(HTREEITEM parent, HTREEITEM after, const NavFolder& f, Kind kind) {
    const uintptr_t id = g_nextId++;
    g_nodes[id] = Node{kind, f.path};
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = after;
    is.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
    is.item.pszText = (LPWSTR)f.text.c_str();
    is.item.iImage = is.item.iSelectedImage = f.icon >= 0 ? f.icon : g_folderIcon;
    is.item.lParam = (LPARAM)id;
    is.item.cChildren = (kind == Kind::Pinned) ? 0 : (f.hasChildren ? 1 : 0);
    HTREEITEM h = TreeView_InsertItem(g_tree, &is);
    if (h) g_nodes[id].h = h;
    else g_nodes.erase(id);
    return h;
}

void SetChildrenFlag(HTREEITEM h, bool has) {
    TVITEMW it{};
    it.mask = TVIF_CHILDREN;
    it.hItem = h;
    it.cChildren = has ? 1 : 0;
    TreeView_SetItem(g_tree, &it);
}

// Aligne les enfants de `parent` sur `want` (même ordre) sans toucher à ceux
// qui restent : leurs sous-arbres déjà ouverts sont conservés.
void SyncChildren(HTREEITEM parent, const std::vector<NavFolder>& want, Kind kind) {
    std::unordered_map<std::wstring, HTREEITEM> have;
    auto key = [](std::wstring p) {
        CharLowerBuffW(p.data(), (DWORD)p.size());
        return p;
    };
    std::unordered_map<std::wstring, const NavFolder*> wanted;
    for (const auto& f : want) wanted[key(f.path)] = &f;
    for (HTREEITEM c = TreeView_GetChild(g_tree, parent); c;) {
        HTREEITEM next = TreeView_GetNextSibling(g_tree, c);
        Node* n = NodeOf(c);
        if (!n || !wanted.count(key(n->path))) TreeView_DeleteItem(g_tree, c);
        else have[key(n->path)] = c;
        c = next;
    }
    HTREEITEM prev = TVI_FIRST;
    for (const auto& f : want) {
        auto it = have.find(key(f.path));
        if (it == have.end()) {
            if (HTREEITEM h = Insert(parent, prev, f, kind)) prev = h;
            continue;
        }
        // Libellé ou icône changés (lecteur renommé, média inséré…).
        wchar_t text[512] = {};
        TVITEMW cur{};
        cur.mask = TVIF_TEXT | TVIF_IMAGE;
        cur.hItem = it->second;
        cur.pszText = text;
        cur.cchTextMax = _countof(text);
        TreeView_GetItem(g_tree, &cur);
        const int icon = f.icon >= 0 ? f.icon : g_folderIcon;
        if (f.text != text || cur.iImage != icon) {
            TVITEMW up{};
            up.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            up.hItem = it->second;
            up.pszText = (LPWSTR)f.text.c_str();
            up.iImage = up.iSelectedImage = icon;
            TreeView_SetItem(g_tree, &up);
        }
        prev = it->second;
    }
}

void Post(Result* r) {
    if (!PostMessageW(g_parent, g_msg, 0, (LPARAM)r)) delete r;
}

void StartLoad(uintptr_t id) {
    Node& n = g_nodes[id];
    if (n.loading) return;
    n.loading = true;
    const bool pc = n.kind == Kind::Pc;
    std::thread([id, path = n.path, pc] {
        auto* r = new Result{Result::Children, id};
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        r->folders = NavPane_ReadChildren(path);
        if (pc) {
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p))) {
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW((LPCWSTR)p, 0, &sfi, sizeof sfi, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                    r->rootIcon = sfi.iIcon;
                CoTaskMemFree(p);
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
        Post(r);
    }).detach();
}

void SetIcon(HTREEITEM h, int icon) {
    if (!h || icon < 0) return;
    TVITEMW it{};
    it.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    it.hItem = h;
    it.iImage = it.iSelectedImage = icon;
    TreeView_SetItem(g_tree, &it);
}

HTREEITEM FindByPath(const std::wstring& path) {
    HTREEITEM pinned = nullptr;
    for (const auto& [id, n] : g_nodes) {
        if (_wcsicmp(n.path.c_str(), path.c_str()) != 0 || n.kind == Kind::Quick) continue;
        if (n.kind == Kind::Pinned) pinned = n.h;
        else return n.h;  // de préférence dans Ce PC
    }
    return pinned;
}

}  // namespace

std::vector<NavFolder> NavPane_ReadChildren(const std::wstring& path) {
    DirData d;
    d.path = path;
    Enumerate(d);
    if (d.error) return {};
    return FoldersOf(d, true);
}

std::vector<NavFolder> NavPane_ReadPinned() {
    std::vector<NavFolder> out;
    ComPtr<IShellItem> qa;
    if (FAILED(SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr,
                                           IID_PPV_ARGS(&qa))))
        return out;
    ComPtr<IEnumShellItems> en;
    if (FAILED(qa->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) return out;
    PROPERTYKEY pinnedKey{};
    const bool havePinned = SUCCEEDED(PSGetPropertyKeyFromName(L"System.Home.IsPinned", &pinnedKey));
    for (ComPtr<IShellItem> it; en->Next(1, &it, nullptr) == S_OK; it.Reset()) {
        // Seulement les épinglés (l'Accès rapide contient aussi les dossiers fréquents).
        ComPtr<IShellItem2> it2;
        BOOL pinned = !havePinned;
        if (havePinned && SUCCEEDED(it.As(&it2)) && FAILED(it2->GetBool(pinnedKey, &pinned))) pinned = FALSE;
        if (!pinned) continue;
        PWSTR p = nullptr, n = nullptr;
        if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) continue;  // emplacement virtuel : ignoré
        NavFolder f;
        f.path = p;
        CoTaskMemFree(p);
        if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) {
            f.text = n;
            CoTaskMemFree(n);
        } else {
            f.text = f.path;
        }
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(f.path.c_str(), 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) f.icon = sfi.iIcon;
        f.hasChildren = false;
        out.push_back(std::move(f));
    }
    return out;
}

HWND NavPane_Create(HWND parent, UINT id, UINT msg, NavHost host) {
    g_parent = parent;
    g_msg = msg;
    g_host = std::move(host);
    g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_SHOWSELALWAYS |
                                 TVS_FULLROWSELECT | TVS_TRACKSELECT | TVS_NOHSCROLL,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (!g_tree) return nullptr;
    SetWindowTheme(g_tree, L"Explorer", nullptr);
    const DWORD ex = TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS | TVS_EX_AUTOHSCROLL;
    TreeView_SetExtendedStyle(g_tree, ex, ex);

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof sfi,
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
        g_folderIcon = sfi.iIcon;

    NavFolder quick{L"Accès rapide", L"", -1, true};
    g_quick = Insert(TVI_ROOT, TVI_LAST, quick, Kind::Quick);
    if (Node* n = NodeOf(g_quick)) n->loaded = true;  // ses enfants viennent de NavPane_RefreshPinned
    NavFolder pc{L"Ce PC", L"", -1, true};
    g_pc = Insert(TVI_ROOT, TVI_LAST, pc, Kind::Pc);
    if (Node* n = NodeOf(g_pc)) n->expandWhenLoaded = true;  // ouvert d'emblée, comme l'Explorateur
    StartLoad(IdOf(g_pc));
    NavPane_RefreshPinned(true);
    return g_tree;
}

void NavPane_SetImageList(HIMAGELIST il) {
    if (g_tree) TreeView_SetImageList(g_tree, il, TVSIL_NORMAL);  // le volet ne la détruit pas
}

void NavPane_RefreshPinned(bool force) {
    if (!g_tree || g_pinnedLoading) return;
    if (!force && GetTickCount64() - g_pinnedAt < 2000) return;
    g_pinnedLoading = true;
    g_pinnedAt = GetTickCount64();
    std::thread([] {
        auto* r = new Result{Result::Pinned};
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        r->folders = NavPane_ReadPinned();
        PIDLIST_ABSOLUTE p = nullptr;
        if (SUCCEEDED(SHParseDisplayName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr, &p, 0, nullptr))) {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW((LPCWSTR)p, 0, &sfi, sizeof sfi, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                r->rootIcon = sfi.iIcon;
            CoTaskMemFree(p);
        }
        if (SUCCEEDED(co)) CoUninitialize();
        Post(r);
    }).detach();
}

void NavPane_RefreshDrives() {
    if (!g_pc) return;
    if (Node* n = NodeOf(g_pc)) {
        n->loading = false;
        StartLoad(IdOf(g_pc));
    }
}

void NavPane_OnResult(LPARAM lp) {
    std::unique_ptr<Result> r((Result*)lp);
    if (!g_tree) return;
    if (r->type == Result::Pinned) {
        g_pinnedLoading = false;
        const bool first = !TreeView_GetChild(g_tree, g_quick);
        SyncChildren(g_quick, r->folders, Kind::Pinned);
        SetIcon(g_quick, r->rootIcon);
        SetChildrenFlag(g_quick, !r->folders.empty());
        if (first && !r->folders.empty()) TreeView_Expand(g_tree, g_quick, TVE_EXPAND);
        return;
    }
    auto f = g_nodes.find(r->id);
    if (f == g_nodes.end()) return;  // nœud supprimé entre-temps
    Node& n = f->second;
    n.loading = false;
    n.loaded = true;
    const HTREEITEM h = n.h;
    const bool expand = n.expandWhenLoaded;
    n.expandWhenLoaded = false;
    const Kind kind = n.kind == Kind::Pc ? Kind::Drive : Kind::Folder;
    SyncChildren(h, r->folders, kind);  // (peut invalider `n` : on ne s'en sert plus)
    if (h == g_pc) SetIcon(h, r->rootIcon);
    SetChildrenFlag(h, !r->folders.empty());
    if (expand && !r->folders.empty()) TreeView_Expand(g_tree, h, TVE_EXPAND);
}

void NavPane_ShowPath(const std::wstring& path) {
    if (!g_tree) return;
    HTREEITEM h = path.empty() ? g_pc : FindByPath(path);
    if (h == TreeView_GetSelection(g_tree)) return;
    g_syncing = true;
    TreeView_SelectItem(g_tree, h);  // nullptr : plus rien de sélectionné
    g_syncing = false;
}

void NavPane_FolderListed(const DirData& d) {
    if (!g_tree) return;
    for (auto& [id, n] : g_nodes) {
        const bool match = d.isRoot() ? n.kind == Kind::Pc
                                      : (n.kind == Kind::Folder || n.kind == Kind::Drive) &&
                                            _wcsicmp(n.path.c_str(), d.path.c_str()) == 0;
        if (!match || !n.loaded || n.loading) continue;
        const HTREEITEM h = n.h;
        const Kind kind = d.isRoot() ? Kind::Drive : Kind::Folder;
        const std::vector<NavFolder> want = FoldersOf(d, false);  // pas de lecture disque ici
        SyncChildren(h, want, kind);
        SetChildrenFlag(h, !want.empty());
        return;  // g_nodes a pu changer : on s'arrête là
    }
}

bool NavPane_Notify(const NMHDR* hdr, LRESULT* result) {
    if (!g_tree || hdr->hwndFrom != g_tree) return false;
    switch (hdr->code) {
        case TVN_ITEMEXPANDINGW: {
            const auto* nm = (const NMTREEVIEWW*)hdr;
            *result = FALSE;
            if (!(nm->action & TVE_EXPAND)) return true;
            Node* n = NodeOf(nm->itemNew.hItem);
            if (!n || n->loaded) return true;
            // Pas encore lu : on ouvrira quand la lecture (sur un thread) reviendra.
            n->expandWhenLoaded = true;
            StartLoad(IdOf(nm->itemNew.hItem));
            *result = TRUE;
            return true;
        }
        case TVN_SELCHANGEDW: {
            const auto* nm = (const NMTREEVIEWW*)hdr;
            *result = 0;
            if (g_syncing || (nm->action != TVC_BYMOUSE && nm->action != TVC_BYKEYBOARD)) return true;
            Node* n = NodeOf(nm->itemNew.hItem);
            if (n && n->kind != Kind::Quick && g_host.navigate) g_host.navigate(n->path);
            return true;
        }
        case TVN_DELETEITEMW: {
            const auto* nm = (const NMTREEVIEWW*)hdr;
            g_nodes.erase((uintptr_t)nm->itemOld.lParam);
            *result = 0;
            return true;
        }
    }
    return false;
}

void NavPane_ContextMenu(LPARAM lp) {
    if (!g_tree || !g_host.contextMenu) return;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    HTREEITEM h = nullptr;
    if (lp == -1) {  // clavier (Maj+F10, touche Menu) : l'élément sélectionné
        h = TreeView_GetSelection(g_tree);
        RECT r{};
        if (!h || !TreeView_GetItemRect(g_tree, h, &r, TRUE)) return;
        pt = {r.left, r.bottom};
        ClientToScreen(g_tree, &pt);
    } else {
        TVHITTESTINFO hit{};
        hit.pt = pt;
        ScreenToClient(g_tree, &hit.pt);
        h = TreeView_HitTest(g_tree, &hit);
        if (!(hit.flags & TVHT_ONITEM)) h = nullptr;
    }
    Node* n = NodeOf(h);
    if (!n || n->kind == Kind::Quick) return;
    const std::wstring path = n->path;
    TreeView_SelectDropTarget(g_tree, h);  // repère l'élément visé, comme l'Explorateur
    g_host.contextMenu(path, pt);
    if (g_tree) TreeView_SelectDropTarget(g_tree, nullptr);
}
