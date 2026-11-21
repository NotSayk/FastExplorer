// Fenêtre principale : barre d'adresse + ListView virtuelle + barre d'état.
//
// Règle d'or : le thread de l'interface ne fait jamais d'E/S disque/réseau
// ni de travail proportionnel à la taille du dossier au-delà d'un seuil.
// Lecture, clés de tri, icônes de lecteurs et tris lourds passent par jobs.h.

#include "ui.h"

#include "dragdrop.h"
#include "fileops.h"
#include "icons.h"
#include "jobs.h"
#include "listing.h"
#include "paths.h"
#include "shellmenu.h"
#include "util.h"
#include "watch.h"

#include <commctrl.h>
#include <dbt.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_RESULT = WM_APP + 1;
constexpr UINT WM_APP_ICON = WM_APP + 2;        // lParam = IconResult*
constexpr UINT WM_APP_DIRCHANGED = WM_APP + 3;  // wParam = id de surveillance
constexpr UINT WM_APP_FILEOP = WM_APP + 4;      // lParam = FileOpResult*
constexpr UINT WM_APP_EDITSEL = WM_APP + 5;     // renommage : sélection du nom sans l'extension
constexpr UINT WM_APP_REEDIT = WM_APP + 6;      // renommage refusé : on rouvre la saisie
constexpr UINT WM_APP_RECLAIM = WM_APP + 7;     // reprendre le premier plan au moteur de copie
constexpr UINT WM_APP_SHELLNOTIFY = WM_APP + 8; // lecteur ou média ajouté / retiré (SHChangeNotifyRegister)
constexpr UINT_PTR TIMER_LOADING = 1;
constexpr UINT_PTR TIMER_WATCH = 2;
constexpr UINT_PTR TIMER_AFTEROP = 3;
constexpr UINT_PTR TIMER_DRIVES = 4;  // Ce PC : regroupe les annonces d'arrivée / retrait de lecteurs
constexpr UINT_PTR TIMER_EJECT = 5;   // éjection demandée : si elle échoue, on reprend la surveillance  // resélection après une opération, différée pendant un renommage
constexpr UINT kLoadingDelayMs = 150;   // en dessous, aucun « Chargement… » (évite le clignotement)
constexpr size_t kSyncSortMax = 20000;  // au-delà, le re-tri part en arrière-plan
// Rafraîchissement après un changement sur disque : regroupe les rafales
// d'événements (copie de milliers de fichiers) et s'espace sur les gros dossiers.
constexpr double kWatchMinDelayMs = 150, kWatchMaxDelayMs = 2000;

enum { ID_BACK = 100, ID_FWD, ID_UP, ID_ADDR, ID_LIST, ID_STATUS };
enum class Nav { Normal, Back, Forward, Refresh };
enum class JobKind { Navigate, Sort };

struct Result {
    uint64_t gen = 0;
    JobKind kind = JobKind::Navigate;
    std::shared_ptr<const DirData> data;
    std::vector<uint32_t> order;
    double sortMs = 0;
};

// Navigation demandée mais pas encore affichée.
struct Pending {
    bool active = false;
    std::wstring path;
    Nav nav = Nav::Normal;
    std::wstring focus;                  // nom à cibler
    std::vector<std::wstring> selected;  // noms à resélectionner (F5)
    // Si le chemin n'est pas un dossier, ouvrir ceci à la place (raccourci .lnk
    // vers un fichier, chemin de fichier tapé dans la barre d'adresse).
    std::wstring fallbackOpen;
    bool fallbackAnyError = false;  // raccourci : aussi si la cible a disparu (le Shell s'en occupe)
};

enum IconState : uint8_t { ICON_NONE, ICON_GENERIC, ICON_PENDING, ICON_OWN };

// Ce qui est affiché. `order` : ligne → index d'entrée ; `rowOf` : l'inverse.
struct View {
    std::shared_ptr<const DirData> data;
    std::vector<uint32_t> order;
    std::vector<int> rowOf;
    std::vector<int32_t> icons;     // par index d'entrée
    std::vector<uint8_t> overlays;  // surimpression par index d'entrée
    std::vector<uint8_t> iconState;
    std::vector<uint8_t> cut;       // 1 = coupé (affiché estompé)
    uint64_t serial = 0;            // identifie la vue auprès du thread d'icônes
    double sortMs = 0;
};

HINSTANCE g_inst;
HWND g_hwnd, g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp;
HFONT g_font;
int g_fontH = 16;
UINT g_dpi = 96;
HIMAGELIST g_imgSmall, g_imgLarge;

View g_view;
std::wstring g_path;
Pending g_pending;
std::vector<std::wstring> g_back, g_fwd;
int g_sortCol = COL_NAME;
bool g_sortAsc = true;
std::unordered_map<std::wstring, int> g_iconCache;
double g_startupMs = -1;  // -1 = pas encore mesuré, 0 = déjà affiché
uint64_t g_viewSerial = 0;
uintptr_t g_watchId = 0;
std::wstring g_watchPath;
std::wstring g_goneNote;  // « X n'est plus accessible » : reste affiché tant qu'on ne navigue pas ailleurs
std::wstring g_watchFailed;  // surveillance impossible ici (droits, réseau) : pas de boucle de relances
bool g_watchPending = false;  // un rafraîchissement est programmé
ULONG g_shellNotify = 0;      // inscription aux annonces de lecteurs du Shell

void RegisterShellNotify();

// ── Opérations sur les fichiers ──
bool g_editing = false;      // saisie d'un nom en cours : aucun rafraîchissement ne doit l'interrompre
std::wstring g_editName;     // élément en cours de renommage
std::wstring g_editAfter;    // après le prochain rafraîchissement : renommer cet élément (nouveau dossier)
bool g_quitWhenIdle = false; // fermeture demandée pendant une copie : on attend sa fin
struct Reedit {
    std::wstring name, text;
    const wchar_t* error = nullptr;
} g_reedit;
// Éléments coupés (estompés tant que le presse-papiers les contient).
struct {
    std::wstring folder;
    std::unordered_set<std::wstring> names;
} g_cut;
// Resélection demandée par une opération terminée.
struct AfterOp {
    bool active = false;
    std::wstring folder, focus, edit;
    std::vector<std::wstring> names;
} g_afterOp;
// Commande du fond du dossier (Nouveau ▸ Document texte…) : le nouvel élément,
// vu au rafraîchissement suivant, passe directement en renommage. Après un
// dépôt (selectAll), les éléments arrivés sont sélectionnés.
struct {
    std::shared_ptr<const DirData> before;
    ULONGLONG deadline = 0;
    bool selectAll = false;
} g_expectNew;
// Glisser-déposer
int g_dropRow = -1;                            // ligne cible en surbrillance
std::unordered_set<std::wstring> g_dragNames;  // éléments que l'on fait glisser

int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
bool HasView() { return g_view.data != nullptr; }
int RowCount() { return HasView() ? (int)g_view.order.size() : 0; }
int EntryAt(int row) { return (row >= 0 && row < RowCount()) ? (int)g_view.order[row] : -1; }
const Entry& E(int idx) { return g_view.data->entries[idx]; }
const wchar_t* NameOf(int idx) { return g_view.data->name(E(idx)); }

void SetStatus(const std::wstring& s) { SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str()); }

// Ouvre un fichier avec son application associée (comme un double-clic dans l'Explorateur).
void OpenWithShell(const std::wstring& full) {
    std::wstring dir, child;
    if (!ParentOf(full, dir, child)) dir.clear();
    const HINSTANCE r = ShellExecuteW(g_hwnd, nullptr, full.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(),
                                      SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) SetStatus(L"Impossible d'ouvrir « " + full + L" » : " + ErrorText(GetLastError()));
}

