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
#include "crumbs.h"
#include "navpane.h"
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
constexpr UINT WM_APP_NAV = WM_APP + 9;         // résultat d'une lecture du volet de navigation
constexpr UINT WM_APP_SELSUM = WM_APP + 10;     // sélection changée : résumé dans la barre d'état (une fois par rafale)
constexpr UINT_PTR TIMER_LOADING = 1;
constexpr UINT_PTR TIMER_WATCH = 2;
constexpr UINT_PTR TIMER_AFTEROP = 3;
constexpr UINT_PTR TIMER_HOVER = 6;   // souris immobile sur la liste : préparer le menu contextuel
constexpr UINT kMenuHoverMs = 100;
constexpr UINT_PTR TIMER_PROGRESS = 7;  // progression des copies de notre moteur
constexpr UINT_PTR TIMER_PINNED = 8;    // après un menu contextuel : relire l'Accès rapide (épinglage)
constexpr UINT_PTR TIMER_SEARCH = 9;    // frappe dans la zone de recherche : lancer la recherche après une pause
constexpr UINT_PTR TIMER_DRIVES = 4;  // Ce PC : regroupe les annonces d'arrivée / retrait de lecteurs
constexpr UINT_PTR TIMER_EJECT = 5;   // éjection demandée : si elle échoue, on reprend la surveillance  // resélection après une opération, différée pendant un renommage
constexpr UINT kLoadingDelayMs = 150;   // en dessous, aucun « Chargement… » (évite le clignotement)
constexpr size_t kSyncSortMax = 20000;  // au-delà, le re-tri part en arrière-plan
// Rafraîchissement après un changement sur disque : regroupe les rafales
// d'événements (copie de milliers de fichiers) et s'espace sur les gros dossiers.
constexpr double kWatchMinDelayMs = 150, kWatchMaxDelayMs = 2000;

enum { ID_BACK = 100, ID_FWD, ID_UP, ID_ADDR, ID_LIST, ID_STATUS, ID_NAV, ID_CRUMBS, ID_SEARCH };
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
    std::wstring query;             // recherche dans `path` ("" : simple lecture)
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
HWND g_hwnd, g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp, g_nav, g_crumbs, g_searchBox;
std::wstring g_query;           // recherche affichée ("" : contenu ordinaire du dossier)
bool g_searchSync = false;      // texte de la zone de recherche changé par le programme
int g_navW96 = 240;            // largeur du volet de navigation (à 96 ppp)
bool g_splitDrag = false;      // séparateur volet / liste en cours de déplacement
RECT g_split{};                // zone du séparateur (coordonnées client)
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
ViewPrefs g_prefs;              // éléments cachés, extensions : réglages de l'Explorateur
std::wstring g_editExt;         // renommage : extension masquée à l'affichage, remise à la fin
double g_startupMs = -1;  // -1 = pas encore mesuré, 0 = déjà affiché
uint64_t g_viewSerial = 0;
uintptr_t g_watchId = 0;
std::wstring g_watchPath;
bool g_progressShown = false;  // la barre d'état montre une copie en cours
ITaskbarList3* g_taskbar = nullptr;
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

// Barre d'état : 0 = contenu du dossier et messages, 1 = sélection, 2 = temps de lecture.
enum { SB_MAIN, SB_SEL, SB_PERF };
void SetStatus(const std::wstring& s) { SendMessageW(g_status, SB_SETTEXTW, SB_MAIN, (LPARAM)s.c_str()); }
void SetStatusPart(int part, const std::wstring& s) { SendMessageW(g_status, SB_SETTEXTW, part, (LPARAM)s.c_str()); }

bool g_selPosted = false;
void ScheduleSelectionSummary() {
    if (g_selPosted || !g_hwnd) return;
    g_selPosted = PostMessageW(g_hwnd, WM_APP_SELSUM, 0, 0) != 0;
}

// Ouvre un fichier avec son application associée (comme un double-clic dans l'Explorateur).
void OpenWithShell(const std::wstring& full) {
    std::wstring dir, child;
    if (!ParentOf(full, dir, child)) dir.clear();
    const HINSTANCE r = ShellExecuteW(g_hwnd, nullptr, full.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(),
                                      SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) SetStatus(L"Impossible d'ouvrir « " + full + L" » : " + ErrorText(GetLastError()));
}

