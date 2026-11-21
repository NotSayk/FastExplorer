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
#include <map>
#include <memory>
#include <optional>
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
constexpr UINT WM_APP_TABEDIT = WM_APP + 11;    // Tab pendant un renommage : renommer l'élément suivant
constexpr UINT WM_APP_CMDFAIL = WM_APP + 12;    // commande tapée dans la barre d'adresse impossible (lParam = std::wstring*)
constexpr UINT WM_APP_TESTDPI = WM_APP + 13;    // tests : comme WM_DPICHANGED (wParam = nouvelle échelle), que Windows
                                                // ne laisse pas envoyer d'un autre processus
constexpr UINT_PTR TIMER_LOADING = 1;
constexpr UINT_PTR TIMER_WATCH = 2;
constexpr UINT_PTR TIMER_AFTEROP = 3;
constexpr UINT_PTR TIMER_HOVER = 6;   // souris immobile sur la liste : préparer le menu contextuel
constexpr UINT kMenuHoverMs = 100;
constexpr UINT_PTR TIMER_PROGRESS = 7;  // progression des copies de notre moteur
constexpr UINT_PTR TIMER_PINNED = 8;    // après un menu contextuel : relire l'Accès rapide (épinglage)
constexpr UINT_PTR TIMER_DRIVES = 4;  // Ce PC : regroupe les annonces d'arrivée / retrait de lecteurs
constexpr UINT_PTR TIMER_EJECT = 5;   // éjection demandée : si elle échoue, on reprend la surveillance  // resélection après une opération, différée pendant un renommage
constexpr UINT kLoadingDelayMs = 150;   // en dessous, aucun « Chargement… » (évite le clignotement)
constexpr size_t kSyncSortMax = 20000;  // au-delà, le re-tri part en arrière-plan
// Rafraîchissement après un changement sur disque : regroupe les rafales
// d'événements (copie de milliers de fichiers) et s'espace sur les gros dossiers.
constexpr double kWatchMinDelayMs = 150, kWatchMaxDelayMs = 2000;

enum { ID_BACK = 100, ID_FWD, ID_UP, ID_ADDR, ID_LIST, ID_STATUS, ID_NAV, ID_CRUMBS, ID_SEARCH, ID_REFRESH, ID_TABS };
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
    size_t steps = 1;               // Précédent / Suivant : pas d'historique franchis (liste déroulante)
    std::wstring command;           // barre d'adresse : si ce n'est pas un dossier, lancer ceci (« cmd », « wt »…)
    int top = -1;                   // retour sur un onglet : première ligne visible à rétablir
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
HWND g_hwnd, g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp, g_btnRefresh, g_nav, g_crumbs, g_searchBox, g_tabBar;
std::wstring g_tabNext;         // renommage validé par Tab / Maj+Tab : élément à renommer ensuite
std::wstring g_tabEdit;         // … saisie à ouvrir (WM_APP_TABEDIT, ou après le renommage en cours)
std::wstring g_tabTyped;        // touches tapées avant que cette saisie ne s'ouvre : remises dedans
ULONGLONG g_tabSince = 0;       // ≠ 0 : saisie suivante attendue depuis ce moment
std::wstring g_query;           // recherche affichée ("" : contenu ordinaire du dossier)
bool g_searchSync = false;      // texte de la zone de recherche changé par le programme
std::wstring g_filter;          // filtre instantané sur les noms affichés ("" : aucun)
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

// Message bref (annulation faite…) : reste devant le résumé quelques secondes,
// même si la liste est relue entre-temps.
std::wstring g_flash;
ULONGLONG g_flashUntil = 0;

std::wstring WithNotes(const std::wstring& s) {
    std::wstring out = s;
    if (!g_flash.empty() && GetTickCount64() < g_flashUntil) out = g_flash + L"   •   " + out;
    if (!g_goneNote.empty()) out = g_goneNote + L"   •   " + out;
    return out;
}