void ShowSummary() {
    if (!HasView()) return;
    const DirData& d = *g_view.data;
    wchar_t s[512];
    int k = swprintf_s(s, L"%zu éléments  (%zu dossiers, %zu fichiers)   •   lecture %.2f ms, clés %.2f ms, tri %.2f ms",
                       d.entries.size(), d.nDirs, d.nFiles, d.enumMs, d.keyMs, g_view.sortMs);
    if (g_startupMs > 0 && k > 0) {
        swprintf_s(s + k, _countof(s) - k, L"   •   démarrage %.0f ms", g_startupMs);
        g_startupMs = 0;  // affiché une seule fois
    }
    SetStatus(g_goneNote.empty() ? std::wstring(s) : g_goneNote + L"   •   " + s);
}

void UpdateButtons() {
    EnableWindow(g_btnBack, !g_back.empty());
    EnableWindow(g_btnFwd, !g_fwd.empty());
    EnableWindow(g_btnUp, HasView() && !g_path.empty());
}

void UpdateSortArrow() {
    HWND hdr = ListView_GetHeader(g_list);
    for (int i = 0; i < COL_COUNT; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sortCol) hi.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hi);
    }
}

// ───────────────────────────── Sélection ─────────────────────────────

int FocusedEntry() { return EntryAt(ListView_GetNextItem(g_list, -1, LVNI_FOCUSED)); }

std::vector<int> SelectedEntries() {
    std::vector<int> v;
    for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0;)
        if (int e = EntryAt(r); e >= 0) v.push_back(e);
    return v;
}

int FindEntryByName(const std::wstring& name) {
    if (!HasView() || name.empty()) return -1;
    const auto& es = g_view.data->entries;
    for (size_t i = 0; i < es.size(); ++i)
        if (_wcsicmp(NameOf((int)i), name.c_str()) == 0) return (int)i;
    return -1;
}

std::vector<int> FindEntriesByNames(const std::vector<std::wstring>& names) {
    std::vector<int> v;
    if (!HasView() || names.empty()) return v;
    std::unordered_set<std::wstring_view> want(names.begin(), names.end());
    const auto& es = g_view.data->entries;
    for (size_t i = 0; i < es.size(); ++i)
        if (want.count(std::wstring_view(NameOf((int)i), es[i].nameLen))) v.push_back((int)i);
    return v;
}

void RebuildRowOf() {
    const int n = RowCount();
    g_view.rowOf.assign(n, -1);
    for (int r = 0; r < n; ++r) g_view.rowOf[g_view.order[r]] = r;
}

// Applique une sélection exprimée en index d'entrées (indépendante du tri).
void ApplySelection(const std::vector<int>& sel, int focus, bool scroll) {
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    const int n = RowCount();
    if (!n) return;
    const std::vector<int>& rowOf = g_view.rowOf;
    for (int e : sel)
        if (e >= 0 && e < n) ListView_SetItemState(g_list, rowOf[e], LVIS_SELECTED, LVIS_SELECTED);
    if (focus >= 0 && focus < n) {
        const int fr = rowOf[focus];
        ListView_SetItemState(g_list, fr, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_SetSelectionMark(g_list, fr);
        if (scroll) ListView_EnsureVisible(g_list, fr, FALSE);
    } else if (scroll) {
        ListView_EnsureVisible(g_list, 0, FALSE);
    }
}

// ───────────────────────────── Affichage des lignes ─────────────────────────────

int IconFor(int idx) {
    const Entry& e = E(idx);
    std::wstring key, probe;
    DWORD attr;
    if (e.isDir()) {
        key = L"\x01dir";
        probe = L"folder";
        attr = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        key.assign(NameOf(idx) + e.extOff, e.nameLen - e.extOff);
        CharLowerBuffW(key.data(), (DWORD)key.size());
        probe = L"f" + key;
        if (key.empty()) key = L"\x01file";
        attr = FILE_ATTRIBUTE_NORMAL;
    }
    auto it = g_iconCache.find(key);
    if (it != g_iconCache.end()) return it->second;
    SHFILEINFOW sfi{};  // SHGFI_USEFILEATTRIBUTES : aucun accès disque
    const int icon = SHGetFileInfoW(probe.c_str(), attr, &sfi, sizeof sfi,
                                    SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)
                         ? sfi.iIcon
                         : 0;
    g_iconCache.emplace(std::move(key), icon);
    return icon;
}

const wchar_t* DriveTypeText(UINT t) {
    switch (t) {
        case DRIVE_FIXED: return L"Disque local";
        case DRIVE_REMOVABLE: return L"Disque amovible";
        case DRIVE_REMOTE: return L"Lecteur réseau";
        case DRIVE_CDROM: return L"Lecteur CD/DVD";
        case DRIVE_RAMDISK: return L"Disque RAM";
        default: return L"Lecteur";
    }
}

void FormatDate(int64_t t, wchar_t* out, int cch) {
    out[0] = 0;
    if (t <= 0) return;
    FILETIME ft{(DWORD)t, (DWORD)((uint64_t)t >> 32)};
    SYSTEMTIME su, sl;
    if (!FileTimeToSystemTime(&ft, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl)) return;
    const int n = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &sl, nullptr, out, cch, nullptr);
    if (n > 0 && n < cch) {
        out[n - 1] = L' ';
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &sl, nullptr, out + n, cch - n);
    }
}

void FillDispInfo(LVITEMW& it) {
    const int idx = EntryAt(it.iItem);
    if (idx < 0) return;
    const Entry& e = E(idx);
    if (it.mask & LVIF_IMAGE) {
        uint8_t& st = g_view.iconState[idx];
        if (st == ICON_NONE) {
            // Icône du type tout de suite ; l'icône propre (exe, lnk…) suit en tâche de fond.
            g_view.icons[idx] = IconFor(idx);
            st = ICON_GENERIC;
            if (!g_view.data->isRoot() && NeedsOwnIcon(e.attr, NameOf(idx) + e.extOff, e.nameLen - e.extOff)) {
                st = ICON_PENDING;
                Icons_Request(g_view.serial, (uint32_t)idx, Join(g_path, NameOf(idx)));
            }
        }
        it.iImage = g_view.icons[idx];
    }
    if (it.mask & LVIF_STATE) {  // surimpression (flèche de raccourci, état cloud), élément coupé
        it.state = INDEXTOOVERLAYMASK(g_view.overlays[idx]) | (g_view.cut[idx] ? LVIS_CUT : 0) |
                   (it.iItem == g_dropRow ? LVIS_DROPHILITED : 0);
        it.stateMask = LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED;
    }
    if (!(it.mask & LVIF_TEXT) || !it.pszText || it.cchTextMax <= 0) return;
    wchar_t* out = it.pszText;
    const int cch = it.cchTextMax;
    out[0] = 0;
    switch (it.iSubItem) {
        case COL_NAME: wcsncpy_s(out, cch, NameOf(idx), _TRUNCATE); break;
        case COL_SIZE:
            if (!e.isDir()) StrFormatKBSizeW((LONGLONG)e.size, out, (UINT)cch);
            break;
        case COL_TYPE:
            if (g_view.data->isRoot()) wcsncpy_s(out, cch, DriveTypeText(e.extra), _TRUNCATE);
            else if (e.isDir()) wcsncpy_s(out, cch, L"Dossier de fichiers", _TRUNCATE);
            else if (e.extOff + 1 < e.nameLen) {
                swprintf_s(out, cch, L"Fichier %s", NameOf(idx) + e.extOff + 1);
                CharUpperBuffW(out + 8, (DWORD)wcslen(out + 8));
            } else wcsncpy_s(out, cch, L"Fichier", _TRUNCATE);
            break;
        case COL_DATE: FormatDate(e.mtime, out, cch); break;
    }
}

// Marque les éléments coupés de la vue courante.
void ApplyCutMarks() {
    const size_t n = HasView() ? g_view.data->entries.size() : 0;
    g_view.cut.assign(n, 0);
    if (g_cut.names.empty() || _wcsicmp(g_cut.folder.c_str(), g_path.c_str()) != 0) return;
    for (size_t i = 0; i < n; ++i)
        if (g_cut.names.count(NameOf((int)i))) g_view.cut[i] = 1;
}