void ShowSummary() {
    if (!HasView() || g_progressShown) return;
    const DirData& d = *g_view.data;
    wchar_t s[512];
    size_t dirs = 0;
    for (uint32_t e : g_view.order) dirs += d.entries[e].isDir();
    const size_t shown = g_view.order.size(), hidden = d.entries.size() - shown;
    wchar_t masked[64] = L"";
    if (hidden) swprintf_s(masked, L", %zu masqué%s", hidden, hidden > 1 ? L"s" : L"");
    if (!d.query.empty()) {
        swprintf_s(s, L"%zu résultat%s pour « %s »%s", shown, shown > 1 ? L"s" : L"", d.query.c_str(),
                   d.searchDone ? (d.truncated ? L"   •   arrêtée : trop de résultats" : L"")
                                : L"   •   recherche en cours…");
        if (!d.searchDone) {
            const size_t k = wcslen(s);
            swprintf_s(s + k, _countof(s) - k, L" (%zu dossiers parcourus)", d.scanned);
        }
    } else {
        swprintf_s(s, L"%zu élément%s  (%zu dossier%s, %zu fichier%s%s)", shown, shown > 1 ? L"s" : L"", dirs,
                   dirs > 1 ? L"s" : L"", shown - dirs, shown - dirs > 1 ? L"s" : L"", masked);
    }
    SetStatus(g_goneNote.empty() ? std::wstring(s) : g_goneNote + L"   •   " + s);
    wchar_t perf[160];
    int k = swprintf_s(perf, L"lecture %.2f ms, clés %.2f ms, tri %.2f ms", d.enumMs, d.keyMs, g_view.sortMs);
    if (g_startupMs > 0 && k > 0) {
        swprintf_s(perf + k, _countof(perf) - k, L"  •  démarrage %.0f ms", g_startupMs);
        g_startupMs = 0;  // affiché une seule fois
    }
    SetStatusPart(SB_PERF, perf);
    ScheduleSelectionSummary();
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

// rowOf : par entrée, sa ligne, ou -1 si elle est masquée (éléments cachés).
void RebuildRowOf() {
    const int n = RowCount();
    g_view.rowOf.assign(HasView() ? g_view.data->entries.size() : 0, -1);
    for (int r = 0; r < n; ++r) g_view.rowOf[g_view.order[r]] = r;
}

// Applique une sélection exprimée en index d'entrées (indépendante du tri).
void ApplySelection(const std::vector<int>& sel, int focus, bool scroll) {
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (!RowCount()) return;
    const std::vector<int>& rowOf = g_view.rowOf;
    const int n = (int)rowOf.size();
    // (-1 à SetItemState sélectionnerait tout : les entrées masquées sont ignorées)
    for (int e : sel)
        if (e >= 0 && e < n && rowOf[e] >= 0) ListView_SetItemState(g_list, rowOf[e], LVIS_SELECTED, LVIS_SELECTED);
    if (focus >= 0 && focus < n && rowOf[focus] >= 0) {
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
    if (e.icon >= 0) return e.icon;  // déjà résolue par le thread de travail (lecteurs de Ce PC)
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

// Ce PC : informations du lecteur de l'entrée (nom, occupation).
const DriveInfo* DriveOfEntry(int idx) {
    if (!HasView() || !g_view.data->isRoot() || idx < 0 || (size_t)idx >= g_view.data->drives.size()) return nullptr;
    return &g_view.data->drives[idx];
}

std::wstring BytesText(uint64_t bytes) {
    wchar_t b[64];
    StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, _countof(b));
    return b;
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
        // Élément caché : estompé, comme dans l'Explorateur (même rendu qu'un élément coupé).
        const bool dim = g_view.cut[idx] || (!g_view.data->isRoot() && (e.attr & FILE_ATTRIBUTE_HIDDEN));
        it.state = INDEXTOOVERLAYMASK(g_view.overlays[idx]) | (dim ? LVIS_CUT : 0) |
                   (it.iItem == g_dropRow ? LVIS_DROPHILITED : 0);
        it.stateMask = LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED;
    }
    if (!(it.mask & LVIF_TEXT) || !it.pszText || it.cchTextMax <= 0) return;
    wchar_t* out = it.pszText;
    const int cch = it.cchTextMax;
    out[0] = 0;
    switch (it.iSubItem) {
        case COL_NAME:
            if (const DriveInfo* di = DriveOfEntry(idx))
                wcsncpy_s(out, cch, DriveDisplayName(NameOf(idx), e.extra, *di).c_str(), _TRUNCATE);
            else {
                const uint32_t hide = e.isDir() || g_view.data->isRoot()
                                          ? 0
                                          : HiddenExtLen(NameOf(idx), e.nameLen, e.extOff, g_prefs.showExtensions);
                wcsncpy_s(out, cch, NameOf(idx), std::min<size_t>(e.nameLen - hide, (size_t)cch - 1));
            }
            break;
        case COL_SIZE:
            if (const DriveInfo* di = DriveOfEntry(idx)) {
                if (di->total)
                    wcsncpy_s(out, cch, (BytesText(di->free) + L" libres sur " + BytesText(di->total)).c_str(), _TRUNCATE);
            } else if (!e.isDir()) StrFormatKBSizeW((LONGLONG)e.size, out, (UINT)cch);
            break;
        case COL_TYPE:
            if (g_view.data->isRoot()) wcsncpy_s(out, cch, DriveTypeText(e.extra), _TRUNCATE);
            else if (e.isDir()) wcsncpy_s(out, cch, L"Dossier de fichiers", _TRUNCATE);
            else if (e.extOff + 1 < e.nameLen) {
                swprintf_s(out, cch, L"Fichier %s", NameOf(idx) + e.extOff + 1);
                CharUpperBuffW(out + 8, (DWORD)wcslen(out + 8));
            } else wcsncpy_s(out, cch, L"Fichier", _TRUNCATE);
            break;
        case COL_DATE:
            if (!g_view.data->isRoot()) FormatDate(e.mtime, out, cch);  // Ce PC : jauge dessinée à la place
            break;
    }
}

// Ce PC : en-têtes adaptés aux lecteurs, colonne d'espace libre assez large.
void UpdateColumnsForView() {
    static bool rootCols = false;
    static int savedSizeW = 0;
    const bool root = HasView() && g_view.data->isRoot();
    if (root == rootCols) return;
    rootCols = root;
    const wchar_t* names[COL_COUNT] = {L"Nom", L"Taille", L"Type", L"Modifié le"};
    if (root) {
        names[COL_SIZE] = L"Espace libre";
        names[COL_DATE] = L"Utilisation";
    }
    for (int i = 0; i < COL_COUNT; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT;
        c.pszText = (LPWSTR)names[i];
        ListView_SetColumn(g_list, i, &c);
    }
    const int w = ListView_GetColumnWidth(g_list, COL_SIZE);
    if (root && w < S(200)) {
        savedSizeW = w;
        ListView_SetColumnWidth(g_list, COL_SIZE, S(200));
    } else if (!root && savedSizeW) {
        ListView_SetColumnWidth(g_list, COL_SIZE, savedSizeW);
        savedSizeW = 0;
    }
}

// Ce PC : jauge d'occupation de chaque lecteur (rouge au-delà de 90 %, comme l'Explorateur).
LRESULT OnListCustomDraw(NMLVCUSTOMDRAW* cd) {
    if (!HasView() || !g_view.data->isRoot()) return CDRF_DODEFAULT;
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT: {
            const int row = (int)cd->nmcd.dwItemSpec;
            const DriveInfo* di = DriveOfEntry(EntryAt(row));
            RECT r;
            if (!di || !di->total || !ListView_GetSubItemRect(g_list, row, COL_DATE, LVIR_BOUNDS, &r)) break;
            const int h = std::max(S(6), (int)(r.bottom - r.top) / 2);
            RECT bar{r.left + S(6), r.top + (r.bottom - r.top - h) / 2, r.right - S(6), 0};
            bar.bottom = bar.top + h;
            if (bar.right - bar.left < S(20)) break;
            const double used = 1.0 - (double)di->free / (double)di->total;
            RECT fill = bar;
            fill.right = bar.left + (LONG)((bar.right - bar.left) * used + 0.5);
            HBRUSH back = CreateSolidBrush(RGB(230, 230, 230));
            HBRUSH front = CreateSolidBrush(used >= 0.9 ? RGB(218, 38, 38) : RGB(38, 160, 218));
            HBRUSH edge = CreateSolidBrush(RGB(188, 188, 188));
            FillRect(cd->nmcd.hdc, &bar, back);
            FillRect(cd->nmcd.hdc, &fill, front);
            FrameRect(cd->nmcd.hdc, &bar, edge);
            DeleteObject(back);
            DeleteObject(front);
            DeleteObject(edge);
            break;
        }
    }
    return CDRF_DODEFAULT;
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

// query : recherche à lancer dans `path`. Par défaut, un rafraîchissement
// relance la recherche affichée ; toute autre navigation en sort.
void Navigate(const std::wstring& path, Nav nav, std::wstring focus = {}, std::vector<std::wstring> selected = {},
              std::wstring fallbackOpen = {}, bool fallbackAnyError = false, const std::wstring* query = nullptr) {
    const CancelToken ct = Jobs_NewGeneration();
    const std::wstring q = query ? *query : (nav == Nav::Refresh ? g_query : std::wstring());
    g_pending = Pending{true,           path, nav, std::move(focus), std::move(selected), std::move(fallbackOpen),
                        fallbackAnyError, q};
    if (g_hwnd) SetTimer(g_hwnd, TIMER_LOADING, kLoadingDelayMs, nullptr);
    const int col = g_sortCol;
    const bool asc = g_sortAsc;
    const bool climb = nav == Nav::Refresh && q.empty();  // dossier affiché disparu : on remonte, comme l'Explorateur
    Jobs_Spawn([ct, path, col, asc, climb, q, prefs = g_prefs] {
        auto d = std::make_shared<DirData>();
        d->path = path;
        if (!q.empty()) {
            // Résultats au fil de l'eau : chaque copie partielle s'affiche comme un rafraîchissement.
            SearchTree(*d, q, prefs, ct, [&](std::shared_ptr<DirData> s) {
                auto* r = new Result{ct.gen, JobKind::Navigate};
                r->order = SortOrder(*s, col, asc, prefs);
                r->data = std::move(s);
                Post(r);
            });
        } else if (climb) EnumerateNearest(*d, ct);
        else Enumerate(*d, ct);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Navigate};
        if (!d->error) {
            const double t = NowMs();
            r->order = SortOrder(*d, col, asc, prefs);
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
    ListView_SetItemCountEx(g_list, RowCount(), LVSICF_NOSCROLL);  // change si des éléments sont masqués / affichés
    ApplySelection(sel, focus, true);
    InvalidateRect(g_list, nullptr, FALSE);
    ShowSummary();
}

void SortView();

void Resort(int col) {
    if (col == g_sortCol) g_sortAsc = !g_sortAsc;
    else { g_sortCol = col; g_sortAsc = (col == COL_NAME || col == COL_TYPE); }
    UpdateSortArrow();
    SortView();
}

// Retrie (et refiltre) la vue avec le tri et les préférences actuels.
void SortView() {
    if (g_pending.active) {  // une navigation est en vol : on la relance avec le nouveau tri
        Pending p = g_pending;
        Navigate(p.path, p.nav, p.focus, p.selected, p.fallbackOpen, p.fallbackAnyError, &p.query);
        return;
    }
    if (!HasView()) return;
    auto data = g_view.data;
    if (data->entries.size() <= kSyncSortMax) {
        const double t = NowMs();
        auto order = SortOrder(*data, g_sortCol, g_sortAsc, g_prefs);
        ApplyOrder(std::move(order), NowMs() - t);
        return;
    }
    const CancelToken ct = Jobs_NewGeneration();
    const int c = g_sortCol;
    const bool asc = g_sortAsc;
    SetStatus(L"Tri…");
    Jobs_Spawn([ct, data, c, asc, prefs = g_prefs] {
        const double t = NowMs();
        auto order = SortOrder(*data, c, asc, prefs);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Sort, data, std::move(order), NowMs() - t};
        Post(r);
    });
}

// « 3 éléments sélectionnés  4,52 Mo ». Une seule fois par rafale de
// changements (Ctrl+A sur 100 000 éléments : un seul parcours).
void ShowSelectionSummary() {
    g_selPosted = false;
    if (!HasView()) return SetStatusPart(SB_SEL, L"");
    size_t count = 0;
    bool anyDir = false;
    uint64_t bytes = 0;
    for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0;) {
        const int e = EntryAt(r);
        if (e < 0) continue;
        ++count;
        if (E(e).isDir()) anyDir = true;
        else bytes += E(e).size;
    }
    SetStatusPart(SB_SEL, SelectionText(count, anyDir, bytes, !g_view.data->isRoot()));
}

void Refresh();

// Lance (ou quitte) la recherche tapée dans la zone de recherche.
void RunSearch() {
    KillTimer(g_hwnd, TIMER_SEARCH);
    if (!HasView()) return;
    const int n = GetWindowTextLengthW(g_searchBox);
    std::wstring q(n + 1, L'\0');
    GetWindowTextW(g_searchBox, q.data(), n + 1);
    q.resize(n);
    while (!q.empty() && iswspace(q.back())) q.pop_back();
    while (!q.empty() && iswspace(q.front())) q.erase(0, 1);
    const std::wstring current = g_pending.active ? g_pending.query : g_query;
    if (q == current) return;
    if (!q.empty() && g_path.empty()) {
        SetStatus(L"Ouvrez un dossier ou un lecteur pour y rechercher.");
        return;
    }
    Navigate(g_path, Nav::Refresh, {}, {}, {}, false, &q);  // q vide : retour au contenu du dossier
}

void FocusSearch() {
    SetFocus(g_searchBox);
    Edit_SetSel(g_searchBox, 0, -1);
}

// Préférences d'affichage changées (ici, dans l'Explorateur ou les options des dossiers).
void ApplyViewPrefs(const ViewPrefs& p) {
    if (p == g_prefs) return;
    const bool refilter = p.showHidden != g_prefs.showHidden || p.showSuperHidden != g_prefs.showSuperHidden;
    g_prefs = p;
    NavPane_SetViewPrefs(p);
    if (!HasView()) return;
    if (refilter && !g_query.empty()) Refresh();  // recherche : les dossiers cachés sont parcourus ou non
    else if (refilter) SortView();
    else InvalidateRect(g_list, nullptr, FALSE);  // extensions : seul l'affichage des noms change
}

// Comme Afficher ▸ Afficher de l'Explorateur : le réglage de Windows change aussi.
void ToggleHidden() {
    ViewPrefs p = g_prefs;
    p.showHidden = !p.showHidden;
    WriteViewPrefs(p);
    ApplyViewPrefs(p);
}

void ToggleExtensions() {
    ViewPrefs p = g_prefs;
    p.showExtensions = !p.showExtensions;
    WriteViewPrefs(p);
    ApplyViewPrefs(p);
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
    if (!p.active && !d.query.empty() && HasView()) {
        // Suite d'une recherche déjà affichée : même dossier, sélection gardée.
        p.nav = Nav::Refresh;
        for (int e : SelectedEntries()) p.selected.emplace_back(NameOf(e));
        if (const int f = FocusedEntry(); f >= 0) p.focus = NameOf(f);
    }
    if (!d.query.empty() && !d.error && d.entries.size() <= kSyncSortMax)  // tri changé pendant la recherche
        r->order = SortOrder(d, g_sortCol, g_sortAsc, g_prefs);

    if (d.error) {
        SetWindowTextW(g_addr, g_path.c_str());
        Crumbs_SetPath(g_path);
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
    g_query = d.query;
    if (g_query.empty() && GetWindowTextLengthW(g_searchBox) && GetFocus() != g_searchBox) {
        g_searchSync = true;  // sorti de la recherche (autre dossier) : zone vidée
        SetWindowTextW(g_searchBox, L"");
        g_searchSync = false;
    }
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
    Crumbs_SetPath(g_path);
    SetWindowTextW(g_hwnd, (L"FastExplorer — " + (g_path.empty() ? std::wstring(L"Ce PC") : g_path)).c_str());
    ListView_SetItemCountEx(g_list, (int)g_view.order.size(), refresh ? LVSICF_NOSCROLL : 0);
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
    UpdateColumnsForView();
    NavPane_ShowPath(g_path);
    NavPane_FolderListed(d);  // sous-dossiers affichés dans le volet : à jour sans relecture
    ShellMenu_Discard();  // contenu du dossier relu : le menu préparé peut ne plus être exact
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
        if (e >= 0 && g_view.rowOf[e] >= 0) {
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

// Barre d'adresse : fil d'Ariane, ou zone de saisie le temps de taper un chemin.
void BeginAddressEdit() {
    if (!IsWindowVisible(g_addr)) {
        SetWindowTextW(g_addr, g_path.empty() ? L"Ce PC" : g_path.c_str());
        ShowWindow(g_addr, SW_SHOW);
        ShowWindow(g_crumbs, SW_HIDE);
    }
    SetFocus(g_addr);
    Edit_SetSel(g_addr, 0, -1);
}

void EndAddressEdit() {
    if (!IsWindowVisible(g_addr)) return;
    ShowWindow(g_crumbs, SW_SHOW);
    ShowWindow(g_addr, SW_HIDE);
    SetWindowTextW(g_addr, g_path.c_str());
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

void StartOp(FileOp op) {
    FileOps_Start(g_hwnd, g_hwnd, WM_APP_FILEOP, std::move(op));
    SetTimer(g_hwnd, TIMER_PROGRESS, 200, nullptr);
}

void TaskbarProgress(TBPFLAG state, uint64_t done = 0, uint64_t total = 0) {
    if (!g_taskbar) {
        if (state == TBPF_NOPROGRESS ||
            FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar))))
            return;
        if (FAILED(g_taskbar->HrInit())) {
            g_taskbar->Release();
            g_taskbar = nullptr;
            return;
        }
    }
    g_taskbar->SetProgressState(g_hwnd, state);
    if (state == TBPF_NORMAL) g_taskbar->SetProgressValue(g_hwnd, done, total);
}

std::wstring SizeText(uint64_t bytes) {
    wchar_t b[64];
    StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, _countof(b));
    return b;
}

// Copies de notre moteur : progression dans la barre d'état et sur le bouton de
// la barre des tâches (rien pour une opération de moins d'une demi-seconde).
void OnProgressTimer() {
    FileOpsProgress p;
    if (!FileOps_Progress(p)) {
        if (FileOps_Running() == 0) KillTimer(g_hwnd, TIMER_PROGRESS);
        if (g_progressShown) {
            g_progressShown = false;
            TaskbarProgress(TBPF_NOPROGRESS);
            ShowSummary();
        }
        return;
    }
    if (GetTickCount64() - p.start < 500) return;
    g_progressShown = true;
    const bool bytes = p.bytesTotal > 0;
    const uint64_t done = bytes ? p.bytesDone : p.filesDone, total = bytes ? p.bytesTotal : p.filesTotal;
    std::wstring s = p.move ? L"Déplacement : " : L"Copie : ";
    s += std::to_wstring(p.filesDone) + L" / " + std::to_wstring(p.filesTotal) + L" fichiers";
    if (bytes) s += L"   •   " + SizeText(p.bytesDone) + L" / " + SizeText(p.bytesTotal);
    if (total) s += L"   •   " + std::to_wstring(done * 100 / total) + L" %";
    s += L"   •   Échap pour annuler";
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
    TaskbarProgress(TBPF_NORMAL, done, total ? total : 1);
}

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
    ShellMenu_Discard();  // « Coller » du menu préparé n'est plus à jour
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
    for (int e : FindEntriesByNames(names))
        if (g_view.rowOf[e] >= 0) rows.push_back(g_view.rowOf[e]);
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
    const Entry& e = E(idx);
    const uint32_t hide = e.isDir() ? 0 : HiddenExtLen(NameOf(idx), e.nameLen, e.extOff, g_prefs.showExtensions);
    g_editExt.assign(NameOf(idx) + e.nameLen - hide, hide);  // la saisie montre le nom affiché, sans elle
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
    if (idx < 0) return;
    // Résultat de recherche (« sous-dossier\nom ») : on ne renomme que le nom.
    const size_t slash = g_editName.rfind(L'\\');
    if (slash != std::wstring::npos) {
        std::wstring leaf = g_editName.substr(slash + 1);
        leaf.resize(leaf.size() - std::min(leaf.size(), g_editExt.size()));
        SetWindowTextW(ed, leaf.c_str());
        const int dot = E(idx).isDir() || !g_editExt.empty() ? -1 : (int)(PathFindExtensionW(leaf.c_str()) - leaf.c_str());
        Edit_SetSel(ed, 0, dot > 0 && dot < (int)leaf.size() ? dot : -1);
        return;
    }
    if (E(idx).isDir() || !g_editExt.empty()) return;  // extension masquée : tout est sélectionné
    const wchar_t* name = NameOf(idx);
    const int dot = (int)(PathFindExtensionW(name) - name);
    if (dot > 0 && name[dot]) Edit_SetSel(ed, 0, dot);
}