void ShowSummary() {
    if (!HasView() || g_progressShown) return;
    const DirData& d = *g_view.data;
    wchar_t s[512];
    size_t dirs = 0;
    for (uint32_t e : g_view.order) dirs += d.entries[e].isDir();
    const size_t shown = g_view.order.size();
    size_t visible = d.entries.size();  // sans le filtre instantané (éléments cachés exclus)
    if (!g_filter.empty() && !d.isRoot()) {
        visible = 0;
        for (const Entry& e : d.entries) visible += IsShown(e.attr, g_prefs);
    }
    const size_t hidden = g_filter.empty() ? d.entries.size() - shown : 0;
    wchar_t masked[64] = L"";
    if (hidden) swprintf_s(masked, L", %zu masqué%s", hidden, hidden > 1 ? L"s" : L"");
    if (!g_filter.empty()) {
        swprintf_s(s, L"%zu %s sur %zu  (filtre « %.100s »)%s", shown, d.query.empty() ? L"élément(s)" : L"résultat(s)",
                   visible, g_filter.c_str(),
                   d.query.empty() ? L"   •   Entrée : chercher aussi dans les sous-dossiers" : L"");
    } else if (!d.query.empty()) {
        swprintf_s(s, L"%zu résultat%s pour « %.100s »%s", shown, shown > 1 ? L"s" : L"", d.query.c_str(),
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
    SetStatus(WithNotes(s));
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
    if (nav != Nav::Refresh && _wcsicmp(path.c_str(), g_path.c_str()) != 0) g_filter.clear();  // autre dossier
    g_pending = Pending{true,           path, nav, std::move(focus), std::move(selected), std::move(fallbackOpen),
                        fallbackAnyError, q};
    if (g_hwnd) SetTimer(g_hwnd, TIMER_LOADING, kLoadingDelayMs, nullptr);
    const int col = g_sortCol;
    const bool asc = g_sortAsc;
    const bool climb = nav == Nav::Refresh && q.empty();  // dossier affiché disparu : on remonte, comme l'Explorateur
    Jobs_Spawn([ct, path, col, asc, climb, q, prefs = g_prefs, filter = g_filter] {
        auto d = std::make_shared<DirData>();
        d->path = path;
        if (!q.empty()) {
            // Résultats au fil de l'eau : chaque copie partielle s'affiche comme un rafraîchissement.
            SearchTree(*d, q, prefs, ct, [&](std::shared_ptr<DirData> s) {
                auto* r = new Result{ct.gen, JobKind::Navigate};
                r->order = SortOrder(*s, col, asc, prefs, filter);
                r->data = std::move(s);
                Post(r);
            });
        } else if (climb) EnumerateNearest(*d, ct);
        else Enumerate(*d, ct);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Navigate};
        if (!d->error) {
            const double t = NowMs();
            r->order = SortOrder(*d, col, asc, prefs, filter);
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
        auto order = SortOrder(*data, g_sortCol, g_sortAsc, g_prefs, g_filter);
        ApplyOrder(std::move(order), NowMs() - t);
        return;
    }
    const CancelToken ct = Jobs_NewGeneration();
    const int c = g_sortCol;
    const bool asc = g_sortAsc;
    SetStatus(L"Tri…");
    Jobs_Spawn([ct, data, c, asc, prefs = g_prefs, filter = g_filter] {
        const double t = NowMs();
        auto order = SortOrder(*data, c, asc, prefs, filter);
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
void RunCommand(const std::wstring& cmd, const std::wstring& dir);

std::wstring SearchText() {
    const int n = GetWindowTextLengthW(g_searchBox);
    std::wstring q(n + 1, L'\0');
    GetWindowTextW(g_searchBox, q.data(), n + 1);
    q.resize(n);
    while (!q.empty() && iswspace(q.back())) q.pop_back();
    while (!q.empty() && iswspace(q.front())) q.erase(0, 1);
    return q;
}

// Filtre instantané : à chaque frappe, la liste ne garde que les noms qui
// correspondent (dossier affiché, ou résultats d'une recherche), sans lire le disque.
void ApplyFilter() {
    if (!HasView()) return;
    std::wstring f = SearchText();
    if (f == (g_pending.active ? g_pending.query : g_query)) f.clear();  // texte de la recherche affichée
    if (f == g_filter) return;
    g_filter = std::move(f);
    SortView();
}

// Entrée dans la zone : recherche dans les sous-dossiers (texte vide : on en sort).
void RunSearch() {
    if (!HasView()) return;
    const std::wstring q = SearchText();
    const std::wstring current = g_pending.active ? g_pending.query : g_query;
    if (q == current) return;
    g_filter.clear();  // les résultats correspondent déjà au texte
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

void SyncTab();

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
    // Tri ou filtre changé pendant la recherche (filtre : toujours, il réduit le travail).
    if (!d.query.empty() && !d.error && (d.entries.size() <= kSyncSortMax || !g_filter.empty()))
        r->order = SortOrder(d, g_sortCol, g_sortAsc, g_prefs, g_filter);

    if (d.error) {
        SetWindowTextW(g_addr, g_path.c_str());
        Crumbs_SetPath(g_path);
        if (!p.fallbackOpen.empty() && (p.fallbackAnyError || d.error == ERROR_DIRECTORY)) {
            ShowSummary();  // ce n'était pas un dossier : on l'ouvre, sans message d'erreur
            OpenWithShell(p.fallbackOpen);
            return;
        }
        if (!p.command.empty() && d.error != ERROR_ACCESS_DENIED) {  // pas un dossier : une commande
            ShowSummary();
            RunCommand(p.command, g_path);
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
            case Nav::Forward: {
                // Plusieurs pas (liste déroulante) : les dossiers franchis passent de l'autre côté.
                auto& from = p.nav == Nav::Back ? g_back : g_fwd;
                auto& to = p.nav == Nav::Back ? g_fwd : g_back;
                to.push_back(g_path);
                for (size_t k = 1; k < p.steps && !from.empty(); ++k) {
                    to.push_back(std::move(from.back()));
                    from.pop_back();
                }
                if (!from.empty()) from.pop_back();
                break;
            }
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
    if (g_query.empty() && g_filter.empty() && GetWindowTextLengthW(g_searchBox) &&
        (GetFocus() != g_searchBox || p.nav != Nav::Refresh)) {
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
        RECT rc{};
        if (p.top > 0 && RowCount() > 0 && ListView_GetItemRect(g_list, 0, &rc, LVIR_BOUNDS))
            ListView_Scroll(g_list, 0, (std::min(p.top, RowCount() - 1) - ListView_GetTopIndex(g_list)) * (rc.bottom - rc.top));
    } else {
        const int f = FindEntryByName(p.focus);
        ApplySelection(f >= 0 ? std::vector<int>{f} : std::vector<int>{}, f, true);
    }

    if (freeze) SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateButtons();
    SyncTab();
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

// Recule (avance) de `steps` dossiers dans l'historique.
void GoHistory(bool back, size_t steps = 1) {
    const auto& h = back ? g_back : g_fwd;
    if (steps == 0 || steps > h.size()) return;
    Navigate(h[h.size() - steps], back ? Nav::Back : Nav::Forward);
    g_pending.steps = steps;
}
void GoBack() { GoHistory(true); }
void GoForward() { GoHistory(false); }

// Nom court d'un dossier de l'historique, comme l'Explorateur : « Documents », « Données (D:) ».
std::wstring HistoryText(const std::wstring& path) {
    if (path.empty()) return L"Ce PC";
    std::wstring parent, leaf;
    if (!ParentOf(path, parent, leaf) || leaf.empty()) return NavPane_DriveText(path);
    return leaf;
}

// Clic droit sur Précédent / Suivant : les derniers dossiers de ce côté, le plus proche en haut.
void ShowHistoryMenu(bool back) {
    const auto& h = back ? g_back : g_fwd;
    if (h.empty()) return;
    HMENU m = CreatePopupMenu();
    const size_t n = std::min<size_t>(h.size(), 15);
    for (size_t k = 1; k <= n; ++k) {
        std::wstring t = HistoryText(h[h.size() - k]);
        for (size_t i = t.find(L'&'); i != std::wstring::npos; i = t.find(L'&', i + 2)) t.insert(i, 1, L'&');
        AppendMenuW(m, MF_STRING, k, t.c_str());
    }
    RECT r;
    GetWindowRect(back ? g_btnBack : g_btnFwd, &r);
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom,
                                      g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) {
        GoHistory(back, cmd);
        SetFocus(g_list);
    }
}
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

// Lance une commande tapée dans la barre d'adresse, sur un thread (la
// recherche du programme dans le PATH et les App Paths touche le disque).
void RunCommand(const std::wstring& cmd, const std::wstring& dir) {
    std::wstring file, params;
    if (cmd[0] == L'"') {
        const size_t q = cmd.find(L'"', 1);
        file = cmd.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        if (q != std::wstring::npos && q + 1 < cmd.size()) params = cmd.substr(q + 1);
    } else {
        const size_t sp = cmd.find(L' ');
        file = cmd.substr(0, sp);
        if (sp != std::wstring::npos) params = cmd.substr(sp + 1);
    }
    while (!params.empty() && iswspace(params.front())) params.erase(0, 1);
    SetStatus(L"Lancement de « " + cmd + L" »…");
    std::thread([hwnd = g_hwnd, cmd, file, params, dir] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof sei};
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpFile = file.c_str();
        sei.lpParameters = params.empty() ? nullptr : params.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            const DWORD err = GetLastError();
            auto* msg = new std::wstring(L"Impossible de trouver « " + cmd + L" » : ce n'est ni un dossier ni une commande (" +
                                         ErrorText(err) + L")");
            if (!PostMessageW(hwnd, WM_APP_CMDFAIL, 0, (LPARAM)msg)) delete msg;
        } else {
            auto* msg = new std::wstring();  // réussi : on rétablit le résumé
            if (!PostMessageW(hwnd, WM_APP_CMDFAIL, 0, (LPARAM)msg)) delete msg;
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
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
    // « cmd », « wt », « notepad x.txt », « code . » : lancés dans le dossier
    // affiché si ce n'est pas un dossier, comme dans l'Explorateur.
    std::wstring cmd = t;
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    while (!cmd.empty() && iswspace(cmd.front())) cmd.erase(0, 1);
    const std::wstring first = cmd.substr(0, std::min(cmd.find(L' '), cmd.size()));
    if (!first.empty() && first.find_first_of(L"\\/:") == std::wstring::npos) g_pending.command = cmd;
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

// ───────────────────────────── Annuler / Rétablir ─────────────────────────────
//
// Notre propre historique (Ctrl+Z / Ctrl+Y), sur plusieurs niveaux : celui de
// l'Explorateur n'est pas accessible aux autres applications. Chaque opération
// réussie y laisse une étape en chemins complets ; l'annuler lance l'opération
// inverse (renommer en sens inverse, remettre à sa place, mettre les copies à la
// Corbeille, restaurer depuis la Corbeille). Une suppression définitive ne
// s'annule pas.

enum { TAG_NONE = 0, TAG_UNDO = 1, TAG_REDO = 2 };

struct UndoStep {
    FileOpKind kind = FileOpKind::Copy;
    std::vector<std::wstring> from, to;  // avant / après, chemins complets
    int64_t since = 0;                   // suppression : moment où elle a commencé
};
std::vector<UndoStep> g_undo, g_redo;
std::optional<UndoStep> g_undoing;  // annulation en cours
size_t g_undoPending = 0;           // opérations de cette annulation pas encore finies
bool g_undoOk = false;
constexpr size_t kUndoMax = 100;

std::wstring ParentPath(const std::wstring& p) {
    std::wstring parent, leaf;
    return ParentOf(p, parent, leaf) ? parent : std::wstring();
}
std::wstring LeafOf(const std::wstring& p) {
    std::wstring parent, leaf;
    return ParentOf(p, parent, leaf) ? leaf : p;
}

void Flash(const std::wstring& msg) {
    g_flash = msg;
    g_flashUntil = GetTickCount64() + 4000;
    SetStatus(WithNotes(L""));
    if (HasView()) ShowSummary();
}

std::wstring StepText(const UndoStep& s) {
    const size_t n = std::max(s.from.size(), s.to.size());
    auto count = [&](const wchar_t* what) {
        return n == 1 ? std::wstring(what) + L" de « " + LeafOf(s.to.empty() ? s.from[0] : s.to[0]) + L" »"
                      : std::wstring(what) + L" de " + std::to_wstring(n) + L" éléments";
    };
    switch (s.kind) {
        case FileOpKind::Rename: return L"renommage de « " + LeafOf(s.from[0]) + L" »";
        case FileOpKind::NewFolder: return L"création de « " + LeafOf(s.to[0]) + L" »";
        case FileOpKind::Copy: return count(L"copie");
        case FileOpKind::Move: return count(L"déplacement");
        case FileOpKind::Delete: return n == 1 ? L"suppression de « " + LeafOf(s.from[0]) + L" »"
                                               : L"suppression de " + std::to_wstring(n) + L" éléments";
        default: return L"opération";
    }
}

// Étape à mémoriser pour une opération terminée (false : rien à annuler).
bool StepFromResult(const FileOpResult& r, UndoStep& s) {
    s = UndoStep{r.kind};
    const bool ok = SUCCEEDED(r.hr) && !r.aborted;
    switch (r.kind) {
        case FileOpKind::Rename:
            if (!ok || r.sources.size() != 1 || r.newName.empty()) return false;
            s.from = {r.sources[0]};
            s.to = {Join(ParentPath(r.sources[0]), r.newName.c_str())};
            return _wcsicmp(s.from[0].c_str(), s.to[0].c_str()) != 0 || s.from[0] != s.to[0];
        case FileOpKind::NewFolder:
            if (r.created.empty()) return false;
            s.to = {Join(r.folder, r.created[0].c_str())};
            return true;
        case FileOpKind::Copy:
            if (r.created.empty()) return false;
            s.from = r.sources;
            for (const auto& c : r.created) s.to.push_back(Join(r.folder, c.c_str()));
            return true;
        case FileOpKind::Move:
            for (const auto& src : r.sources) {
                const std::wstring leaf = LeafOf(src);
                if (_wcsicmp(ParentPath(src).c_str(), r.folder.c_str()) == 0) continue;  // déjà là
                for (const auto& c : r.created)
                    if (_wcsicmp(c.c_str(), leaf.c_str()) == 0) {
                        s.from.push_back(src);
                        s.to.push_back(Join(r.folder, c.c_str()));
                        break;
                    }
            }
            return !s.from.empty();
        case FileOpKind::Delete:
            if (r.permanent || r.sources.empty() || (FAILED(r.hr) && !r.aborted)) return false;
            s.from = r.sources;
            s.since = r.started;
            return true;
        default: return false;
    }
}

// Opérations qui défont (undo) ou refont l'étape.
std::vector<FileOp> OpsFor(const UndoStep& s, bool undo) {
    std::vector<FileOp> ops;
    auto add = [&](FileOpKind k, std::wstring folder, std::vector<std::wstring> paths) {
        FileOp op;
        op.kind = k;
        op.folder = std::move(folder);
        op.paths = std::move(paths);
        ops.push_back(std::move(op));
        return &ops.back();
    };
    switch (s.kind) {
        case FileOpKind::Rename: {
            const std::wstring& src = undo ? s.to[0] : s.from[0];
            add(FileOpKind::Rename, ParentPath(src), {src})->newName = LeafOf(undo ? s.from[0] : s.to[0]);
            break;
        }
        case FileOpKind::NewFolder:
            if (undo) add(FileOpKind::Delete, ParentPath(s.to[0]), s.to);
            else add(FileOpKind::NewFolder, ParentPath(s.to[0]), {})->newName = LeafOf(s.to[0]);
            break;
        case FileOpKind::Copy:
            if (undo) add(FileOpKind::Delete, ParentPath(s.to[0]), s.to);
            else add(FileOpKind::Copy, ParentPath(s.to[0]), s.from);
            break;
        case FileOpKind::Move:
            if (undo) {  // chaque élément retourne dans son dossier d'origine
                std::map<std::wstring, std::vector<std::wstring>> back;
                for (size_t i = 0; i < s.from.size(); ++i) back[ParentPath(s.from[i])].push_back(s.to[i]);
                for (auto& [dir, paths] : back) add(FileOpKind::Move, dir, std::move(paths));
            } else {
                add(FileOpKind::Move, ParentPath(s.to[0]), s.from);
            }
            break;
        case FileOpKind::Delete:
            if (undo) add(FileOpKind::Restore, ParentPath(s.from[0]), s.from)->since = s.since;
            else add(FileOpKind::Delete, ParentPath(s.from[0]), s.from);
            break;
        default: break;
    }
    return ops;
}

void StartOp(FileOp op);

void Undo() {
    if (g_undoPending) { Flash(L"Annulation déjà en cours…"); return; }
    if (g_undo.empty()) { Flash(L"Rien à annuler."); return; }
    g_undoing = std::move(g_undo.back());
    g_undo.pop_back();
    auto ops = OpsFor(*g_undoing, true);
    g_undoPending = ops.size();
    g_undoOk = false;
    if (ops.empty()) { g_undoing.reset(); return; }
    Flash(L"Annulation : " + StepText(*g_undoing) + L"…");
    for (auto& op : ops) {
        op.tag = TAG_UNDO;
        StartOp(std::move(op));
    }
}

void Redo() {
    if (g_undoPending) { Flash(L"Annulation en cours…"); return; }
    if (g_redo.empty()) { Flash(L"Rien à rétablir."); return; }
    UndoStep s = std::move(g_redo.back());
    g_redo.pop_back();
    Flash(L"Rétablir : " + StepText(s) + L"…");
    for (auto& op : OpsFor(s, false)) {
        op.tag = TAG_REDO;
        StartOp(std::move(op));
    }
}

// Résultat d'une opération : mémorisé pour Ctrl+Z (ou fin d'une annulation).
void RecordUndo(const FileOpResult& r) {
    if (r.tag == TAG_UNDO) {
        if (!FileOps_IsFailure(r) && !r.aborted) g_undoOk = true;
        if (g_undoPending && --g_undoPending == 0) {
            if (g_undoOk && g_undoing) {
                Flash(L"Annulé : " + StepText(*g_undoing) + L"   (Ctrl+Y pour rétablir)");
                g_redo.push_back(std::move(*g_undoing));
            }
            g_undoing.reset();
        }
        return;
    }
    UndoStep s;
    if (!StepFromResult(r, s)) return;
    if (r.tag != TAG_REDO) g_redo.clear();
    else Flash(L"Rétabli : " + StepText(s));
    g_undo.push_back(std::move(s));
    if (g_undo.size() > kUndoMax) g_undo.erase(g_undo.begin());
}

void OnFileOp(FileOpResult* raw) {
    std::unique_ptr<FileOpResult> r(raw);
    RecordUndo(*r);
    if (FileOps_IsFailure(*r))
        SetStatus(L"Opération impossible : " + ErrorText((DWORD)r->hr) + (r->detail.empty() ? L"" : L" (" + r->detail + L")"));
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
            if (!g_tabEdit.empty()) {  // Tab : renommer le suivant, une fois la liste à jour (nouveau nom visible)
                const std::wstring t = std::move(g_tabEdit);
                g_tabEdit.clear();
                if (SUCCEEDED(r->hr) && !r->aborted) AfterOpSelect(r->folder, {t}, t, t);
                else g_tabSince = 0;
                break;
            }
            [[fallthrough]];
        case FileOpKind::Restore:
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

// « Copier en tant que chemin d'accès » (Ctrl+Maj+C) : chemins complets entre
// guillemets, un par ligne, comme l'Explorateur. Rien sélectionné : le dossier.
void CopyPaths(const std::vector<std::wstring>& names) {
    if (!HasView()) return;
    std::vector<std::wstring> full;
    if (names.empty()) {
        if (g_path.empty()) return;
        full.push_back(g_path);
    }
    for (const auto& n : names) full.push_back(g_path.empty() ? n : Join(g_path, n.c_str()));  // Ce PC : lecteurs
    std::wstring text;
    for (const auto& p : full) text += (text.empty() ? L"\"" : L"\r\n\"") + p + L"\"";
    bool ok = false;
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
            if (!ok) GlobalFree(h);
        }
        CloseClipboard();
    }
    if (!ok) { SetStatus(L"Impossible d'utiliser le presse-papiers."); return; }
    SetStatus(full.size() == 1 ? L"Chemin copié : " + full[0]
                               : std::to_wstring(full.size()) + L" chemins copiés dans le presse-papiers.");
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
void SelectEditName(HWND ed);

void OnEditSel() {
    HWND ed = ListView_GetEditControl(g_list);
    if (!ed || !g_editing) return;
    SelectEditName(ed);
    if (g_tabSince) {
        g_tabSince = 0;
        if (!g_tabTyped.empty()) SendMessageW(ed, EM_REPLACESEL, TRUE, (LPARAM)g_tabTyped.c_str());
        g_tabTyped.clear();
    }
}

void SelectEditName(HWND ed) {
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

// Tab pendant un renommage : saisie suivante, ouverte après celle-ci (message
// posté avant le lancement du renommage, donc traité avant son résultat).
// renamed : un renommage est lancé, la saisie suivante s'ouvrira après lui
// (sinon l'ancien nom resterait affiché pendant qu'on tape le suivant).
void ContinueWithTab(const std::wstring& next, bool renamed) {
    if (next.empty()) return;
    g_tabEdit = next;
    g_tabTyped.clear();
    g_tabSince = GetTickCount64();
    if (!renamed) PostMessageW(g_hwnd, WM_APP_TABEDIT, 0, 0);
}

void OnTabEdit() {
    const int e = g_tabEdit.empty() || !HasView() ? -1 : FindEntryByName(g_tabEdit);
    g_tabEdit.clear();
    if (e < 0 || g_view.rowOf[e] < 0) { g_tabSince = 0; return; }
    ApplySelection({e}, e, true);
    SetFocus(g_list);
    ListView_EditLabel(g_list, g_view.rowOf[e]);
}

void OnEndLabelEdit(const NMLVDISPINFOW* di) {
    g_editing = false;
    const std::wstring old = g_editName;
    const std::wstring next = std::move(g_tabNext);
    g_tabNext.clear();
    g_editName.clear();
    if (!di->item.pszText || old.empty()) return;  // Échap
    const std::wstring typed = di->item.pszText;
    std::wstring name = CleanFileName(typed);
    if (!name.empty() && !g_editExt.empty()) name += g_editExt;  // extension masquée : conservée
    const size_t slash = old.rfind(L'\\');  // résultat de recherche : seul le nom change
    const std::wstring oldLeaf = slash == std::wstring::npos ? old : old.substr(slash + 1);
    if (name.empty() || name == oldLeaf || name == old) { ContinueWithTab(next, false); return; }
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
    ContinueWithTab(next, true);
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

// ───────────────────────────── Onglets, fenêtres ─────────────────────────────
//
// Chaque onglet garde son dossier, son historique, son tri, son filtre, sa
// sélection, sa position et le contenu déjà lu : y revenir l'affiche aussitôt,
// puis le dossier est relu en arrière-plan (comme F5). Seul l'onglet affiché
// est surveillé.

struct Tab {
    std::wstring path, query, filter, focus;
    std::vector<std::wstring> back, fwd, selected;
    int sortCol = COL_NAME;
    bool sortAsc = true;
    int top = 0;
    std::shared_ptr<const DirData> data;
};
std::vector<Tab> g_tabList(1);
size_t g_tab = 0;
int g_tabDrag = -1;  // onglet que l'on fait glisser (réorganisation)

std::wstring TabText(const Tab& t) {
    std::wstring s = t.query.empty() ? HistoryText(t.path) : L"Recherche « " + t.query + L" »";
    if (s.size() > 32) s = s.substr(0, 31) + L"…";
    return s;
}

void UpdateTabStrip() {
    if (!g_tabBar) return;
    SendMessageW(g_tabBar, WM_SETREDRAW, FALSE, 0);
    TabCtrl_DeleteAllItems(g_tabBar);
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    for (size_t i = 0; i < g_tabList.size(); ++i) {
        std::wstring t = TabText(g_tabList[i]);
        it.pszText = t.data();
        TabCtrl_InsertItem(g_tabBar, (int)i, &it);
    }
    it.pszText = (LPWSTR)L"  +  ";  // dernier faux onglet : en ouvrir un nouveau
    TabCtrl_InsertItem(g_tabBar, (int)g_tabList.size(), &it);
    TabCtrl_SetCurSel(g_tabBar, (int)g_tab);
    SendMessageW(g_tabBar, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_tabBar, nullptr, TRUE);
}

// L'onglet affiché suit ce que montre la liste (appelé à chaque résultat).
void SyncTab() {
    Tab& t = g_tabList[g_tab];
    const std::wstring before = TabText(t);
    t.path = g_path;
    t.query = g_query;
    t.data = g_view.data;
    if (std::wstring now = TabText(t); now != before && g_tabBar) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = now.data();
        TabCtrl_SetItem(g_tabBar, (int)g_tab, &it);
    }
}

// Mémorise l'état affiché dans l'onglet courant, avant d'en changer.
void SaveTab() {
    Tab& t = g_tabList[g_tab];
    t.path = g_pending.active ? g_pending.path : g_path;  // navigation en cours : là où l'on va
    t.query = g_pending.active ? g_pending.query : g_query;
    t.filter = g_filter;
    t.back = g_back;
    t.fwd = g_fwd;
    t.sortCol = g_sortCol;
    t.sortAsc = g_sortAsc;
    t.selected = HasView() ? NamesOf(SelectedEntries()) : std::vector<std::wstring>{};
    const int f = HasView() ? FocusedEntry() : -1;
    t.focus = f >= 0 ? NameOf(f) : L"";
    t.top = ListView_GetTopIndex(g_list);
    t.data = g_pending.active ? nullptr : g_view.data;
}

void ShowTab(size_t i) {
    if (i >= g_tabList.size()) return;
    if (g_editing) SendMessageW(g_list, LVM_CANCELEDITLABEL, 0, 0);
    g_tab = i;
    const Tab t = g_tabList[i];
    g_back = t.back;
    g_fwd = t.fwd;
    g_sortCol = t.sortCol;
    g_sortAsc = t.sortAsc;
    UpdateSortArrow();
    g_filter = t.filter;
    g_goneNote.clear();
    g_searchSync = true;
    SetWindowTextW(g_searchBox, (t.filter.empty() ? t.query : t.filter).c_str());
    g_searchSync = false;
    if (g_tabBar) TabCtrl_SetCurSel(g_tabBar, (int)i);
    const bool cached = t.data && _wcsicmp(t.data->path.c_str(), t.path.c_str()) == 0 && t.data->query == t.query &&
                        t.data->searchDone && t.data->entries.size() <= 100000;
    if (!cached) {
        Navigate(t.path, Nav::Refresh, t.focus, t.selected, {}, false, &t.query);
        g_pending.top = t.top;
        return;
    }
    // Contenu déjà lu : affiché tout de suite, puis relu en arrière-plan.
    Pending p;
    p.active = true;
    p.path = t.path;
    p.nav = Nav::Refresh;
    p.focus = t.focus;
    p.selected = t.selected;
    p.query = t.query;
    p.top = t.top;
    g_pending = std::move(p);
    auto* r = new Result{Jobs_NewGeneration().gen, JobKind::Navigate};
    r->data = t.data;
    r->order = SortOrder(*t.data, g_sortCol, g_sortAsc, g_prefs, g_filter);
    OnResult(r);
    if (t.query.empty()) Refresh();  // changements faits pendant qu'on était ailleurs
}

void SwitchTab(size_t i) {
    if (i == g_tab || i >= g_tabList.size()) {
        UpdateTabStrip();
        return;
    }
    SaveTab();
    ShowTab(i);
}

// Nouvel onglet après l'onglet courant ; activate : on y va (sinon il attend en arrière-plan).
void NewTab(const std::wstring& path, bool activate) {
    SaveTab();
    Tab t;
    t.path = path;
    t.sortCol = g_sortCol;
    t.sortAsc = g_sortAsc;
    g_tabList.insert(g_tabList.begin() + (ptrdiff_t)g_tab + 1, std::move(t));
    if (!activate) {
        UpdateTabStrip();
        Flash(L"Ouvert dans un nouvel onglet : " + HistoryText(path));
        return;
    }
    ++g_tab;
    UpdateTabStrip();
    ShowTab(g_tab);
}

void CloseTab(size_t i) {
    if (i >= g_tabList.size()) return;
    if (g_tabList.size() == 1) {  // dernier onglet : la fenêtre se ferme
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    const bool active = i == g_tab;
    if (!active) SaveTab();
    g_tabList.erase(g_tabList.begin() + (ptrdiff_t)i);
    if (active) {
        g_tab = std::min(i, g_tabList.size() - 1);
        UpdateTabStrip();
        ShowTab(g_tab);
        return;
    }
    if (i < g_tab) --g_tab;
    UpdateTabStrip();
}

void MoveTab(size_t from, size_t to) {
    if (from == to || from >= g_tabList.size() || to >= g_tabList.size()) return;
    SaveTab();
    Tab t = std::move(g_tabList[from]);
    g_tabList.erase(g_tabList.begin() + (ptrdiff_t)from);
    g_tabList.insert(g_tabList.begin() + (ptrdiff_t)to, std::move(t));
    if (g_tab == from) g_tab = to;
    else if (from < g_tab && to >= g_tab) --g_tab;
    else if (from > g_tab && to <= g_tab) ++g_tab;
    UpdateTabStrip();
}

// Nouvelle fenêtre (Ctrl+N), un peu décalée par rapport à celle-ci, sur le même écran.
void NewWindow(const std::wstring& path) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(g_hwnd, &wp);
    // Décalée de 30 px, sans déborder de la zone de travail de l'écran.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const LONG w = std::min(wp.rcNormalPosition.right - wp.rcNormalPosition.left, wa.right - wa.left);
    const LONG h = std::min(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, wa.bottom - wa.top);
    LONG x = wp.rcNormalPosition.left + S(30), y = wp.rcNormalPosition.top + S(30);
    if (x + w > wa.right) x = wa.left;
    if (y + h > wa.bottom) y = wa.top;
    x = std::max(x, wa.left);
    y = std::max(y, wa.top);
    std::wstring arg = path;
    if (!arg.empty() && arg.back() == L'\\') arg += L'\\';  // « C:\" » : le \ protégerait le guillemet
    wchar_t at[96];
    swprintf_s(at, L"%ld,%ld,%ld,%ld", x, y, w, h);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + arg + L"\" --at " + at;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        SetStatus(L"Impossible d'ouvrir une nouvelle fenêtre : " + ErrorText(GetLastError()));
    }
}

void ShowTabMenu(int hit, POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Nouvel onglet\tCtrl+T");
    if (hit >= 0) {
        AppendMenuW(m, MF_STRING, 2, L"Dupliquer l'onglet");
        AppendMenuW(m, MF_STRING, 5, L"Ouvrir dans une nouvelle fenêtre");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 3, L"Fermer l'onglet\tCtrl+W");
        AppendMenuW(m, MF_STRING | (g_tabList.size() > 1 ? 0 : MF_GRAYED), 4, L"Fermer les autres onglets");
        AppendMenuW(m, MF_STRING | (hit + 1 < (int)g_tabList.size() ? 0 : MF_GRAYED), 6, L"Fermer les onglets à droite");
    }
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case 1: NewTab(L"", true); break;
        case 2: {
            SaveTab();
            Tab copy = g_tabList[(size_t)hit];
            g_tabList.insert(g_tabList.begin() + hit + 1, std::move(copy));
            if ((size_t)hit < g_tab) ++g_tab;
            UpdateTabStrip();
            SwitchTab((size_t)hit + 1);
            break;
        }
        case 3: CloseTab((size_t)hit); break;
        case 4: {
            SaveTab();
            const bool same = (size_t)hit == g_tab;
            Tab keep = g_tabList[(size_t)hit];
            g_tabList.assign(1, std::move(keep));
            g_tab = 0;
            UpdateTabStrip();
            if (!same) ShowTab(0);
            break;
        }
        case 6: {
            SaveTab();
            const bool shown = g_tab > (size_t)hit;  // l'onglet affiché disparaît : on montre celui cliqué
            g_tabList.resize((size_t)hit + 1);
            if (shown) g_tab = (size_t)hit;
            UpdateTabStrip();
            if (shown) ShowTab(g_tab);
            break;
        }
        case 5: {
            if ((size_t)hit == g_tab) SaveTab();
            NewWindow(g_tabList[(size_t)hit].path);
            break;
        }
    }
    SetFocus(g_list);
}

// « Épingler dans FastExplorer » : Accès rapide propre à l'application, qui
// marche même quand celui de Windows ne répond pas. Un dossier épinglé dans
// Windows se désépingle par le menu du Shell (« Désépingler de l'Accès rapide »).
std::vector<MenuExtra> PinExtras(const std::wstring& path, bool withOpen) {
    std::vector<MenuExtra> x;
    if (withOpen) {
        x.push_back({L"Ouvrir", [path] { Navigate(path, Nav::Normal); }, true});
        x.push_back({L"Ouvrir dans un nouvel onglet", [path] { NewTab(path, true); }});
    }
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
    } else if (sel.empty() && !g_path.empty()) {  // fond du dossier : annuler / rétablir, options d'affichage
        if (!g_undo.empty()) extras.push_back({L"Annuler " + StepText(g_undo.back()) + L"\tCtrl+Z", Undo});
        if (!g_redo.empty()) extras.push_back({L"Rétablir " + StepText(g_redo.back()) + L"\tCtrl+Y", Redo});
        extras.push_back({L"Afficher les éléments masqués", ToggleHidden, false, g_prefs.showHidden});
        extras.push_back({L"Afficher les extensions de noms de fichiers", ToggleExtensions, false, g_prefs.showExtensions});
    }
    if ((!names.empty() || !g_path.empty()) && GetKeyState(VK_SHIFT) >= 0)  // Maj : le Shell la propose déjà
        extras.push_back({L"Copier en tant que chemin d'accès\tCtrl+Maj+C", [names] { CopyPaths(names); }});
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
        if (single >= 0 && E(single).isDir() && (is(L"opennewwindow") || is(L"opennewtab"))) {
            const std::wstring dir = g_path.empty() ? names[0] : Join(g_path, names[0].c_str());
            if (is(L"opennewwindow")) NewWindow(dir); else NewTab(dir, true);
            return true;
        }
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
        if (!path.empty() && is(L"opennewwindow")) { NewWindow(path); return true; }
        if (!path.empty() && is(L"opennewtab")) { NewTab(path, true); return true; }
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
    for (HWND h : {g_list, g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp, g_btnRefresh, g_nav, g_crumbs, g_searchBox, g_tabBar})
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
    HDWP dwp = BeginDeferWindowPos(11);
    auto place = [&](HWND w, int x, int y, int cx, int cy) {
        if (dwp) dwp = DeferWindowPos(dwp, w, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
        else MoveWindow(w, x, y, cx, cy, TRUE);
    };
    // Onglets tout en haut, sur toute la largeur.
    const int tabH = g_fontH + S(12);
    place(g_tabBar, 0, S(2), rc.right, tabH);
    const int y0 = S(2) + tabH + S(2);
    int x = m;
    for (HWND b : {g_btnBack, g_btnFwd, g_btnUp}) {
        place(b, x, y0, bw, h);
        x += bw + S(2);
    }
    x += S(4);
    const int searchW = std::min(S(260), std::max(0, (int)rc.right - x - m) / 3);
    const int addrW = std::max(0, (int)rc.right - x - m - searchW - S(6) - bw - S(2));
    place(g_addr, x, y0 + (h - editH) / 2, addrW, editH);
    place(g_crumbs, x, y0 + (h - editH) / 2, addrW, editH);  // même place
    place(g_btnRefresh, x + addrW + S(2), y0, bw, h);  // Actualiser, à droite de l'adresse comme l'Explorateur
    place(g_searchBox, (int)rc.right - m - searchW, y0 + (h - editH) / 2, searchW, editH);
    const int top = y0 + h + m;
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
    g_tabBar = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 0, 0, 0,
                               0, hwnd, (HMENU)ID_TABS, g_inst, nullptr);
    g_btnBack = btn(L"←", ID_BACK);
    g_btnFwd = btn(L"→", ID_FWD);
    g_btnUp = btn(L"↑", ID_UP);
    g_btnRefresh = btn(L"↻", ID_REFRESH);

    // Zone de saisie cachée : le fil d'Ariane occupe sa place, sauf pendant la saisie.
    g_addr = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                             (HMENU)ID_ADDR, g_inst, nullptr);
    // Suggestions de dossiers pendant la frappe (énumérées par le Shell sur son propre thread).
    SHAutoComplete(g_addr, SHACF_FILESYS_DIRS | SHACF_AUTOSUGGEST_FORCE_ON | SHACF_AUTOAPPEND_FORCE_OFF);
    CrumbHost crumbs;
    crumbs.navigate = [](const std::wstring& path) {
        Navigate(path, Nav::Normal);
        SetFocus(g_list);
    };
    crumbs.edit = BeginAddressEdit;
    crumbs.openNewTab = [](const std::wstring& path) { NewTab(path, false); };
    crumbs.driveText = NavPane_DriveText;
    g_crumbs = Crumbs_Create(hwnd, ID_CRUMBS, std::move(crumbs));
    g_searchBox = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                                  0, 0, hwnd, (HMENU)ID_SEARCH, g_inst, nullptr);
    Edit_SetCueBannerTextFocused(g_searchBox, L"Filtrer, Entrée : rechercher (Ctrl+F)", TRUE);

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
    UpdateTabStrip();
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
            if ((HWND)wp == g_btnBack || (HWND)wp == g_btnFwd) { ShowHistoryMenu((HWND)wp == g_btnBack); return 0; }
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
        case WM_APP_TABEDIT: OnTabEdit(); return 0;
        case WM_APP_CMDFAIL: {
            std::unique_ptr<std::wstring> text((std::wstring*)lp);
            if (text->empty()) ShowSummary();
            else SetStatus(*text);
            return 0;
        }
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
        case WM_APP_TESTDPI: {
            RECT r;
            GetWindowRect(hwnd, &r);
            r.right = r.left + MulDiv(r.right - r.left, (int)wp, (int)g_dpi);
            r.bottom = r.top + MulDiv(r.bottom - r.top, (int)wp, (int)g_dpi);
            return SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(wp, wp), (LPARAM)&r);
        }
        case WM_DPICHANGED: {
            // Passage sur un écran d'une autre échelle : colonnes, police, icônes,
            // taille et positions changent en plusieurs temps. Sans précaution,
            // chaque étape s'affiche (et Windows recopie l'ancien contenu aux
            // nouvelles positions) : tout semble se décaler un instant. On
            // recompose donc sans dessiner, puis on peint une seule fois.
            std::vector<HWND> kids;
            for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                if (IsWindowVisible(c)) kids.push_back(c);
            for (HWND c : kids) SendMessageW(c, WM_SETREDRAW, FALSE, 0);
            const UINT nd = HIWORD(wp);
            for (int i = 0; i < COL_COUNT; ++i)
                ListView_SetColumnWidth(g_list, i, MulDiv(ListView_GetColumnWidth(g_list, i), (int)nd, (int)g_dpi));
            g_dpi = nd;
            ApplyFont();
            ApplyImageList();
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            Layout();
            for (HWND c : kids) SendMessageW(c, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
                case ID_REFRESH: Refresh(); SetFocus(g_list); return 0;
                case ID_ADDR:
                    if (HIWORD(wp) == EN_KILLFOCUS) EndAddressEdit();
                    return 0;
                case ID_SEARCH:  // recherche lancée après une courte pause dans la frappe
                    if (HIWORD(wp) == EN_CHANGE && !g_searchSync) ApplyFilter();
                    return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (LRESULT r = 0; NavPane_Notify(hdr, &r)) return r;
            if (hdr->hwndFrom == g_tabBar) {
                if (hdr->code == TCN_SELCHANGE) {
                    const int sel = TabCtrl_GetCurSel(g_tabBar);
                    if (sel >= (int)g_tabList.size()) NewTab(L"", true);  // « + »
                    else if (sel >= 0) SwitchTab((size_t)sel);
                    SetFocus(g_list);
                } else if (hdr->code == NM_RCLICK) {
                    TCHITTESTINFO ht{};
                    GetCursorPos(&ht.pt);
                    const POINT screen = ht.pt;
                    ScreenToClient(g_tabBar, &ht.pt);
                    const int hit = TabCtrl_HitTest(g_tabBar, &ht);
                    ShowTabMenu(hit < (int)g_tabList.size() ? hit : -1, screen);
                    return TRUE;
                }
                return 0;
            }
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
                case NM_DBLCLK: {
                    const int e = HasView() ? EntryAt(((NMITEMACTIVATE*)lp)->iItem) : -1;
                    if (e >= 0 && E(e).isDir() && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+double-clic : nouvelle fenêtre
                        NewWindow(g_path.empty() ? std::wstring(NameOf(e)) : Join(g_path, NameOf(e)));
                        return 0;
                    }
                    Activate(((NMITEMACTIVATE*)lp)->iItem);
                    return 0;
                }
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
    if (g_tabSince && !g_editing && m.hwnd == g_list && (m.message == WM_KEYDOWN || m.message == WM_CHAR)) {
        if (GetTickCount64() - g_tabSince > 2000) g_tabSince = 0;  // la saisie ne s'est pas ouverte : on lâche
        else if (m.message == WM_CHAR) { if (m.wParam >= 32) g_tabTyped += (wchar_t)m.wParam; return true; }
        else if (m.wParam != VK_ESCAPE) { TranslateMessage(&m); return true; }  // → WM_CHAR, gardé ci-dessus
        else { g_tabSince = 0; g_tabEdit.clear(); g_editAfter.clear(); return true; }
    }
    if (g_editing) {
        // Tab / Maj+Tab : valide le nom et renomme l'élément suivant / précédent, comme l'Explorateur.
        if (m.message == WM_KEYDOWN && m.wParam == VK_TAB && m.hwnd == ListView_GetEditControl(g_list)) {
            const int e = HasView() ? FindEntryByName(g_editName) : -1;
            const int row = e >= 0 ? g_view.rowOf[e] : -1;
            const int next = row + (GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            g_tabNext = row >= 0 && next >= 0 && next < RowCount() ? NameOf(EntryAt(next)) : L"";
            SetFocus(g_list);  // la liste valide la saisie (LVN_ENDLABELEDIT)
            g_tabNext.clear();
            return true;
        }
        return false;  // saisie d'un nom : toutes les touches vont à la zone d'édition
    }
    // Onglets à la souris : clic du milieu = fermer (sur un onglet) ou ouvrir
    // dans un nouvel onglet (sur un dossier) ; glisser un onglet le déplace.
    if (m.message == WM_MBUTTONUP && (m.hwnd == g_tabBar || m.hwnd == g_list || m.hwnd == g_nav)) {
        POINT c{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
        if (m.hwnd == g_tabBar) {
            TCHITTESTINFO ht{c, 0};
            const int hit = TabCtrl_HitTest(g_tabBar, &ht);
            if (hit >= 0 && hit < (int)g_tabList.size()) CloseTab((size_t)hit);
        } else if (m.hwnd == g_list) {
            LVHITTESTINFO ht{};
            ht.pt = c;
            const int e = HasView() ? EntryAt(ListView_HitTest(g_list, &ht)) : -1;
            if (e >= 0 && E(e).isDir())
                NewTab(g_path.empty() ? std::wstring(NameOf(e)) : Join(g_path, NameOf(e)), false);
        } else {
            POINT s = c;
            ClientToScreen(g_nav, &s);
            if (std::wstring p; NavPane_PathAt(s, p)) NewTab(p, false);
        }
        return true;
    }
    if (m.hwnd == g_tabBar) {
        if (m.message == WM_LBUTTONDOWN) {
            TCHITTESTINFO ht{{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)}, 0};
            const int hit = TabCtrl_HitTest(g_tabBar, &ht);
            g_tabDrag = hit >= 0 && hit < (int)g_tabList.size() ? hit : -1;
        } else if (m.message == WM_MOUSEMOVE && g_tabDrag >= 0 && (m.wParam & MK_LBUTTON)) {
            TCHITTESTINFO ht{{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)}, 0};
            const int hit = TabCtrl_HitTest(g_tabBar, &ht);
            if (hit >= 0 && hit < (int)g_tabList.size() && hit != g_tabDrag && (size_t)g_tabDrag == g_tab) {
                MoveTab((size_t)g_tabDrag, (size_t)hit);
                g_tabDrag = hit;
            }
        } else if (m.message == WM_LBUTTONUP) {
            g_tabDrag = -1;
        }
        return false;
    }
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
    if (ctrl) {  // onglets et fenêtres
        const bool sh = GetKeyState(VK_SHIFT) < 0;
        const size_t n = g_tabList.size();
        switch (m.wParam) {
            case 'T': NewTab(L"", true); return true;
            case 'W':
            case VK_F4: CloseTab(g_tab); return true;
            case VK_RETURN:  // Ctrl+Entrée : le dossier sélectionné dans une nouvelle fenêtre
                if (focus == g_list && HasView()) {
                    const int e = FocusedEntry();
                    if (e >= 0 && E(e).isDir()) {
                        NewWindow(g_path.empty() ? std::wstring(NameOf(e)) : Join(g_path, NameOf(e)));
                        return true;
                    }
                }
                break;
            case 'N':
                if (sh && focus == g_list) break;  // Ctrl+Maj+N : nouveau dossier
                NewWindow(g_pending.active ? g_pending.path : g_path);
                return true;
            case VK_TAB: SwitchTab((g_tab + (sh ? n - 1 : 1)) % n); return true;
            case VK_PRIOR: SwitchTab((g_tab + n - 1) % n); return true;
            case VK_NEXT: SwitchTab((g_tab + 1) % n); return true;
        }
        if (m.wParam >= '1' && m.wParam <= '9' && !sh) {
            SwitchTab(m.wParam == '9' ? n - 1 : std::min<size_t>(m.wParam - '1', n - 1));
            return true;
        }
    }
    switch (m.wParam) {
        case VK_F5: Refresh(); return true;
        case 'R':
            if (!ctrl) return false;
            Refresh();
            return true;
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
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if ((focus == g_list || focus == g_nav) && ctrl && (m.wParam == 'Z' || m.wParam == 'Y')) {
        if (m.wParam == 'Y' || shift) Redo();
        else Undo();
        return true;
    }
    if (focus != g_list) return false;
    switch (m.wParam) {
        case VK_F2: BeginRename(); return true;
        case VK_DELETE: DeleteNames(NamesOf(SelectedEntries()), shift); return true;
        case 'C':
            if (!ctrl) return false;
            if (shift) CopyPaths(NamesOf(SelectedEntries()));
            else CopyNames(NamesOf(SelectedEntries()), false);
            return true;
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

bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start, const RECT* at) {
    g_inst = inst;
    g_prefs = ReadViewPrefs();  // avant la première lecture : elle filtre déjà les éléments cachés
    Navigate(start, Nav::Normal);  // la lecture démarre pendant qu'on construit la fenêtre
    WarmUpShell();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FastExplorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                at ? at->left : CW_USEDEFAULT, at ? at->top : CW_USEDEFAULT,
                                at ? at->right : MulDiv(1100, (int)sd, 96), at ? at->bottom : MulDiv(700, (int)sd, 96),
                                nullptr, nullptr, inst, nullptr);
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