// Recherche au clavier (taper « rap » saute à « rapport.docx »).
int FindItem(const NMLVFINDITEMW* f) {
    if (!(f->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !f->lvfi.psz) return -1;
    const int n = RowCount();
    if (!n) return -1;
    const size_t len = wcslen(f->lvfi.psz);
    const int start = (f->iStart >= 0 && f->iStart < n) ? f->iStart : 0;
    for (int k = 0; k < n; ++k) {
        const int r = (start + k) % n;
        if (_wcsnicmp(NameOf(EntryAt(r)), f->lvfi.psz, len) == 0) return r;
    }
    return -1;
}

// ───────────────────────────── Navigation ─────────────────────────────

// Au démarrage, la première lecture est lancée AVANT la création de la fenêtre
// (les deux se font en parallèle). Un résultat qui arrive alors que la fenêtre
// n'existe pas encore est déposé dans g_early ; WM_CREATE le récupère.
// Les deux côtés font un exchange : exactement un seul des deux le poste.
std::atomic<HWND> g_target{nullptr};
std::atomic<Result*> g_early{nullptr};

void PostTo(HWND h, Result* r) {
    if (!PostMessageW(h, WM_APP_RESULT, 0, (LPARAM)r)) delete r;
}

void Post(Result* r) {
    if (HWND h = g_target.load()) { PostTo(h, r); return; }
    delete g_early.exchange(r);  // (jamais deux résultats en attente : le précédent est périmé)
    if (HWND h = g_target.load())
        if (Result* e = g_early.exchange(nullptr)) PostTo(h, e);
}

void AttachTarget(HWND hwnd) {
    g_target.store(hwnd);
    if (Result* e = g_early.exchange(nullptr)) PostTo(hwnd, e);
}

void Navigate(const std::wstring& path, Nav nav, std::wstring focus = {}, std::vector<std::wstring> selected = {},
              std::wstring fallbackOpen = {}, bool fallbackAnyError = false) {
    const CancelToken ct = Jobs_NewGeneration();
    g_pending = Pending{true,           path, nav, std::move(focus), std::move(selected), std::move(fallbackOpen),
                        fallbackAnyError};
    if (g_hwnd) SetTimer(g_hwnd, TIMER_LOADING, kLoadingDelayMs, nullptr);
    const int col = g_sortCol;
    const bool asc = g_sortAsc;
    const bool climb = nav == Nav::Refresh;  // dossier affiché disparu : on remonte, comme l'Explorateur
    Jobs_Spawn([ct, path, col, asc, climb] {
        auto d = std::make_shared<DirData>();
        d->path = path;
        if (climb) EnumerateNearest(*d, ct);
        else Enumerate(*d, ct);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Navigate};
        if (!d->error) {
            const double t = NowMs();
            r->order = SortOrder(*d, col, asc);
            r->sortMs = NowMs() - t;
        }
        r->data = std::move(d);
        Post(r);
    });
}

void ApplyOrder(std::vector<uint32_t> order, double ms) {
    const auto sel = SelectedEntries();
    const int focus = FocusedEntry();
    g_view.order = std::move(order);
    g_view.sortMs = ms;
    RebuildRowOf();
    ApplySelection(sel, focus, true);
    InvalidateRect(g_list, nullptr, FALSE);
    ShowSummary();
}

void Resort(int col) {
    if (col == g_sortCol) g_sortAsc = !g_sortAsc;
    else { g_sortCol = col; g_sortAsc = (col == COL_NAME || col == COL_TYPE); }
    UpdateSortArrow();

    if (g_pending.active) {  // une navigation est en vol : on la relance avec le nouveau tri
        Pending p = g_pending;
        Navigate(p.path, p.nav, p.focus, p.selected, p.fallbackOpen, p.fallbackAnyError);
        return;
    }
    if (!HasView()) return;
    auto data = g_view.data;
    if (data->entries.size() <= kSyncSortMax) {
        const double t = NowMs();
        auto order = SortOrder(*data, g_sortCol, g_sortAsc);
        ApplyOrder(std::move(order), NowMs() - t);
        return;
    }
    const CancelToken ct = Jobs_NewGeneration();
    const int c = g_sortCol;
    const bool asc = g_sortAsc;
    SetStatus(L"Tri…");
    Jobs_Spawn([ct, data, c, asc] {
        const double t = NowMs();
        auto order = SortOrder(*data, c, asc);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Sort, data, std::move(order), NowMs() - t};
        Post(r);
    });
}

// Premier vrai dossier affiché : préchauffe les menus contextuels (une fois)
// avec le fond du dossier, un sous-dossier et un fichier présents sur le disque.
void WarmUpMenus(const DirData& d) {
    constexpr DWORD kNotLocal =
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    std::vector<std::wstring> names;
    bool dir = false, file = false;
    for (const Entry& e : d.entries) {
        if (e.attr & kNotLocal) continue;  // ne jamais provoquer de téléchargement cloud
        bool& got = e.isDir() ? dir : file;
        if (got) continue;
        got = true;
        names.emplace_back(d.name(e), e.nameLen);
        if (dir && file) break;
    }
    ShellMenu_WarmUp(d.path, names);
}