void OnReedit() {
    const int idx = FindEntryByName(g_reedit.name);
    if (idx < 0 || !HasView() || g_view.rowOf[idx] < 0) { g_reedit = Reedit{}; return; }
    SetFocus(g_list);
    ListView_EditLabel(g_list, g_view.rowOf[idx]);
}

void OnEndLabelEdit(const NMLVDISPINFOW* di) {
    g_editing = false;
    const std::wstring old = g_editName;
    g_editName.clear();
    if (!di->item.pszText || old.empty()) return;  // Échap
    const std::wstring typed = di->item.pszText;
    std::wstring name = CleanFileName(typed);
    if (!name.empty() && !g_editExt.empty()) name += g_editExt;  // extension masquée : conservée
    const size_t slash = old.rfind(L'\\');  // résultat de recherche : seul le nom change
    const std::wstring oldLeaf = slash == std::wstring::npos ? old : old.substr(slash + 1);
    if (name == oldLeaf) return;
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
    h.transfer = [](const std::wstring& dest, std::vector<std::wstring> paths, bool move) {
        FileOp op;
        op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
        op.folder = dest;
        op.paths = std::move(paths);
        StartOp(std::move(op));
    };
    DragDrop_RegisterTarget(g_list, std::move(h));
}

// « Épingler dans FastExplorer » : Accès rapide propre à l'application, qui
// marche même quand celui de Windows ne répond pas. Un dossier épinglé dans
// Windows se désépingle par le menu du Shell (« Désépingler de l'Accès rapide »).
std::vector<MenuExtra> PinExtras(const std::wstring& path, bool withOpen) {
    std::vector<MenuExtra> x;
    if (withOpen) x.push_back({L"Ouvrir", [path] { Navigate(path, Nav::Normal); }, true});
    const NavPin pin = NavPane_PinOf(path);
    if (pin == NavPin::Windows) {
        x.push_back({L"Désépingler de l'Accès rapide", [path] { NavPane_UnpinWindows(path); }});
        return x;
    }
    const bool add = pin == NavPin::None;
    x.push_back({add ? L"Épingler dans FastExplorer" : L"Désépingler de FastExplorer", [path, add] {
                     if (!NavPane_Pin(path, add)) SetStatus(L"Impossible d'enregistrer les dossiers épinglés.");
                 }});
    return x;
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
    std::vector<MenuExtra> extras;
    if (sel.size() == 1 && E(sel[0]).isDir()) {
        const std::wstring dir = g_path.empty() ? names[0] : Join(g_path, names[0].c_str());  // Ce PC : un lecteur
        extras = PinExtras(dir, true);
    } else if (sel.empty() && !g_path.empty()) {  // fond du dossier : options d'affichage
        extras.push_back({L"Afficher les éléments masqués", ToggleHidden, false, g_prefs.showHidden});
        extras.push_back({L"Afficher les extensions de noms de fichiers", ToggleExtensions, false, g_prefs.showExtensions});
    }
    if (!g_query.empty() && names.size() == 1) {  // résultat de recherche : son dossier, élément sélectionné
        std::wstring parent, leaf;
        if (ParentOf(Join(g_path, names[0].c_str()), parent, leaf))
            extras.insert(extras.begin(), {L"Ouvrir l'emplacement du fichier", [parent, leaf] {
                              Navigate(parent, Nav::Normal, leaf, {leaf});
                              SetFocus(g_list);
                          }});
    }
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
        if (single >= 0 && is(L"rename") && g_view.rowOf[single] >= 0) {
            ListView_EditLabel(g_list, g_view.rowOf[single]);
            return true;
        }
        if (is(L"NewFolder")) { NewFolder(); return true; }
        if (background) g_expectNew = {g_view.data, GetTickCount64() + 10000};
        return false;
    }, extras);
    if (!ok && !g_query.empty() && names.size() > 1)
        SetStatus(L"Menu contextuel : choisissez des résultats d'un même dossier.");
    else if (!ok) SetStatus(L"Menu contextuel indisponible pour cet emplacement.");
    SetTimer(g_hwnd, TIMER_PINNED, 800, nullptr);  // « Épingler à Accès rapide » a pu être choisi
}