void OnResult(Result* raw) {
    std::unique_ptr<Result> r(raw);
    if (r->gen != Jobs_Current()) return;  // périmé
    KillTimer(g_hwnd, TIMER_LOADING);

    if (r->kind == JobKind::Sort) {
        if (r->data == g_view.data) ApplyOrder(std::move(r->order), r->sortMs);
        return;
    }

    Pending p = std::move(g_pending);
    g_pending = Pending{};
    const DirData& d = *r->data;

    if (d.error) {
        SetWindowTextW(g_addr, g_path.c_str());
        if (!p.fallbackOpen.empty() && (p.fallbackAnyError || d.error == ERROR_DIRECTORY)) {
            ShowSummary();  // ce n'était pas un dossier : on l'ouvre, sans message d'erreur
            OpenWithShell(p.fallbackOpen);
            return;
        }
        SetStatus(L"Impossible d'ouvrir « " + (d.path.empty() ? std::wstring(L"Ce PC") : d.path) + L" » : " +
                  ErrorText(d.error));
        if (!HasView()) Navigate(L"", Nav::Normal);  // démarrage raté → Ce PC
        return;
    }

    if (HasView()) {
        switch (p.nav) {
            case Nav::Normal:
                if (_wcsicmp(g_path.c_str(), d.path.c_str()) != 0) { g_back.push_back(g_path); g_fwd.clear(); }
                break;
            case Nav::Back:
                if (!g_back.empty()) g_back.pop_back();
                g_fwd.push_back(g_path);
                break;
            case Nav::Forward:
                if (!g_fwd.empty()) g_fwd.pop_back();
                g_back.push_back(g_path);
                break;
            case Nav::Refresh: break;
        }
    }

    // Remonté à un parent : ce n'est plus un rafraîchissement du même dossier.
    const bool refresh = p.nav == Nav::Refresh && d.vanished.empty();
    // Un seul repaint, pas de clignotement. Inutile (et coûteux : ~13 ms) au
    // premier affichage, quand la liste est encore vide.
    const bool freeze = HasView();
    if (freeze) SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);

    // Icônes : préréglées (lecteurs), sinon à résoudre à l'affichage. Sur un
    // rafraîchissement du même dossier, on reprend celles déjà connues (pas
    // de clignotement générique → propre, pas de nouvelle demande).
    const size_t n = d.entries.size();
    std::vector<int32_t> icons(n, -1);
    std::vector<uint8_t> overlays(n, 0), states(n, ICON_NONE);
    for (size_t i = 0; i < n; ++i)
        if (d.entries[i].icon >= 0) { icons[i] = d.entries[i].icon; states[i] = ICON_OWN; }
    if (refresh && HasView() && g_view.data->path == d.path) {
        const DirData& old = *g_view.data;
        std::unordered_map<std::wstring_view, int> oldIdx;
        oldIdx.reserve(old.entries.size());
        for (size_t i = 0; i < old.entries.size(); ++i)
            oldIdx.emplace(std::wstring_view(old.name(old.entries[i]), old.entries[i].nameLen), (int)i);
        for (size_t j = 0; j < n; ++j) {
            auto it = oldIdx.find(std::wstring_view(d.name(d.entries[j]), d.entries[j].nameLen));
            if (it == oldIdx.end()) continue;
            const uint8_t st = g_view.iconState[it->second];
            if (st != ICON_GENERIC && st != ICON_OWN) continue;  // une demande en vol sera refaite
            icons[j] = g_view.icons[it->second];
            overlays[j] = g_view.overlays[it->second];
            states[j] = st;
        }
    }

    g_view.data = r->data;
    g_view.order = std::move(r->order);
    g_view.sortMs = r->sortMs;
    g_view.icons = std::move(icons);
    g_view.overlays = std::move(overlays);
    g_view.iconState = std::move(states);
    g_view.serial = ++g_viewSerial;
    Icons_SetSerial(g_view.serial);
    RebuildRowOf();
    g_path = d.path;
    ApplyCutMarks();

    // Surveillance du dossier (pas de « Ce PC » : les lecteurs viendront avec le bloc 4).
    if (g_path.empty()) {
        Watch_Stop();
        g_watchPath.clear();
        g_watchFailed.clear();
    } else if (g_watchPath != g_path && g_watchFailed != g_path) {
        g_watchPath = g_path;
        g_watchFailed.clear();
        Watch_Start(g_hwnd, WM_APP_DIRCHANGED, g_path, ++g_watchId);
    }

    SetWindowTextW(g_addr, g_path.c_str());
    SetWindowTextW(g_hwnd, (L"FastExplorer — " + (g_path.empty() ? std::wstring(L"Ce PC") : g_path)).c_str());
    ListView_SetItemCountEx(g_list, (int)d.entries.size(), refresh ? LVSICF_NOSCROLL : 0);
    if (refresh) {
        ApplySelection(FindEntriesByNames(p.selected), FindEntryByName(p.focus), false);
    } else {
        const int f = FindEntryByName(p.focus);
        ApplySelection(f >= 0 ? std::vector<int>{f} : std::vector<int>{}, f, true);
    }

    if (freeze) SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateButtons();
    if (g_startupMs < 0) g_startupMs = MsSinceProcessStart();  // premier affichage réellement peint
    if (!d.vanished.empty()) g_goneNote = L"« " + d.vanished + L" » n'est plus accessible";
    else if (p.nav != Nav::Refresh) g_goneNote.clear();
    ShowSummary();
    if (!g_path.empty()) WarmUpMenus(d);
    RegisterShellNotify();

    // Nouvel élément créé par une commande du fond du dossier : renommage direct.
    if (g_expectNew.before) {
        if (GetTickCount64() > g_expectNew.deadline || _wcsicmp(g_expectNew.before->path.c_str(), d.path.c_str()) != 0) {
            g_expectNew = {};
        } else if (refresh && g_editAfter.empty()) {
            const DirData& old = *g_expectNew.before;
            std::unordered_set<std::wstring_view> had;
            had.reserve(old.entries.size());
            for (const Entry& e : old.entries) had.emplace(old.name(e), e.nameLen);
            std::vector<int> added;
            for (size_t i = 0; i < d.entries.size(); ++i)
                if (!had.count(std::wstring_view(d.name(d.entries[i]), d.entries[i].nameLen))) added.push_back((int)i);
            if (g_expectNew.selectAll) {
                if (!added.empty()) ApplySelection(added, added[0], true);
            } else if (added.size() == 1) {
                g_editAfter = NameOf(added[0]);
            }
            if (!added.empty()) g_expectNew = {};
        }
    }
    if (!g_editAfter.empty()) {
        const int e = refresh ? FindEntryByName(g_editAfter) : -1;
        g_editAfter.clear();
        if (e >= 0) {
            const int row = g_view.rowOf[e];
            ApplySelection({e}, e, true);
            SetFocus(g_list);
            ListView_EditLabel(g_list, row);
        }
    }
}

void GoBack() { if (!g_back.empty()) Navigate(g_back.back(), Nav::Back); }
void GoForward() { if (!g_fwd.empty()) Navigate(g_fwd.back(), Nav::Forward); }
void GoUp() {
    std::wstring parent, child;
    if (HasView() && ParentOf(g_path, parent, child)) Navigate(parent, Nav::Normal, child);
}
void Refresh() {
    if (!HasView()) return;
    std::vector<std::wstring> names;
    for (int e : SelectedEntries()) names.emplace_back(NameOf(e));
    const int f = FocusedEntry();
    Navigate(g_path, Nav::Refresh, f >= 0 ? NameOf(f) : L"", std::move(names));
}

void NavigateAddress() {
    const int n = GetWindowTextLengthW(g_addr);
    std::wstring t(n + 1, L'\0');
    GetWindowTextW(g_addr, t.data(), n + 1);
    t.resize(n);
    const std::wstring path = NormalizePath(t, g_path);
    Navigate(path, Nav::Normal, {}, {}, path);  // un fichier tapé dans la barre s'ouvre
    SetFocus(g_list);
}

bool IsLnk(int idx) {
    const Entry& e = E(idx);
    return e.nameLen - e.extOff == 4 && _wcsicmp(NameOf(idx) + e.extOff, L".lnk") == 0;
}

void ActivateEntry(int idx) {
    if (idx < 0) return;
    const std::wstring full = Join(g_path, NameOf(idx));
    if (E(idx).isDir()) {
        Navigate(full, Nav::Normal);
        return;
    }
    // Raccourci : on navigue DANS FastExplorer si la cible est un dossier
    // (le Shell ouvrirait l'Explorateur Windows). Sinon le Shell l'ouvre,
    // y compris pour une cible disparue (il propose de la rechercher).
    std::wstring target;
    if (IsLnk(idx) && ResolveShortcut(full, target)) {
        Navigate(NormalizePath(target, L""), Nav::Normal, {}, {}, full, true);
        return;
    }
    OpenWithShell(full);
}

void Activate(int row) { ActivateEntry(EntryAt(row)); }

// ───────────────────────────── Menu contextuel, propriétés ─────────────────────────────

std::vector<std::wstring> NamesOf(const std::vector<int>& entries) {
    std::vector<std::wstring> v;
    v.reserve(entries.size());
    for (int e : entries) v.emplace_back(NameOf(e));
    return v;
}

// ───────────────────────────── Opérations sur les fichiers ─────────────────────────────

bool CanModify() { return HasView() && !g_view.data->isRoot(); }

void StartOp(FileOp op) { FileOps_Start(g_hwnd, g_hwnd, WM_APP_FILEOP, std::move(op)); }

// Rafraîchit le dossier en sélectionnant `names` (et renomme `edit`), dès
// qu'aucun renommage ni aucune navigation n'est en cours.
void ApplyAfterOp() {
    if (!g_afterOp.active) return;
    if (g_editing || g_pending.active) {
        SetTimer(g_hwnd, TIMER_AFTEROP, 100, nullptr);
        return;
    }
    KillTimer(g_hwnd, TIMER_AFTEROP);
    AfterOp a = std::move(g_afterOp);
    g_afterOp = AfterOp{};
    if (!HasView() || _wcsicmp(a.folder.c_str(), g_path.c_str()) != 0) return;  // on est allé ailleurs
    g_editAfter = a.edit;
    Navigate(g_path, Nav::Refresh, a.focus, std::move(a.names));
}

void AfterOpSelect(const std::wstring& folder, std::vector<std::wstring> names, std::wstring focus,
                   std::wstring edit = {}) {
    g_afterOp = AfterOp{true, folder, std::move(focus), std::move(edit), std::move(names)};
    ApplyAfterOp();
}

void OnFileOp(FileOpResult* raw) {
    std::unique_ptr<FileOpResult> r(raw);
    if (FileOps_IsFailure(*r)) SetStatus(L"Opération impossible : " + ErrorText((DWORD)r->hr));
    if (r->clipboardCut && SUCCEEDED(r->hr) && !r->aborted) Clipboard_ClearAfterMove(r->clipSeq);
    if (g_quitWhenIdle) {
        if (FileOps_Running() == 0) DestroyWindow(g_hwnd);
        return;
    }
    const auto& c = r->created;
    switch (r->kind) {
        case FileOpKind::NewFolder:
            if (!c.empty()) AfterOpSelect(r->folder, {c[0]}, c[0], c[0]);
            break;
        case FileOpKind::Delete:
            AfterOpSelect(r->folder, r->focusAfter.empty() ? std::vector<std::wstring>{} : std::vector{r->focusAfter},
                          r->focusAfter);
            break;
        case FileOpKind::Rename:
        case FileOpKind::Copy:
        case FileOpKind::Move:
            if (!c.empty()) AfterOpSelect(r->folder, c, c[0]);  // sinon la surveillance suffit
            break;
    }
}

void CopyNames(const std::vector<std::wstring>& names, bool cut) {
    if (!CanModify() || names.empty()) return;
    if (!Clipboard_Put(g_hwnd, g_path, names, cut)) {
        SetStatus(L"Impossible d'utiliser le presse-papiers.");
        return;
    }
    g_cut.folder = cut ? g_path : std::wstring();
    g_cut.names.clear();
    if (cut) g_cut.names.insert(names.begin(), names.end());
    ApplyCutMarks();
    InvalidateRect(g_list, nullptr, FALSE);
    wchar_t s[128];
    swprintf_s(s, L"%zu élément%s %s dans le presse-papiers.", names.size(), names.size() > 1 ? L"s" : L"",
               cut ? L"coupé(s)" : L"copié(s)");
    SetStatus(g_goneNote.empty() ? std::wstring(s) : g_goneNote + L"   •   " + s);
}

void OnClipboardUpdate() {
    if (g_cut.names.empty() || Clipboard_IsOurs()) return;
    g_cut.names.clear();  // le presse-papiers a changé : plus rien n'est « coupé »
    ApplyCutMarks();
    InvalidateRect(g_list, nullptr, FALSE);
}

void Paste(const std::wstring& dest) {
    if (!CanModify() || dest.empty()) return;
    FileOp op;
    if (!Clipboard_Get(op.items, op.clipboardCut, op.clipSeq)) {
        SetStatus(L"Rien à coller : le presse-papiers ne contient pas de fichiers.");
        return;
    }
    op.kind = op.clipboardCut ? FileOpKind::Move : FileOpKind::Copy;
    op.folder = dest;
    StartOp(std::move(op));
}

// Supprime ; la cible passe ensuite à l'élément suivant (ou précédent), comme dans l'Explorateur.
void DeleteNames(const std::vector<std::wstring>& names, bool permanent) {
    if (!CanModify() || names.empty() || !ConfirmDelete(g_hwnd, names, permanent)) return;
    std::vector<int> rows;
    for (int e : FindEntriesByNames(names)) rows.push_back(g_view.rowOf[e]);
    std::sort(rows.begin(), rows.end());
    std::wstring next;
    if (!rows.empty()) {
        const std::unordered_set<int> gone(rows.begin(), rows.end());
        int r = -1;
        for (int k = rows.back() + 1; k < RowCount() && r < 0; ++k)
            if (!gone.count(k)) r = k;
        for (int k = rows.front() - 1; k >= 0 && r < 0; --k)
            if (!gone.count(k)) r = k;
        if (r >= 0) next = NameOf(EntryAt(r));
    }
    FileOp op;
    op.kind = FileOpKind::Delete;
    op.folder = g_path;
    op.permanent = permanent;
    op.focusAfter = next;
    for (const auto& n : names) op.paths.push_back(Join(g_path, n.c_str()));
    StartOp(std::move(op));
}

void NewFolder() {
    if (!CanModify()) return;
    FileOp op;
    op.kind = FileOpKind::NewFolder;
    op.folder = g_path;
    op.newName = L"Nouveau dossier";
    StartOp(std::move(op));
}

void BeginRename() {
    if (!CanModify()) return;
    int row = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (row < 0) row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (row < 0) return;
    SetFocus(g_list);
    ListView_EditLabel(g_list, row);
}

BOOL OnBeginLabelEdit(const NMLVDISPINFOW* di) {
    const int idx = EntryAt(di->item.iItem);
    if (!CanModify() || idx < 0 || g_pending.active) return TRUE;  // refusé
    g_editing = true;
    g_editName = NameOf(idx);
    PostMessageW(g_hwnd, WM_APP_EDITSEL, 0, 0);
    return FALSE;
}

// Après l'ouverture de la saisie : nom sans l'extension sélectionné, ou texte
// refusé remis en place avec la raison dans une bulle.
void OnEditSel() {
    HWND ed = ListView_GetEditControl(g_list);
    if (!ed || !g_editing) return;
    if (g_reedit.error) {
        SetWindowTextW(ed, g_reedit.text.c_str());
        Edit_SetSel(ed, 0, -1);
        EDITBALLOONTIP bt{sizeof bt, L"Nom incorrect", g_reedit.error, TTI_ERROR};
        Edit_ShowBalloonTip(ed, &bt);
        g_reedit = Reedit{};
        return;
    }
    const int idx = FindEntryByName(g_editName);
    if (idx < 0 || E(idx).isDir()) return;
    const wchar_t* name = NameOf(idx);
    const int dot = (int)(PathFindExtensionW(name) - name);
    if (dot > 0 && name[dot]) Edit_SetSel(ed, 0, dot);
}

void OnReedit() {
    const int idx = FindEntryByName(g_reedit.name);
    if (idx < 0 || !HasView()) { g_reedit = Reedit{}; return; }
    SetFocus(g_list);
    ListView_EditLabel(g_list, g_view.rowOf[idx]);
}

void OnEndLabelEdit(const NMLVDISPINFOW* di) {
    g_editing = false;
    const std::wstring old = g_editName;
    g_editName.clear();
    if (!di->item.pszText || old.empty()) return;  // Échap
    const std::wstring typed = di->item.pszText;
    const std::wstring name = CleanFileName(typed);
    if (name.empty() || name == old) return;
    if (const wchar_t* err = FileNameError(name)) {
        g_reedit = Reedit{old, typed, err};
        PostMessageW(g_hwnd, WM_APP_REEDIT, 0, 0);
        return;
    }
    const int idx = FindEntryByName(old);
    if (idx >= 0 && !E(idx).isDir() && _wcsicmp(PathFindExtensionW(old.c_str()), PathFindExtensionW(name.c_str())) != 0 &&
        MessageBoxW(g_hwnd,
                    L"Si vous modifiez l'extension du nom de fichier, le fichier risque de devenir inutilisable.\n\n"
                    L"Voulez-vous vraiment la modifier ?",
                    L"Renommer", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    FileOp op;
    op.kind = FileOpKind::Rename;
    op.folder = g_path;
    op.paths.push_back(Join(g_path, old.c_str()));
    op.newName = name;
    StartOp(std::move(op));
}

// Le moteur de copie du Shell active sa fenêtre de progression dès le début,
// encore invisible (~100 ms, même pour un seul petit fichier) : les touches
// tapées juste après Ctrl+V ou un renommage partiraient dans le vide. On
// reprend aussitôt le premier plan tant que cette fenêtre reste invisible ;
// une fois affichée (longue copie), elle garde la main comme dans l'Explorateur.
void ReclaimForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || IsWindowVisible(fg)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, _countof(cls));
    if (pid == GetCurrentProcessId() && wcscmp(cls, L"OperationStatusWindow") == 0) SetForegroundWindow(g_hwnd);
}