// Menu contextuel d'un dossier du volet de navigation.
void ShowNavMenu(const std::wstring& path, POINT pt) {
    std::wstring parent, leaf;
    std::vector<std::wstring> names;
    if (!path.empty()) {
        if (!ParentOf(path, parent, leaf) || leaf.empty()) {  // lecteur : élément de Ce PC
            parent.clear();
            leaf = path;
        }
        names = {leaf};
    }
    const bool ok = ShellMenu_Show(g_hwnd, parent, names, pt, GetKeyState(VK_SHIFT) < 0,
                                   [path, parent, leaf](const std::wstring& verb) {
        auto is = [&](const wchar_t* v) { return _wcsicmp(verb.c_str(), v) == 0; };
        if (is(L"open") || is(L"explore")) { Navigate(path, Nav::Normal); return true; }
        if (leaf.empty() || parent.empty()) return false;  // Ce PC, lecteurs : le Shell
        if (is(L"rename")) {  // dans la liste du dossier parent, comme un renommage ordinaire
            g_editAfter = leaf;
            Navigate(parent, Nav::Normal, leaf, {leaf});
            return true;
        }
        if (is(L"copy") || is(L"cut")) {
            if (!Clipboard_Put(g_hwnd, parent, {leaf}, is(L"cut"))) SetStatus(L"Impossible d'utiliser le presse-papiers.");
            return true;
        }
        if (is(L"paste")) { Paste(path); return true; }
        return false;
    }, path.empty() ? std::vector<MenuExtra>{} : PinExtras(path, true));
    if (!ok) SetStatus(L"Menu contextuel indisponible pour cet emplacement.");
    SetTimer(g_hwnd, TIMER_PINNED, 800, nullptr);
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
    NavPane_RefreshDrives();
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
        ShellMenu_Discard(true);
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
    // Navigation, renommage ou recherche en cours : on réessaie plus tard.
    if (g_pending.active || g_editing || (HasView() && !g_view.data->searchDone)) {
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
    for (HWND h : {g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp, g_nav, g_crumbs, g_searchBox})
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
    if (pick) NavPane_SetImageList(pick);
}

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    {  // zones de la barre d'état : sélection et temps de lecture à droite
        const int perfW = S(340), selW = S(260);
        int parts[3] = {std::max(S(120), (int)rc.right - perfW - selW), std::max(S(240), (int)rc.right - perfW), -1};
        SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
    }
    RECT rs;
    GetWindowRect(g_status, &rs);
    const int statusH = rs.bottom - rs.top;
    const int m = S(6), bw = S(34);
    const int editH = g_fontH + S(10);  // texte centré verticalement
    const int h = std::max(S(30), editH);
    // Un seul lot de déplacements : un repaint au lieu d'un par contrôle.
    HDWP dwp = BeginDeferWindowPos(9);
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
    const int searchW = std::min(S(260), std::max(0, (int)rc.right - x - m) / 3);
    const int addrW = std::max(0, (int)rc.right - x - m - searchW - S(6));
    place(g_addr, x, m + (h - editH) / 2, addrW, editH);
    place(g_crumbs, x, m + (h - editH) / 2, addrW, editH);  // même place
    place(g_searchBox, (int)rc.right - m - searchW, m + (h - editH) / 2, searchW, editH);
    const int top = m + h + m;
    const int bodyH = std::max(0, (int)rc.bottom - top - statusH);
    // Volet de navigation, séparateur (déplaçable à la souris), liste.
    const int split = S(5);
    const int navW = std::clamp(S(g_navW96), S(120), std::max(S(120), (int)rc.right / 2));
    place(g_nav, 0, top, navW, bodyH);
    g_split = {navW, top, navW + split, top + bodyH};
    place(g_list, navW + split, top, std::max(0, (int)rc.right - navW - split), bodyH);
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

    // Zone de saisie cachée : le fil d'Ariane occupe sa place, sauf pendant la saisie.
    g_addr = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                             (HMENU)ID_ADDR, g_inst, nullptr);
    CrumbHost crumbs;
    crumbs.navigate = [](const std::wstring& path) {
        Navigate(path, Nav::Normal);
        SetFocus(g_list);
    };
    crumbs.edit = BeginAddressEdit;
    crumbs.driveText = NavPane_DriveText;
    g_crumbs = Crumbs_Create(hwnd, ID_CRUMBS, std::move(crumbs));
    g_searchBox = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                                  0, 0, hwnd, (HMENU)ID_SEARCH, g_inst, nullptr);
    Edit_SetCueBannerTextFocused(g_searchBox, L"Rechercher (Ctrl+F)", TRUE);

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

    NavHost nav;
    nav.navigate = [](const std::wstring& path) { Navigate(path, Nav::Normal); };
    nav.contextMenu = ShowNavMenu;
    nav.status = [](const std::wstring& text) { SetStatus(text); };
    g_nav = NavPane_Create(hwnd, ID_NAV, WM_APP_NAV, std::move(nav));

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
            g_prefs = ReadViewPrefs();
            CreateChildren(hwnd);
            NavPane_SetViewPrefs(g_prefs);
            Icons_Start(hwnd, WM_APP_ICON);
            AddClipboardFormatListener(hwnd);
            RegisterDrop();
            AttachTarget(hwnd);
            return 0;
        case WM_CONTEXTMENU:
            if ((HWND)wp == g_list) { ShowContextMenu(lp); return 0; }
            if ((HWND)wp == g_nav) { NavPane_ContextMenu(lp); return 0; }
            break;
        case WM_APP_NAV:
            NavPane_OnResult(lp);
            Crumbs_Update();  // noms des lecteurs peut-être connus maintenant
            return 0;
        case WM_SETCURSOR:
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (PtInRect(&g_split, p)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN:
            if (POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; PtInRect(&g_split, p)) {
                g_splitDrag = true;
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_splitDrag) {
                g_navW96 = MulDiv(std::max(0, (int)GET_X_LPARAM(lp)), 96, (int)g_dpi);
                Layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_splitDrag) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED: g_splitDrag = false; return 0;
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
            else {
                NavPane_RefreshPinned();             // épinglé depuis l'Explorateur entre-temps ?
                ApplyViewPrefs(ReadViewPrefs());  // options des dossiers changées entre-temps ?
            }
            break;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ShellState") == 0) ApplyViewPrefs(ReadViewPrefs());
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
            if (wp == TIMER_PROGRESS) { OnProgressTimer(); return 0; }
            if (wp == TIMER_PINNED) { KillTimer(hwnd, wp); NavPane_RefreshPinned(true); return 0; }
            if (wp == TIMER_SEARCH) { RunSearch(); return 0; }
            if (wp == TIMER_HOVER) {
                KillTimer(hwnd, wp);
                POINT c;
                if (!g_editing && GetCursorPos(&c) && WindowFromPoint(c) == g_list && ScreenToClient(g_list, &c))
                    PrepareContextMenu(MAKELPARAM(c.x, c.y), GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
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
                case ID_ADDR:
                    if (HIWORD(wp) == EN_KILLFOCUS) EndAddressEdit();
                    return 0;
                case ID_SEARCH:  // recherche lancée après une courte pause dans la frappe
                    if (HIWORD(wp) == EN_CHANGE && !g_searchSync) SetTimer(hwnd, TIMER_SEARCH, 300, nullptr);
                    return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (LRESULT r = 0; NavPane_Notify(hdr, &r)) return r;
            if (hdr->hwndFrom != g_list) break;
            switch (hdr->code) {
                case LVN_GETDISPINFOW: FillDispInfo(((NMLVDISPINFOW*)lp)->item); return 0;
                case LVN_ITEMCHANGED:
                    if ((((NMLISTVIEW*)lp)->uChanged & LVIF_STATE) &&
                        ((((NMLISTVIEW*)lp)->uNewState ^ ((NMLISTVIEW*)lp)->uOldState) & LVIS_SELECTED))
                        ScheduleSelectionSummary();
                    return 0;
                case LVN_ODSTATECHANGED: ScheduleSelectionSummary(); return 0;  // plage (Maj+clic, Ctrl+A)
                case NM_CUSTOMDRAW: return OnListCustomDraw((NMLVCUSTOMDRAW*)lp);
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
        case WM_APP_SELSUM: ShowSelectionSummary(); return 0;
        case WM_DESTROY:
            g_target.store(nullptr);
            Icons_Stop();
            Watch_Stop();
            ShellMenu_Discard(true);
            if (g_taskbar) g_taskbar->Release();
            g_taskbar = nullptr;
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
    // Survol : le menu contextuel se construit déjà sur son thread (≈100 ms de
    // gestionnaires du Shell), sans rien coûter à l'interface. Au clic droit,
    // il ne reste qu'à l'afficher.
    // (Minuteur à nous : la liste redéfinit elle-même TrackMouseEvent.)
    if (m.hwnd == g_list && m.message == WM_MOUSEMOVE) {
        if (!(m.wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) SetTimer(g_hwnd, TIMER_HOVER, kMenuHoverMs, nullptr);
        return false;
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
            case 'D': BeginAddressEdit(); return true;
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
            BeginAddressEdit();
            return true;
        case VK_F4: BeginAddressEdit(); return true;
        case VK_F3: FocusSearch(); return true;
        case 'F':
        case 'E':
            if (!ctrl) return false;
            FocusSearch();
            return true;
        case VK_TAB:  // barre d'adresse → recherche → liste
            if (focus == g_addr) FocusSearch();
            else if (focus == g_searchBox) SetFocus(g_list);
            else BeginAddressEdit();
            return true;
        case VK_RETURN:
            if (focus == g_addr) { NavigateAddress(); return true; }
            if (focus == g_searchBox) { RunSearch(); SetFocus(g_list); return true; }
            if (focus == g_list) { Activate(ListView_GetNextItem(g_list, -1, LVNI_FOCUSED)); return true; }
            return false;
        case VK_ESCAPE: {
            if (focus == g_addr) { SetWindowTextW(g_addr, g_path.c_str()); SetFocus(g_list); return true; }
            if (focus == g_searchBox) {  // Échap : on quitte la recherche
                SetWindowTextW(g_searchBox, L"");
                RunSearch();
                SetFocus(g_list);
                return true;
            }
            FileOpsProgress p;
            if (FileOps_Progress(p)) {  // copie en cours : on l'arrête (ce qui est copié reste)
                FileOps_Cancel();
                return true;
            }
            return false;
        }
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
        case 'H': if (!ctrl) return false; ToggleHidden(); return true;
        case 'N': if (!ctrl || !shift) return false; NewFolder(); return true;
        case 'D': if (!ctrl) return false; DeleteNames(NamesOf(SelectedEntries()), false); return true;
    }
    return false;
}

}  // namespace

bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start) {
    g_inst = inst;
    g_prefs = ReadViewPrefs();  // avant la première lecture : elle filtre déjà les éléments cachés
    Navigate(start, Nav::Normal);  // la lecture démarre pendant qu'on construit la fenêtre
    WarmUpShell();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
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