// ───────────────────────────── Glisser-déposer ─────────────────────────────

void StartDrag(POINT client) {
    if (!HasView()) return;
    const std::vector<std::wstring> names = NamesOf(SelectedEntries());
    if (names.empty()) return;
    g_dragNames.clear();
    g_dragNames.insert(names.begin(), names.end());
    DragDrop_Start(g_list, g_path, names, client);  // bloquant jusqu'au dépôt
    g_dragNames.clear();
    ShellMenu_Discard();  // menu préparé au bouton droit enfoncé : c'était un glisser
}

void SetDropRow(int row) {
    if (row == g_dropRow) return;
    const int old = g_dropRow;
    g_dropRow = row;
    if (old >= 0 && old < RowCount()) ListView_RedrawItems(g_list, old, old);
    if (row >= 0) ListView_RedrawItems(g_list, row, row);
    UpdateWindow(g_list);
}

void RegisterDrop() {
    DropHost h;
    h.folder = [] { return g_path; };
    h.hit = [](POINT c, std::wstring& name) {
        if (!HasView() || g_pending.active) return -1;
        LVHITTESTINFO hit{};
        hit.pt = c;
        const int row = ListView_HitTest(g_list, &hit);
        const int e = EntryAt(row);
        if (e < 0) return -1;
        name = NameOf(e);
        return row;
    };
    h.dragged = [](const std::wstring& n) { return g_dragNames.count(n) > 0; };
    h.highlight = SetDropRow;
    h.dropped = [](bool intoFolder) {
        if (intoFolder && HasView()) g_expectNew = {g_view.data, GetTickCount64() + 15000, true};
    };
    DragDrop_RegisterTarget(g_list, std::move(h));
}

void ShowContextMenu(LPARAM lp) {
    if (!HasView()) return;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    std::vector<int> sel = SelectedEntries();
    if (pt.x == -1 && pt.y == -1) {  // clavier (Maj+F10, touche Menu) : sous l'élément ciblé
        const int r = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
        RECT rc{};
        if (r >= 0 && !sel.empty() && ListView_GetItemRect(g_list, r, &rc, LVIR_LABEL)) pt = {rc.left + S(8), rc.bottom};
        else pt = {S(8), S(8)};
        ClientToScreen(g_list, &pt);
    } else {
        POINT c = pt;
        ScreenToClient(g_list, &c);
        LVHITTESTINFO hit{};
        hit.pt = c;
        if (ListView_HitTest(g_list, &hit) < 0) sel.clear();  // clic dans le vide : menu du dossier
    }
    // Les noms (et non des index) : la vue peut être rafraîchie pendant que le menu est ouvert.
    const std::vector<std::wstring> names = NamesOf(sel);
    const bool ok = ShellMenu_Show(g_hwnd, g_path, names, pt, GetKeyState(VK_SHIFT) < 0, [names](const std::wstring& verb) {
        auto is = [&](const wchar_t* v) { return _wcsicmp(verb.c_str(), v) == 0; };
        const int single = names.size() == 1 ? FindEntryByName(names[0]) : -1;
        const bool background = names.empty();
        // « Ouvrir » sur un dossier ou un raccourci : chez nous, pas dans l'Explorateur.
        if (single >= 0 && (is(L"open") || is(L"explore"))) { ActivateEntry(single); return true; }
        // Opérations de fichiers : notre moteur (resélection, estompage des éléments coupés…).
        if (!background && (is(L"copy") || is(L"cut"))) { CopyNames(names, is(L"cut")); return true; }
        if (is(L"paste")) {
            Paste(single >= 0 && E(single).isDir() ? Join(g_path, NameOf(single)) : g_path);
            return true;
        }
        if (!background && is(L"delete")) { DeleteNames(names, GetKeyState(VK_SHIFT) < 0); return true; }
        if (single >= 0 && is(L"rename")) { ListView_EditLabel(g_list, g_view.rowOf[single]); return true; }
        if (is(L"NewFolder")) { NewFolder(); return true; }
        if (background) g_expectNew = {g_view.data, GetTickCount64() + 10000};
        return false;
    });
    if (!ok) SetStatus(L"Menu contextuel indisponible pour cet emplacement.");
}

// Bouton droit enfoncé : on construit le menu pendant que l'utilisateur relâche
// (un clic dure ~100 ms, autant que les gestionnaires du Shell). La sélection
// est celle que la liste aura au WM_CONTEXTMENU : l'élément cliqué s'il n'était
// pas sélectionné, la sélection entière sinon, le fond du dossier dans le vide.
// Si la prédiction se révèle fausse, ShellMenu_Show reconstruit simplement.
void PrepareContextMenu(LPARAM lp, bool shift) {
    if (!HasView()) return;
    LVHITTESTINFO hit{};
    hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const int row = ListView_HitTest(g_list, &hit);
    std::vector<int> sel;
    if (row >= 0) {
        if (ListView_GetItemState(g_list, row, LVIS_SELECTED) & LVIS_SELECTED) sel = SelectedEntries();
        else if (int e = EntryAt(row); e >= 0) sel = {e};
        else return;
    }
    ShellMenu_Prepare(g_hwnd, g_path, NamesOf(sel), shift);
}

// Alt+Entrée : propriétés de la sélection, ou du dossier courant si rien n'est sélectionné.
void ShowProperties() {
    if (!HasView()) return;
    std::vector<std::wstring> names = NamesOf(SelectedEntries());
    std::wstring folder = g_path;
    if (names.empty()) {
        std::wstring parent, child;
        if (!ParentOf(g_path, parent, child)) return;  // Ce PC : rien à afficher
        folder = parent;
        names.push_back(child);
    }
    if (!ShellMenu_Invoke(g_hwnd, folder, names, "properties")) SetStatus(L"Propriétés indisponibles.");
}

void OnIcon(IconResult* raw) {
    std::unique_ptr<IconResult> r(raw);
    if (!HasView() || r->serial != g_view.serial || r->idx >= g_view.icons.size()) return;
    g_view.icons[r->idx] = r->icon;
    g_view.overlays[r->idx] = (uint8_t)r->overlay;
    g_view.iconState[r->idx] = ICON_OWN;
    const int row = g_view.rowOf[r->idx];
    if (row >= 0) ListView_RedrawItems(g_list, row, row);
}

// Changement sur disque : rafraîchissement différé (regroupe les rafales), plus
// espacé sur les gros dossiers pour ne pas relire 30 000 entrées en boucle.
void OnDirChanged(uintptr_t id, LPARAM kind) {
    if (id != g_watchId) return;
    if (kind == kWatchOpened) { Watch_Opened(g_hwnd, id); return; }
    if (kind == kWatchGone) {  // dossier supprimé, lecteur retiré, réseau perdu
        Watch_Stop();
        g_watchFailed = g_watchPath;  // s'il est toujours lisible, on ne relance pas la surveillance
        g_watchPath.clear();
        if (!g_pending.active && !g_editing) Refresh();  // remonte au plus proche parent
        else if (!g_watchPending) { g_watchPending = true; SetTimer(g_hwnd, TIMER_WATCH, (UINT)kWatchMinDelayMs, nullptr); }
        return;
    }
    if (g_watchPending) return;
    double delay = kWatchMinDelayMs;
    if (HasView()) delay = std::clamp(4 * (g_view.data->enumMs + g_view.data->keyMs + g_view.sortMs), kWatchMinDelayMs,
                                      kWatchMaxDelayMs);
    g_watchPending = true;
    SetTimer(g_hwnd, TIMER_WATCH, (UINT)delay, nullptr);
}

// ── Lecteurs : arrivée, retrait, éjection ──

wchar_t DriveOf(const std::wstring& p) {
    return (p.size() >= 2 && p[1] == L':') ? (wchar_t)towupper(p[0]) : 0;
}

// Lecteurs annoncés (masque : bit 0 = A:). Ce PC se met à jour ; si le dossier
// affiché était sur un lecteur retiré, on remonte (jusqu'à Ce PC).
void OnDrivesChanged(DWORD removedMask) {
    if (!HasView()) return;
    const wchar_t d = DriveOf(g_path);
    if (d && (removedMask & (1u << (d - L'A')))) {
        Watch_Stop();
        g_watchPath.clear();
        SetTimer(g_hwnd, TIMER_EJECT, 50, nullptr);  // rafraîchit (remonte) dès que possible
        return;
    }
    if (g_path.empty()) SetTimer(g_hwnd, TIMER_DRIVES, 300, nullptr);
}

LRESULT OnDeviceChange(WPARAM wp, LPARAM lp) {
    const auto* hdr = (const DEV_BROADCAST_HDR*)lp;
    if (!hdr) return TRUE;
    if (hdr->dbch_devicetype == DBT_DEVTYP_HANDLE && wp == DBT_DEVICEQUERYREMOVE) {
        // Éjection demandée du lecteur affiché : on le libère tout de suite
        // (sinon Windows répond « périphérique en cours d'utilisation »).
        Watch_Stop(true);
        g_watchPath.clear();
        ShellMenu_Discard();
        SetTimer(g_hwnd, TIMER_EJECT, 3000, nullptr);  // éjection refusée ailleurs : on reprendra la surveillance
        return TRUE;
    }
    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME &&
        (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
        const auto* v = (const DEV_BROADCAST_VOLUME*)lp;
        OnDrivesChanged(wp == DBT_DEVICEREMOVECOMPLETE ? v->dbcv_unitmask : 0);
    }
    return TRUE;
}

// Annonces du Shell (lecteur réseau connecté, média inséré…) : complètent WM_DEVICECHANGE.
void OnShellNotify(WPARAM wp, LPARAM lp) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG ev = 0;
    HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &ev);
    if (!lock) return;
    DWORD removed = 0;
    if ((ev & (SHCNE_DRIVEREMOVED | SHCNE_MEDIAREMOVED)) && pidls && pidls[0]) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidls[0], path))
            if (const wchar_t d = DriveOf(path)) removed = 1u << (d - L'A');
    }
    SHChangeNotification_Unlock(lock);
    OnDrivesChanged(removed);
}

void RegisterShellNotify() {
    if (g_shellNotify || !g_hwnd) return;
    SHChangeNotifyEntry e{nullptr, TRUE};  // tous les emplacements
    g_shellNotify = SHChangeNotifyRegister(g_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                           SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED | SHCNE_MEDIAINSERTED |
                                               SHCNE_MEDIAREMOVED,
                                           WM_APP_SHELLNOTIFY, 1, &e);
}

void OnWatchTimer() {
    KillTimer(g_hwnd, TIMER_WATCH);
    if (g_pending.active || g_editing) {  // navigation ou renommage en cours : on réessaie plus tard
        SetTimer(g_hwnd, TIMER_WATCH, (UINT)kWatchMinDelayMs, nullptr);
        return;
    }
    g_watchPending = false;
    Refresh();
}

// ───────────────────────────── Fenêtre ─────────────────────────────

void ApplyFont() {
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (HDC dc = GetDC(g_hwnd)) {
        HGDIOBJ old = SelectObject(dc, g_font);
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) g_fontH = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(g_hwnd, dc);
    }
    for (HWND h : {g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

// Liste d'icônes système la plus proche de 16 px × échelle de l'écran courant.
// Les index d'icônes sont communs à toutes les listes système.
void ApplyImageList() {
    if (!g_imgSmall) {
        IImageList* il = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_SMALL, __uuidof(IImageList), (void**)&il))) g_imgSmall = (HIMAGELIST)il;
        if (SUCCEEDED(SHGetImageList(SHIL_LARGE, __uuidof(IImageList), (void**)&il))) g_imgLarge = (HIMAGELIST)il;
    }
    int cs = 16, cl = 32, cy = 0;
    if (g_imgSmall) ImageList_GetIconSize(g_imgSmall, &cs, &cy);
    if (g_imgLarge) ImageList_GetIconSize(g_imgLarge, &cl, &cy);
    const int want = S(16);
    HIMAGELIST pick = (g_imgLarge && std::abs(cl - want) < std::abs(cs - want)) ? g_imgLarge : g_imgSmall;
    if (pick) ListView_SetImageList(g_list, pick, LVSIL_SMALL);
}

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT rs;
    GetWindowRect(g_status, &rs);
    const int statusH = rs.bottom - rs.top;
    const int m = S(6), bw = S(34);
    const int editH = g_fontH + S(10);  // texte centré verticalement
    const int h = std::max(S(30), editH);
    // Un seul lot de déplacements : un repaint au lieu d'un par contrôle.
    HDWP dwp = BeginDeferWindowPos(5);
    auto place = [&](HWND w, int x, int y, int cx, int cy) {
        if (dwp) dwp = DeferWindowPos(dwp, w, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
        else MoveWindow(w, x, y, cx, cy, TRUE);
    };
    int x = m;
    for (HWND b : {g_btnBack, g_btnFwd, g_btnUp}) {
        place(b, x, m, bw, h);
        x += bw + S(2);
    }
    x += S(4);
    place(g_addr, x, m + (h - editH) / 2, std::max(0, (int)rc.right - x - m), editH);
    const int top = m + h + m;
    place(g_list, 0, top, rc.right, std::max(0, (int)rc.bottom - top - statusH));
    if (dwp) EndDeferWindowPos(dwp);
}

// Initialise le Shell (listes d'icônes système) sur un thread à part, pendant
// que la fenêtre se construit : ~20 ms de moins sur le thread de l'interface.
void WarmUpShell() {
    std::thread([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IImageList* il = nullptr;
        SHGetImageList(SHIL_SMALL, __uuidof(IImageList), (void**)&il);
        SHGetImageList(SHIL_LARGE, __uuidof(IImageList), (void**)&il);
        SHFILEINFOW sfi{};
        SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof sfi,
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}

void CreateChildren(HWND hwnd) {
    auto btn = [&](const wchar_t* t, int id) {
        return CreateWindowExW(0, WC_BUTTONW, t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                               (HMENU)(INT_PTR)id, g_inst, nullptr);
    };
    g_btnBack = btn(L"←", ID_BACK);
    g_btnFwd = btn(L"→", ID_FWD);
    g_btnUp = btn(L"↑", ID_UP);

    g_addr = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             0, 0, 0, 0, hwnd, (HMENU)ID_ADDR, g_inst, nullptr);
    Edit_SetCueBannerText(g_addr, L"Ce PC");

    g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHAREIMAGELISTS |
                                 LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                             0, 0, 0, 0, hwnd, (HMENU)ID_LIST, g_inst, nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    ListView_SetCallbackMask(g_list, LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED);  // fournis par LVN_GETDISPINFO
    SetWindowTheme(g_list, L"Explorer", nullptr);

    struct {
        const wchar_t* t;
        int w;
        int fmt;
    } cols[COL_COUNT] = {{L"Nom", 380, LVCFMT_LEFT},
                         {L"Taille", 110, LVCFMT_RIGHT},
                         {L"Type", 170, LVCFMT_LEFT},
                         {L"Modifié le", 170, LVCFMT_LEFT}};
    for (int i = 0; i < COL_COUNT; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        c.fmt = cols[i].fmt;
        c.iSubItem = i;
        ListView_InsertColumn(g_list, i, &c);
    }

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                               (HMENU)ID_STATUS, g_inst, nullptr);
    ApplyFont();
    ApplyImageList();
    UpdateSortArrow();
    UpdateButtons();
    Layout();  // fenêtre encore invisible : aucun repaint
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (LRESULT lr = 0; ShellMenu_HandleMessage(msg, wp, lp, &lr)) return lr;
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            g_dpi = GetDpiForWindow(hwnd);
            CreateChildren(hwnd);
            Icons_Start(hwnd, WM_APP_ICON);
            AddClipboardFormatListener(hwnd);
            RegisterDrop();
            AttachTarget(hwnd);
            return 0;
        case WM_CONTEXTMENU:
            if ((HWND)wp == g_list) { ShowContextMenu(lp); return 0; }
            break;
        case WM_APP_ICON: OnIcon((IconResult*)lp); return 0;
        case WM_APP_DIRCHANGED: OnDirChanged((uintptr_t)wp, lp); return 0;
        case WM_DEVICECHANGE: return OnDeviceChange(wp, lp);
        case WM_APP_SHELLNOTIFY: OnShellNotify(wp, lp); return 0;
        case WM_APP_FILEOP: OnFileOp((FileOpResult*)lp); return 0;
        case WM_APP_EDITSEL: OnEditSel(); return 0;
        case WM_APP_REEDIT: OnReedit(); return 0;
        case WM_APP_RECLAIM: ReclaimForeground(); return 0;
        case WM_ACTIVATE:
            // (y compris juste après la fin d'une opération : la fenêtre du moteur peut encore s'activer)
            if (LOWORD(wp) == WA_INACTIVE) PostMessageW(hwnd, WM_APP_RECLAIM, 0, 0);
            break;
        case WM_CLIPBOARDUPDATE: OnClipboardUpdate(); return 0;
        case WM_CLOSE:
            // Une copie en cours continue jusqu'au bout : on masque la fenêtre et on attend.
            if (FileOps_Running() > 0) {
                g_quitWhenIdle = true;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        case WM_SIZE: Layout(); return 0;
        case WM_SETFOCUS: SetFocus(g_list); return 0;
        case WM_DPICHANGED: {
            const UINT nd = HIWORD(wp);
            for (int i = 0; i < COL_COUNT; ++i)
                ListView_SetColumnWidth(g_list, i, MulDiv(ListView_GetColumnWidth(g_list, i), (int)nd, (int)g_dpi));
            g_dpi = nd;
            ApplyFont();
            ApplyImageList();
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_LOADING) {
                KillTimer(hwnd, TIMER_LOADING);
                if (g_pending.active)
                    SetStatus(L"Chargement de " + (g_pending.path.empty() ? std::wstring(L"Ce PC") : g_pending.path) +
                              L"…");
                return 0;
            }
            if (wp == TIMER_WATCH) { OnWatchTimer(); return 0; }
            if (wp == TIMER_AFTEROP) { ApplyAfterOp(); return 0; }
            if (wp == TIMER_DRIVES || wp == TIMER_EJECT) {
                if (g_pending.active || g_editing) return 0;  // le minuteur reste armé : on réessaie
                KillTimer(hwnd, wp);
                if (HasView() && (wp == TIMER_EJECT || g_path.empty())) Refresh();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_BACK: GoBack(); SetFocus(g_list); return 0;
                case ID_FWD: GoForward(); SetFocus(g_list); return 0;
                case ID_UP: GoUp(); SetFocus(g_list); return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->hwndFrom != g_list) break;
            switch (hdr->code) {
                case LVN_GETDISPINFOW: FillDispInfo(((NMLVDISPINFOW*)lp)->item); return 0;
                case LVN_ODFINDITEMW: return FindItem((NMLVFINDITEMW*)lp);
                case LVN_COLUMNCLICK: Resort(((NMLISTVIEW*)lp)->iSubItem); return 0;
                case NM_DBLCLK: Activate(((NMITEMACTIVATE*)lp)->iItem); return 0;
                case LVN_BEGINDRAG:
                case LVN_BEGINRDRAG: StartDrag(((NMLISTVIEW*)lp)->ptAction); return 0;
                case LVN_BEGINLABELEDITW: return OnBeginLabelEdit((NMLVDISPINFOW*)lp);
                case LVN_ENDLABELEDITW: OnEndLabelEdit((NMLVDISPINFOW*)lp); return FALSE;  // la liste relira le nom
            }
            break;
        }
        case WM_APP_RESULT: OnResult((Result*)lp); return 0;
        case WM_DESTROY:
            g_target.store(nullptr);
            Icons_Stop();
            Watch_Stop();
            ShellMenu_Discard();
            RemoveClipboardFormatListener(hwnd);
            DragDrop_RevokeTarget(g_list);
            if (g_shellNotify) SHChangeNotifyDeregister(g_shellNotify);
            Clipboard_Flush();  // ce qui a été copié reste disponible après la fermeture
            Jobs_NewGeneration();  // périme et interrompt la tâche en cours
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Raccourcis globaux, traités avant TranslateMessage (pas de bip, pas de WM_CHAR parasite).
bool HandleGlobalKeys(const MSG& m) {
    if (m.message == WM_RBUTTONDOWN && m.hwnd == g_list) {
        PrepareContextMenu(m.lParam, (m.wParam & MK_SHIFT) != 0);
        return false;  // la liste traite ensuite le clic normalement (sélection)
    }
    if (g_editing) return false;  // saisie d'un nom : toutes les touches vont à la zone d'édition
    const HWND focus = GetFocus();
    if (m.message == WM_XBUTTONUP) {
        if (GET_XBUTTON_WPARAM(m.wParam) == XBUTTON1) GoBack(); else GoForward();
        return true;
    }
    if (m.message == WM_SYSKEYDOWN) {
        switch (m.wParam) {
            case VK_LEFT: GoBack(); return true;
            case VK_RIGHT: GoForward(); return true;
            case VK_UP: GoUp(); return true;
            case 'D': SetFocus(g_addr); Edit_SetSel(g_addr, 0, -1); return true;
            case VK_RETURN:
                if (focus == g_addr) return false;
                ShowProperties();
                return true;
        }
        return false;
    }
    if (m.message != WM_KEYDOWN) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (m.wParam) {
        case VK_F5: Refresh(); return true;
        case 'L':
            if (!ctrl) return false;
            SetFocus(g_addr);
            Edit_SetSel(g_addr, 0, -1);
            return true;
        case VK_TAB:
            if (focus == g_addr) SetFocus(g_list);
            else { SetFocus(g_addr); Edit_SetSel(g_addr, 0, -1); }
            return true;
        case VK_RETURN:
            if (focus == g_addr) { NavigateAddress(); return true; }
            if (focus == g_list) { Activate(ListView_GetNextItem(g_list, -1, LVNI_FOCUSED)); return true; }
            return false;
        case VK_ESCAPE:
            if (focus == g_addr) { SetWindowTextW(g_addr, g_path.c_str()); SetFocus(g_list); return true; }
            return false;
        case VK_BACK:
            if (focus == g_list) { GoUp(); return true; }
            return false;
    }
    if (focus != g_list) return false;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (m.wParam) {
        case VK_F2: BeginRename(); return true;
        case VK_DELETE: DeleteNames(NamesOf(SelectedEntries()), shift); return true;
        case 'C': if (!ctrl) return false; CopyNames(NamesOf(SelectedEntries()), false); return true;
        case 'X': if (!ctrl) return false; CopyNames(NamesOf(SelectedEntries()), true); return true;
        case 'V': if (!ctrl) return false; Paste(g_path); return true;
        case 'A': if (!ctrl) return false; ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED); return true;
        case 'N': if (!ctrl || !shift) return false; NewFolder(); return true;
        case 'D': if (!ctrl) return false; DeleteNames(NamesOf(SelectedEntries()), false); return true;
    }
    return false;
}

}  // namespace

bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start) {
    g_inst = inst;
    Navigate(start, Nav::Normal);  // la lecture démarre pendant qu'on construit la fenêtre
    WarmUpShell();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FastExplorerWnd";
    if (!RegisterClassExW(&wc)) return false;

    const UINT sd = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FastExplorer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1100, (int)sd, 96), MulDiv(700, (int)sd, 96), nullptr, nullptr,
                                inst, nullptr);
    if (!hwnd) return false;
    ShowWindow(hwnd, show);
    return true;
}

int Ui_Run() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (HandleGlobalKeys(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
