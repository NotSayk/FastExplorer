// Fenêtre principale : barre d'adresse + ListView virtuelle + barre d'état.
//
// Règle d'or : le thread de l'interface ne fait jamais d'E/S disque/réseau
// ni de travail proportionnel à la taille du dossier au-delà d'un seuil.
// Lecture, clés de tri, icônes de lecteurs et tris lourds passent par jobs.h.

#include "ui.h"

#include "columns.h"
#include "copyqueue.h"
#include "dragdrop.h"
#include "fileops.h"
#include "foldersize.h"
#include "groups.h"
#include "icons.h"
#include "index.h"
#include "thumbs.h"
#include "jobs.h"
#include "listing.h"
#include "crumbs.h"
#include "navpane.h"
#include "panes.h"
#include "paths.h"
#include "places.h"
#include "sessions.h"
#include "archive.h"
#include "batchrename.h"
#include "net.h"
#include "shellmenu.h"
#include "util.h"
#include "watch.h"

#include <commctrl.h>
#include <dbt.h>
#include <propkey.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
constexpr UINT WM_APP_PASSIVE = WM_APP + 19;    // double panneau : contenu relu du panneau inactif (lParam = DirData*)
constexpr UINT WM_APP_CMDMENU = WM_APP + 60;  // menu d'un bouton de la barre de commandes
constexpr UINT WM_APP_SELSUM = WM_APP + 10;     // sélection changée : résumé dans la barre d'état (une fois par rafale)
constexpr UINT WM_APP_TABEDIT = WM_APP + 11;    // Tab pendant un renommage : renommer l'élément suivant
constexpr UINT WM_APP_CMDFAIL = WM_APP + 12;    // commande tapée dans la barre d'adresse impossible (lParam = std::wstring*)
constexpr UINT WM_APP_DIRSIZE = WM_APP + 14;    // taille d'un dossier calculée (lParam = DirSizeResult*)
constexpr UINT WM_APP_THUMB = WM_APP + 16;      // miniature lue (lParam = ThumbResult*)
constexpr UINT WM_APP_PROPS = WM_APP + 17;      // propriétés lues (lParam = PropResult*)
constexpr UINT WM_APP_SAVEVIEW = WM_APP + 18;   // colonne redimensionnée ou déplacée : mémoriser l'affichage du dossier
constexpr UINT WM_APP_INDEXDONE = WM_APP + 15;  // service de recherche installé / retiré (wParam = code, lParam = 1 : installé)
constexpr UINT ID_SYS_INDEX = 0x0100;           // menu de la fenêtre : activer / désactiver la recherche instantanée
constexpr UINT WM_APP_TESTDPI = WM_APP + 13;    // tests : comme WM_DPICHANGED (wParam = nouvelle échelle), que Windows
                                                // ne laisse pas envoyer d'un autre processus
constexpr UINT_PTR TIMER_LOADING = 1;
constexpr UINT_PTR TIMER_WATCH = 2;
constexpr UINT_PTR TIMER_AFTEROP = 3;
constexpr UINT_PTR TIMER_HOVER = 6;   // souris immobile sur la liste : préparer le menu contextuel
constexpr UINT kMenuHoverMs = 100;
constexpr UINT_PTR TIMER_PROGRESS = 7;  // progression des copies de notre moteur
constexpr UINT_PTR TIMER_PANE = 10;     // sélection posée : mettre à jour le volet de droite
constexpr UINT_PTR TIMER_INDEXASK = 9;  // peu après le premier lancement : proposer la recherche instantanée
constexpr UINT_PTR TIMER_PINNED = 8;    // après un menu contextuel : relire l'Accès rapide (épinglage)
constexpr UINT_PTR TIMER_DRIVES = 4;  // Ce PC : regroupe les annonces d'arrivée / retrait de lecteurs
constexpr UINT_PTR TIMER_EJECT = 5;   // éjection demandée : si elle échoue, on reprend la surveillance  // resélection après une opération, différée pendant un renommage
constexpr UINT kLoadingDelayMs = 150;   // en dessous, aucun « Chargement… » (évite le clignotement)
constexpr size_t kSyncSortMax = 20000;  // au-delà, le re-tri part en arrière-plan
// Rafraîchissement après un changement sur disque : regroupe les rafales
// d'événements (copie de milliers de fichiers) et s'espace sur les gros dossiers.
constexpr double kWatchMinDelayMs = 150, kWatchMaxDelayMs = 2000;

enum { ID_BACK = 100, ID_FWD, ID_UP, ID_ADDR, ID_LIST, ID_STATUS, ID_NAV, ID_CRUMBS, ID_SEARCH, ID_REFRESH, ID_TABS, ID_PANE, ID_LIST2, ID_HDR0, ID_HDR1, ID_CMDBAR,
       CMD_NEW = 300, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_RENAME, CMD_DELETE, CMD_SORT, CMD_VIEW, CMD_MORE };
enum class Nav { Normal, Back, Forward, Refresh };
enum class JobKind { Navigate, Sort };

struct Result {
    uint64_t gen = 0;
    JobKind kind = JobKind::Navigate;
    std::shared_ptr<const DirData> data;
    std::vector<uint32_t> order;
    double sortMs = 0;
};

// Affichage d'un dossier : mode, colonnes (dans l'ordre affiché), tri.
struct FolderView {
    int mode = 0;  // ViewMode
    int px = 96;   // taille des icônes (mode icônes)
    std::vector<ColSpec> cols = Columns_Default();
    ColSpec sort = Columns_Builtin(COL_NAME);
    bool asc = true;
    int group = -1;  // Regrouper par (COL_*), -1 : aucun
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
    std::optional<FolderView> view; // autre dossier : son affichage mémorisé, appliqué avec le résultat
    bool credsAsked = false;        // identifiants réseau déjà demandés pour cette navigation
    std::wstring typed;             // texte tapé dans la barre d'adresse : mémorisé (F4) si l'ouverture réussit
};

enum IconState : uint8_t { ICON_NONE, ICON_GENERIC, ICON_PENDING, ICON_OWN };
enum SizeState : uint8_t { SIZE_KNOWN = 1, SIZE_PARTIAL = 2, SIZE_PENDING = 4 };
enum ThumbState : int32_t { THUMB_UNKNOWN = -1, THUMB_PENDING = -2, THUMB_NONE = -3 };
enum ViewMode { VM_DETAILS, VM_LIST, VM_SMALL, VM_ICONS, VM_TILES, VM_CONTENT };

// Ce qui est affiché. `order` : ligne → index d'entrée ; `rowOf` : l'inverse.
struct View {
    std::shared_ptr<const DirData> data;
    std::vector<uint32_t> order;
    std::vector<int> rowOf;
    std::vector<uint32_t> sorted;   // ordre du tri, avant regroupement et filtres de colonne
    std::vector<uint32_t> shown;    // arranged : éléments gardés par les filtres (repliés compris)
    std::vector<GroupInfo> groups;  // en-têtes de groupes (kGroupRow | numéro dans order)
    bool arranged = false;          // order ≠ sorted (groupes ou filtres)
    std::vector<int32_t> icons;     // par index d'entrée
    std::vector<uint8_t> overlays;  // surimpression par index d'entrée
    std::vector<uint8_t> iconState;
    std::vector<uint8_t> cut;       // 1 = coupé (affiché estompé)
    std::vector<uint64_t> dirBytes; // taille calculée des dossiers (par index d'entrée)
    std::vector<uint8_t> dirState;  // SIZE_* (combinables)
    std::vector<int32_t> thumb;     // modes icônes : place de la miniature (THUMB_* si négatif)
    std::deque<std::pair<PROPERTYKEY, PropValues>> props;  // colonnes de propriétés : valeurs lues (deque : PropsOf garde ses pointeurs valides)
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
HWND g_pane;                   // volet de droite : aperçu ou détails
int g_paneW96 = 320;           // sa largeur (à 96 ppp)
bool g_paneDrag = false;
RECT g_paneSplit{};            // séparateur liste / volet
// Double panneau (F9) : deux listes côte à côte. g_list est celle du panneau
// actif, qui seule est « vivante » (lecture, surveillance, icônes) ; l'autre
// garde l'image de son dernier état (g_passive) et redevient active au clic.
HWND g_lists[2];
HWND g_paneHdr[2];             // chemin au-dessus de chaque liste
int g_side = 0;                // panneau actif : 0 gauche, 1 droite
bool g_dual = false;
HWND g_cmdBar;                 // barre de commandes (Nouveau, Couper, Copier…)
bool g_fullMenu = false;
void FillCommandBar();       // clic droit : menu complet du Shell plutôt que le menu rapide
bool g_dualAtStart = false;    // rétabli au premier dossier affiché
HFONT g_font;
int g_fontH = 16;
UINT g_dpi = 96;
HIMAGELIST g_imgSmall, g_imgLarge;

View g_view;
std::wstring g_path;
Pending g_pending;
std::vector<std::wstring> g_back, g_fwd;
ColSpec g_sort = Columns_Builtin(COL_NAME);  // colonne de tri (même si elle n'est pas affichée)
std::vector<ColSpec> g_cols = Columns_Default();  // sous-élément i du ListView = g_cols[i]
bool g_sortAsc = true;
// Panneau inactif : ce qu'il montrait quand il a perdu la main (ou relu depuis).
struct Passive {
    View view;
    std::vector<ColSpec> cols;
    std::wstring path, query, filter;
    int mode = VM_DETAILS;
    ColSpec sort = Columns_Builtin(COL_NAME);
    bool asc = true;
} g_passive;
std::vector<ColSpec> g_listCols[2];  // colonnes posées sur chaque liste
void SetDual(bool on);
void UpdateCommandBar();
void ReloadPassive();
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
int EntryAt(int row) {
    if (row < 0 || row >= RowCount()) return -1;
    const uint32_t v = g_view.order[row];
    return IsGroupRow(v) ? -1 : (int)v;  // en-tête de groupe : aucun élément
}
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
    const std::vector<uint32_t>& vis = g_view.arranged ? g_view.shown : g_view.order;
    for (uint32_t e : vis) dirs += d.entries[e].isDir();
    const size_t shown = vis.size();
    const size_t colHidden = g_view.sorted.size() > shown ? g_view.sorted.size() - shown : 0;  // filtres de colonne
    size_t visible = d.entries.size();  // sans le filtre instantané (éléments cachés exclus)
    if (!g_filter.empty() && !d.isRoot()) {
        visible = 0;
        for (const Entry& e : d.entries) visible += IsShown(e.attr, g_prefs);
    }
    const size_t hidden = g_filter.empty() ? d.entries.size() - shown - colHidden : 0;
    wchar_t masked[64] = L"";
    if (hidden) swprintf_s(masked, L", %zu masqué%s", hidden, hidden > 1 ? L"s" : L"");
    if (!g_filter.empty()) {
        swprintf_s(s, L"%zu %s sur %zu  (filtre « %.100s »)%s", shown, d.query.empty() ? L"élément(s)" : L"résultat(s)",
                   visible, g_filter.c_str(),
                   d.query.empty() ? L"   •   Entrée : chercher aussi dans les sous-dossiers" : L"");
    } else if (!d.query.empty()) {
        swprintf_s(s, L"%zu résultat%s pour « %.100s »%s%s%s", shown, shown > 1 ? L"s" : L"", d.query.c_str(),
                   d.path.empty() ? L" dans tout le PC" : L"",
                   d.searchDone ? (d.truncated ? L"   •   arrêtée : trop de résultats" : L"")
                                : L"   •   recherche en cours…",
                   d.fromIndex ? L"   •   index instantané"
                               : (d.contentIndexed ? L"   •   contenu : index de Windows Search" : L""));
        if (!d.searchDone) {
            const size_t k = wcslen(s);
            swprintf_s(s + k, _countof(s) - k, L" (%zu dossiers parcourus)", d.scanned);
        }
    } else {
        swprintf_s(s, L"%zu élément%s  (%zu dossier%s, %zu fichier%s%s)", shown, shown > 1 ? L"s" : L"", dirs,
                   dirs > 1 ? L"s" : L"", shown - dirs, shown - dirs > 1 ? L"s" : L"", masked);
    }
    if (colHidden) {
        const size_t k = wcslen(s);
        swprintf_s(s + k, _countof(s) - k, L"   •   filtre de colonne : %zu masqué%s", colHidden, colHidden > 1 ? L"s" : L"");
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
    UpdateCommandBar();
}

// Sous-élément affichant cette colonne (-1 : pas affichée).
int ColIndex(const ColSpec& c) {
    for (size_t i = 0; i < g_cols.size(); ++i)
        if (SameCol(g_cols[i], c)) return (int)i;
    return -1;
}
int ColIndex(int key) { return key == COL_PROP ? -1 : ColIndex(Columns_Builtin(key)); }

int g_groupKey = -1;                                 // Regrouper par (COL_*), -1 : aucun
std::map<int, std::set<std::wstring>> g_colFilter;  // par colonne : tranches gardées
std::set<std::wstring> g_collapsed;                  // groupes repliés
bool g_checkboxes = false;                           // cases à cocher des éléments

bool Groupable(int key) {
    return key == COL_NAME || key == COL_SIZE || key == COL_TYPE || key == COL_DATE || key == COL_CTIME || key == COL_ATIME;
}

void UpdateSortArrow() {
    HWND hdr = ListView_GetHeader(g_list);
    const int sorted = ColIndex(g_sort);
    for (int i = 0; i < (int)g_cols.size(); ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN | HDF_SPLITBUTTON);
        if (i == sorted) hi.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        const int key = g_cols[(size_t)i].key;
        if (HasView() && !g_view.data->isRoot() && Groupable(key)) hi.fmt |= HDF_SPLITBUTTON;  // ▾ : filtre de colonne
        const auto f = g_colFilter.find(key);
        std::wstring text = g_cols[(size_t)i].title;
        if (f != g_colFilter.end() && !f->second.empty()) text += L"  ✓";  // colonne filtrée
        hi.mask |= HDI_TEXT;
        hi.pszText = text.data();
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
    for (int r = 0; r < n; ++r)
        if (!IsGroupRow(g_view.order[r])) g_view.rowOf[g_view.order[r]] = r;
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

int IconOf(const DirData& d, const Entry& e) {
    if (e.icon >= 0) return e.icon;  // déjà résolue par le thread de travail (lecteurs de Ce PC)
    std::wstring key, probe;
    DWORD attr;
    if (e.isDir()) {
        key = L"\x01dir";
        probe = L"folder";
        attr = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        key.assign(d.name(e) + e.extOff, e.nameLen - e.extOff);
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

int IconFor(int idx) { return IconOf(*g_view.data, E(idx)); }

// Chemin complet d'une entrée : réel pour la Corbeille et l'Accueil (DirData::target), sinon dans g_path.
std::wstring FullPathOf(const DirData& d, int idx) {
    if (d.isVirtual()) return (size_t)idx < d.target.size() ? d.target[idx] : std::wstring();
    return Join(d.path, d.name(d.entries[idx]));
}
std::wstring FullPath(int idx) { return FullPathOf(*g_view.data, idx); }

bool InVirtual() { return HasView() && g_view.data->isVirtual(); }

// Nom affiché d'un emplacement : « Ce PC », « Corbeille », « Accueil » ou le chemin.
std::wstring PlaceText(const std::wstring& path) {
    if (path.empty()) return L"Ce PC";
    if (IsVirtualPath(path)) return Places_Title(path);
    return path;
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

// ───────────────────────────── Taille des dossiers ─────────────────────────────
//
// Calculée en arrière-plan pour les sous-dossiers du dossier affiché, gardée
// en cache par chemin : y revenir l'affiche aussitôt. Une valeur de moins de
// 30 s n'est pas recalculée (sauf F5) ; une opération de fichiers périme celles
// des dossiers qu'elle touche. Pas de calcul sur les lecteurs réseau et optiques.

struct SizeCache {
    uint64_t bytes = 0;
    bool complete = true;
    ULONGLONG at = 0;  // GetTickCount64 du calcul
};
std::unordered_map<std::wstring, SizeCache> g_sizeCache;  // clé : chemin en minuscules
bool g_forceSizes = false;   // F5 : tout recalculer
size_t g_sizeWaiting = 0;    // dossiers demandés pour la vue, pas encore mesurés
constexpr ULONGLONG kSizeFreshMs = 30000;

std::wstring SizeKey(std::wstring p) {
    CharLowerBuffW(p.data(), (DWORD)p.size());
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

// Un dossier et tous ses parents : leur taille a changé.
void ForgetSizes(const std::wstring& path) {
    std::wstring p = path, parent, leaf;
    for (int guard = 0; guard < 512 && !p.empty(); ++guard) {
        g_sizeCache.erase(SizeKey(p));
        if (!ParentOf(p, parent, leaf) || parent == p) break;
        p = parent;
    }
}

bool SizesWanted(const DirData& d) {
    if (d.isRoot() || d.isVirtual() || d.inArchive() || !d.query.empty() || d.path.size() < 2) return false;
    if (d.path[0] == L'\\' && d.path[1] == L'\\') return false;  // UNC
    const wchar_t root[4] = {d.path[0], L':', L'\\', 0};
    const UINT t = GetDriveTypeW(root);
    return t != DRIVE_REMOTE && t != DRIVE_CDROM;
}

// Nouvelle vue : tailles déjà connues reprises du cache.
void InitDirSizes() {
    const DirData& d = *g_view.data;
    const size_t n = d.entries.size();
    g_view.dirBytes.assign(n, 0);
    g_view.dirState.assign(n, 0);
    if (!SizesWanted(d) || g_sizeCache.empty()) return;
    for (size_t i = 0; i < n; ++i) {
        if (!d.entries[i].isDir()) continue;
        auto it = g_sizeCache.find(SizeKey(Join(d.path, d.name(d.entries[i]))));
        if (it == g_sizeCache.end()) continue;
        g_view.dirBytes[i] = it->second.bytes;
        g_view.dirState[i] = SIZE_KNOWN | (it->second.complete ? 0 : SIZE_PARTIAL);
    }
}

// Demande la taille des sous-dossiers affichés qui ne sont pas à jour, dans l'ordre d'affichage.
void RequestDirSizes() {
    const bool force = std::exchange(g_forceSizes, false);
    g_sizeWaiting = 0;
    std::vector<std::wstring> dirs;
    if (HasView() && SizesWanted(*g_view.data)) {
        const ULONGLONG now = GetTickCount64();
        for (uint32_t i : g_view.sorted) {
            if (!E((int)i).isDir()) break;  // dossiers d'abord
            std::wstring full = FullPath((int)i);
            auto it = g_sizeCache.find(SizeKey(full));
            if (!force && it != g_sizeCache.end() && now - it->second.at < kSizeFreshMs) continue;
            g_view.dirState[i] |= SIZE_PENDING;
            dirs.push_back(std::move(full));
            if (dirs.size() >= 5000) break;
        }
    }
    g_sizeWaiting = dirs.size();
    DirSizes_Request(g_view.serial, std::move(dirs));
}

void SortView();

void OnDirSize(DirSizeResult* raw) {
    std::unique_ptr<DirSizeResult> r(raw);
    if (g_sizeCache.size() > 200000) g_sizeCache.clear();
    g_sizeCache[SizeKey(r->path)] = SizeCache{r->size.bytes, r->size.complete, GetTickCount64()};
    std::wstring parent, leaf;
    if (!HasView() || !ParentOf(r->path, parent, leaf) || _wcsicmp(parent.c_str(), g_path.c_str()) != 0 ||
        !g_query.empty())
        return;
    const int e = FindEntryByName(leaf);
    if (e < 0 || !(g_view.dirState[e] & SIZE_PENDING)) return;
    g_view.dirBytes[e] = r->size.bytes;
    g_view.dirState[e] = SIZE_KNOWN | (r->size.complete ? 0 : SIZE_PARTIAL);
    if (const int row = g_view.rowOf[e]; row >= 0) ListView_RedrawItems(g_list, row, row);
    if (ListView_GetItemState(g_list, g_view.rowOf[e], LVIS_SELECTED) & LVIS_SELECTED) ScheduleSelectionSummary();
    if (g_sizeWaiting && --g_sizeWaiting == 0 && g_sort.key == COL_SIZE) SortView();  // tout est mesuré : on retrie
}

// ───────────────────────────── Modes d'affichage ─────────────────────────────
//
// Détails, liste, petites icônes : liste d'images système (16 px × échelle).
// Icônes (32 à 256 px) : notre propre liste d'images, à la taille demandée :
// icônes système agrandies (liste « jumbo ») et miniatures, lues sur un thread.

int g_viewMode = VM_DETAILS;
int g_iconPx = 96;                    // taille des icônes à 96 ppp (VM_ICONS)
HIMAGELIST g_bigList = nullptr;       // liste d'images des modes icônes
int g_bigPx = 0;                      // sa taille en pixels réels
std::unordered_map<int, int> g_bigSys;  // icône système → place dans g_bigList
uint32_t g_bigOverlays = 0;           // surimpressions déjà déclarées (bits 1 à 15)
int g_wheelAcc = 0;                   // Ctrl+molette : crans partiels (pavés tactiles)
constexpr int kZoomPx[] = {32, 48, 64, 96, 128, 192, 256};

bool g_frozen = false;                // liste gelée par OnResult : ne pas la redessiner en cours de route
void ListRedraw(bool on) {
    if (!g_frozen) SendMessageW(g_list, WM_SETREDRAW, on, 0);
}
bool g_compact = true;                // mode compact : lignes serrées (sinon espacées, comme l'Explorateur)

// Modes qui utilisent g_bigList : icônes, mosaïques, contenu.
bool BigMode() { return g_viewMode >= VM_ICONS; }
// Taille des images de g_bigList (à 96 ppp) : mosaïques et contenu en 48 px, comme l'Explorateur.
int BigPx96() { return g_viewMode == VM_ICONS ? g_iconPx : 48; }

DWORD RegDword(const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : def;
}

void ResetBigList() {
    if (g_bigList) ImageList_Destroy(g_bigList);
    g_bigPx = S(BigPx96());
    g_bigList = ImageList_Create(g_bigPx, g_bigPx, ILC_COLOR32, 64, 64);
    g_bigSys.clear();
    g_bigOverlays = 0;
    if (HasView()) g_view.thumb.assign(g_view.data->entries.size(), THUMB_UNKNOWN);
    if (HasView()) Thumbs_SetSerial(g_view.serial);  // demandes à l'ancienne taille abandonnées
}

// Beaucoup de miniatures accumulées (≈ 256 Ko chacune en très grandes icônes) : on repart de zéro.
void TrimBigList() {
    if (g_bigList && ImageList_GetImageCount(g_bigList) > 3000) ResetBigList();
}

// Icône système agrandie, ajoutée une fois à g_bigList.
int SysSlot(int sys) {
    if (auto it = g_bigSys.find(sys); it != g_bigSys.end()) return it->second;
    IImageList* il = nullptr;
    int slot = -1;
    if (SUCCEEDED(SHGetImageList(g_bigPx > S(48) ? SHIL_JUMBO : SHIL_EXTRALARGE, __uuidof(IImageList), (void**)&il))) {
        HICON ic = nullptr;
        if (SUCCEEDED(il->GetIcon(sys, ILD_TRANSPARENT, &ic)) && ic) {
            slot = ImageList_ReplaceIcon(g_bigList, -1, ic);  // mise à l'échelle par la liste d'images
            DestroyIcon(ic);
        }
    }
    g_bigSys[sys] = slot;
    return slot;
}

// Surimpression système (flèche de raccourci, état cloud) déclarée dans g_bigList.
void BigOverlay(int ov) {
    if (ov <= 0 || ov > 15 || (g_bigOverlays & (1u << ov))) return;
    g_bigOverlays |= 1u << ov;
    IImageList* il = nullptr;
    int img = -1;
    if (SUCCEEDED(SHGetImageList(SHIL_SMALL, __uuidof(IImageList), (void**)&il)) && SUCCEEDED(il->GetOverlayImage(ov, &img)) &&
        img >= 0)
        if (const int slot = SysSlot(img); slot >= 0) ImageList_SetOverlayImage(g_bigList, slot, ov);
}

// Copie une image 32 bits (sw × sh, alpha non prémultiplié) au centre d'une case
// transparente cw × ch (en bas si `bottom`) et l'ajoute à `list`.
int AddPadded(HIMAGELIST list, int cw, int ch, const std::vector<uint32_t>& src, int sw, int sh, bool bottom) {
    BITMAPINFO di{};
    di.bmiHeader = {sizeof(BITMAPINFOHEADER), cw, -ch, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &di, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return -1;
    bool alpha = false;  // image opaque livrée avec un canal alpha nul : on la rend opaque
    for (uint32_t p : src)
        if (p >> 24) { alpha = true; break; }
    auto* dst = (uint32_t*)bits;
    memset(dst, 0, (size_t)cw * ch * 4);
    const int w = std::min(sw, cw), h = std::min(sh, ch);
    const int x0 = (cw - w) / 2, y0 = bottom ? ch - h : (ch - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t p = src[(size_t)y * sw + x];
            dst[(size_t)(y0 + y) * cw + x0 + x] = alpha ? p : (p | 0xFF000000u);
        }
    const int slot = ImageList_Add(list, dib, nullptr);
    DeleteObject(dib);
    return slot;
}

// Pixels 32 bits d'un bitmap (haut en premier).
bool BitmapPixels(HBITMAP bmp, std::vector<uint32_t>& px, int& w, int& h) {
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    w = bm.bmWidth;
    h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    px.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    const int got = GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return got != 0;
}

// Miniature (au plus px × px) posée en bas et au centre d'un carré transparent.
int AddThumb(HBITMAP bmp) {
    std::vector<uint32_t> src;
    int w, h;
    if (!BitmapPixels(bmp, src, w, h)) return -1;
    return AddPadded(g_bigList, g_bigPx, g_bigPx, src, w, h, true);
}

// ── Mode non compact : lignes espacées ──
// Le ListView règle la hauteur des lignes sur celle de sa liste de petites
// icônes. On lui donne donc une liste de cases plus hautes que les icônes, où
// chaque icône système est recopiée au centre (une fois par icône différente).
HIMAGELIST g_smallPick = nullptr;      // liste système des petites icônes (selon l'échelle)
HIMAGELIST g_rowList = nullptr;        // cases espacées
int g_rowW = 0, g_rowH = 0;
HIMAGELIST g_rowSrc = nullptr;         // liste système d'où viennent ses icônes
std::unordered_map<int, int> g_rowSys;  // icône système → case
uint32_t g_rowOverlays = 0;

bool RowMode() { return !g_compact && g_viewMode <= VM_SMALL && g_rowList; }

int RowSlot(int sys) {
    if (auto it = g_rowSys.find(sys); it != g_rowSys.end()) return it->second;
    int slot = -1;
    if (HICON ic = ImageList_GetIcon(g_smallPick, sys, ILD_TRANSPARENT)) {
        ICONINFO ii{};
        if (GetIconInfo(ic, &ii)) {
            std::vector<uint32_t> px, mask;
            int w = 0, h = 0, mw = 0, mh = 0;
            if (ii.hbmColor && BitmapPixels(ii.hbmColor, px, w, h)) {
                bool alpha = false;
                for (uint32_t p : px)
                    if (p >> 24) { alpha = true; break; }
                // Ancienne icône sans canal alpha : la transparence vient du masque.
                if (!alpha && ii.hbmMask && BitmapPixels(ii.hbmMask, mask, mw, mh) && mw == w && mh >= h)
                    for (size_t i = 0; i < px.size(); ++i) px[i] = (mask[i] & 0xFFFFFF) ? 0 : (px[i] | 0xFF000000u);
                if (!alpha && mask.empty())
                    for (uint32_t& p : px) p |= 0xFF000000u;
                slot = AddPadded(g_rowList, g_rowW, g_rowH, px, w, h, false);
            }
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
        DestroyIcon(ic);
    }
    g_rowSys[sys] = slot;
    return slot;
}

void RowOverlay(int ov) {
    if (ov <= 0 || ov > 15 || (g_rowOverlays & (1u << ov))) return;
    g_rowOverlays |= 1u << ov;
    int img = -1;
    if (SUCCEEDED(((IImageList*)g_smallPick)->GetOverlayImage(ov, &img)) && img >= 0)
        if (const int slot = RowSlot(img); slot >= 0) ImageList_SetOverlayImage(g_rowList, slot, ov);
}
void OnThumb(ThumbResult* raw) {
    std::unique_ptr<ThumbResult> r(raw);
    struct Bmp { HBITMAP b; ~Bmp() { if (b) DeleteObject(b); } } bmp{r->bmp};
    if (!HasView() || !g_bigList || r->serial != g_view.serial || r->px != g_bigPx || r->idx >= g_view.thumb.size())
        return;
    const int slot = bmp.b ? AddThumb(bmp.b) : -1;
    g_view.thumb[r->idx] = slot >= 0 ? slot : THUMB_NONE;
    if (slot >= 0)
        if (const int row = g_view.rowOf[r->idx]; row >= 0) ListView_RedrawItems(g_list, row, row);
}

// Image d'une entrée dans les modes icônes : miniature si elle existe, sinon l'icône agrandie.
int BigImage(int idx, int sysIcon) {
    int32_t& th = g_view.thumb[idx];
    if (th == THUMB_UNKNOWN) {
        const Entry& e = E(idx);
        th = THUMB_NONE;
        // Dossier : aperçu de ses premiers fichiers, lisible à partir de 48 px.
        if (!g_view.data->isRoot() && !g_view.data->listOnly() && Thumbs_Wanted(e.attr, e.size) &&
            !(e.isDir() && g_bigPx < S(48))) {
            th = THUMB_PENDING;
            Thumbs_Request(g_view.serial, (uint32_t)idx, FullPath(idx), g_bigPx);
        }
    }
    return th >= 0 ? th : SysSlot(sysIcon);
}

void SaveFolderView();
void SaveViewMode() { SaveFolderView(); }  // le mode est mémorisé pour le dossier affiché

// Mosaïques et contenu : taille des cases. Contenu = une case par ligne, sur toute la largeur.
void ApplyTileInfo() {
    if (g_viewMode != VM_TILES && g_viewMode != VM_CONTENT) return;
    RECT rc;
    GetWindowRect(g_list, &rc);
    const bool content = g_viewMode == VM_CONTENT;
    const int lines = content ? 3 : 2;  // sous le nom : type et taille (mosaïques) ; date, taille, type (contenu)
    LVTILEVIEWINFO ti{sizeof ti};
    ti.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS | LVTVIM_LABELMARGIN;
    ti.dwFlags = LVTVIF_FIXEDSIZE;
    const int w = content ? std::max(S(200), (int)(rc.right - rc.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) - S(6))
                          : S(280);
    ti.sizeTile = {w, std::max(g_bigPx, g_fontH * (lines + 1)) + S(content ? 14 : 10)};
    ti.cLines = lines;
    ti.rcLabelMargin = {S(6), 0, S(4), 0};
    ListView_SetTileViewInfo(g_list, &ti);
}

// Hauteur des lignes (détails, liste, petites icônes) : liste système en mode
// compact, sinon g_rowList (cases plus hautes que les icônes).
void ApplyRowSpacing() {
    HIMAGELIST want = g_smallPick;
    if (!g_compact && g_viewMode <= VM_SMALL && g_smallPick) {
        int cx = 16, cy = 16;
        ImageList_GetIconSize(g_smallPick, &cx, &cy);
        const int h = std::max(cy, g_fontH) + S(14);
        if (!g_rowList || g_rowW != cx || g_rowH != h || g_rowSrc != g_smallPick) {
            if (g_rowList) {
                for (HWND lv : g_lists) ListView_SetImageList(lv, g_smallPick, LVSIL_SMALL);  // l'inactive aussi
                ImageList_Destroy(g_rowList);
            }
            g_rowW = cx;
            g_rowH = h;
            g_rowSrc = g_smallPick;
            g_rowList = ImageList_Create(cx, h, ILC_COLOR32, 64, 64);
            g_rowSys.clear();
            g_rowOverlays = 0;
        }
        want = g_rowList;
    }
    if (want && (HIMAGELIST)SendMessageW(g_list, LVM_GETIMAGELIST, LVSIL_SMALL, 0) != want)
        ListView_SetImageList(g_list, want, LVSIL_SMALL);
}
void LoadCompact() {
    // Réglage propre à l'application s'il a été changé ; sinon celui de l'Explorateur.
    const DWORD win = RegDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"UseCompactMode", 0);
    g_compact = RegDword(L"Software\\FastExplorer", L"Compact", win) != 0;
}

// px96 : taille des icônes (VM_ICONS), 0 = inchangée.
// ───────────────────────────── Regrouper par, filtres de colonne ─────────────────────────────
//
// Groupes : lignes d'en-tête insérées dans l'ordre affiché (la liste est virtuelle),
// dessinées par nous, repliables d'un clic. Seulement en Détails et Contenu (une
// ligne par élément). Filtres : tranches de valeurs gardées, par colonne (flèche ▾
// de l'en-tête), oubliés en changeant de dossier.


std::wstring TypeLabel(const DirData& d, const Entry& e) {
    if (e.isDir()) return d.server ? L"Partage réseau" : d.path == kNetPath ? L"Ordinateur" : L"Dossier de fichiers";
    if (e.extOff + 1 >= e.nameLen) return L"Fichier";
    std::wstring ext(d.name(e) + e.extOff + 1, e.nameLen - e.extOff - 1);
    CharUpperBuffW(ext.data(), (DWORD)ext.size());
    return L"Fichier " + ext;
}

BucketFn BucketFor(int key) {
    std::shared_ptr<const DirData> data = g_view.data;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    switch (key) {
        case COL_NAME:
            return [data](uint32_t i) {
                const wchar_t* n = data->name(data->entries[i]);
                if (const wchar_t* s = wcsrchr(n, L'\\')) n = s + 1;  // résultats de recherche : chemin relatif
                return NameBucket(n);
            };
        case COL_SIZE: return [data](uint32_t i) { const Entry& e = data->entries[i]; return SizeBucket(e.size, e.isDir()); };
        case COL_TYPE:
            return [data](uint32_t i) {
                const Entry& e = data->entries[i];
                return Bucket{e.isDir() ? 0 : 1, TypeLabel(*data, e)};
            };
        case COL_CTIME: return [data, now](uint32_t i) { return DateBucket(data->entries[i].ctime, now); };
        case COL_ATIME: return [data, now](uint32_t i) { return DateBucket(data->entries[i].atime, now); };
        default: return [data, now](uint32_t i) { return DateBucket(data->entries[i].mtime, now); };
    }
}

bool GroupsShown() {
    return HasView() && !g_view.data->isRoot() && Groupable(g_groupKey) && (g_viewMode == VM_DETAILS || g_viewMode == VM_CONTENT);
}

// order ← sorted, regroupé et filtré.
void ArrangeView() {
    g_view.groups.clear();
    g_view.shown.clear();
    g_view.arranged = false;
    if (!HasView()) return;
    std::vector<ColumnFilter> filters;
    if (!g_view.data->isRoot())
        for (const auto& [k, keep] : g_colFilter)
            if (!keep.empty()) filters.push_back({BucketFor(k), keep});
    const bool groups = GroupsShown();
    if (filters.empty() && !groups) {
        g_view.order = g_view.sorted;
        return;
    }
    // Groupes dans leur ordre naturel (dates : les plus récentes d'abord), inversé si l'on trie
    // la même colonne dans l'autre sens.
    const bool natural = g_groupKey != COL_DATE && g_groupKey != COL_CTIME && g_groupKey != COL_ATIME;
    const bool reverse = groups && g_sort.key == g_groupKey && g_sortAsc != natural;
    g_view.order = ArrangeOrder(g_view.sorted, filters, groups ? BucketFor(g_groupKey) : BucketFn{}, reverse, g_collapsed,
                                g_view.groups, &g_view.shown);
    g_view.arranged = true;
}

void UpdateSortArrow();

// Regroupement, repli ou filtre changé : la liste suit, sélection gardée.
void ReArrange() {
    if (!HasView()) return;
    const auto sel = SelectedEntries();
    const int focus = FocusedEntry();
    ArrangeView();
    RebuildRowOf();
    ListView_SetItemCountEx(g_list, RowCount(), LVSICF_NOSCROLL);
    ApplySelection(sel, focus, false);
    InvalidateRect(g_list, nullptr, TRUE);
    UpdateSortArrow();
    ShowSummary();
}

// Ligne d'en-tête de groupe → son numéro (-1 : élément ordinaire).
int GroupAt(int row) {
    if (row < 0 || row >= RowCount() || !IsGroupRow(g_view.order[row])) return -1;
    const int g = (int)(g_view.order[row] & ~kGroupRow);
    return g < (int)g_view.groups.size() ? g : -1;
}

void ToggleGroup(int row) {
    const int g = GroupAt(row);
    if (g < 0) return;
    const std::wstring label = g_view.groups[(size_t)g].label;
    if (!g_collapsed.erase(label)) g_collapsed.insert(label);
    ReArrange();
    for (int r = 0; r < RowCount(); ++r)  // l'en-tête garde le focus
        if (const int k = GroupAt(r); k >= 0 && g_view.groups[(size_t)k].label == label) {
            ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(g_list, r, LVIS_FOCUSED, LVIS_FOCUSED);
            ListView_SetSelectionMark(g_list, r);
            ListView_EnsureVisible(g_list, r, FALSE);
            break;
        }
}

void ApplyViewMode(int mode, int px96, bool save = true) {
    g_viewMode = mode;
    if (px96) g_iconPx = std::clamp(px96, 32, 256);
    const int focus = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
    ListRedraw(false);
    ApplyRowSpacing();
    if (BigMode()) {
        if (!g_bigList || g_bigPx != S(BigPx96())) ResetBigList();
        ListView_SetImageList(g_list, g_bigList, LVSIL_NORMAL);
        if (mode == VM_ICONS)
            ListView_SetIconSpacing(g_list, g_bigPx + S(std::max(28, g_iconPx / 3)), g_bigPx + g_fontH * 2 + S(14));
    }
    static const DWORD kViews[] = {LV_VIEW_DETAILS, LV_VIEW_LIST, LV_VIEW_SMALLICON, LV_VIEW_ICON, LV_VIEW_TILE, LV_VIEW_TILE};
    ListView_SetView(g_list, kViews[mode]);
    ApplyTileInfo();
    if (focus >= 0) ListView_EnsureVisible(g_list, focus, FALSE);
    ListRedraw(true);
    InvalidateRect(g_list, nullptr, TRUE);
    if (save) {
        SaveViewMode();
        if (HasView() && (g_view.arranged || GroupsShown())) ReArrange();
    }
}

void SetCompact(bool on) {
    g_compact = on;
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"Compact", REG_DWORD, &v, sizeof v);
    ApplyViewMode(g_viewMode, 0, false);
}

void LoadViewMode() {
    LoadCompact();
    const DWORD mode = RegDword(L"Software\\FastExplorer", L"ViewMode", VM_DETAILS);
    const DWORD px = RegDword(L"Software\\FastExplorer", L"IconPx", 96);
    ApplyViewMode(mode <= VM_CONTENT ? (int)mode : VM_DETAILS, (int)px, false);
}

// Ctrl+molette : détails → liste → petites icônes → icônes de 32 à 256 px (et retour).
// Depuis les mosaïques ou le contenu, on repart des icônes moyennes.
void Zoom(int dir) {
    int level;  // 0 détails, 1 liste, 2 petites icônes, 3… tailles d'icônes
    if (g_viewMode < VM_ICONS) level = g_viewMode == VM_DETAILS ? 0 : g_viewMode == VM_LIST ? 1 : 2;
    else {
        const int px = g_viewMode == VM_ICONS ? g_iconPx : 48;
        level = 3;
        for (int i = 0; i < (int)std::size(kZoomPx); ++i)
            if (kZoomPx[i] <= px) level = 3 + i;
    }
    level = std::clamp(level + dir, 0, 2 + (int)std::size(kZoomPx));
    if (level < 3) ApplyViewMode(level == 0 ? VM_DETAILS : level == 1 ? VM_LIST : VM_SMALL, 0);
    else ApplyViewMode(VM_ICONS, kZoomPx[level - 3]);
}

// Ctrl+Maj+1…8, comme l'Explorateur.
void ViewByKey(int k) {
    switch (k) {
        case 1: ApplyViewMode(VM_ICONS, 256); break;  // très grandes icônes
        case 2: ApplyViewMode(VM_ICONS, 96); break;   // grandes icônes
        case 3: ApplyViewMode(VM_ICONS, 48); break;   // icônes moyennes
        case 4: ApplyViewMode(VM_SMALL, 0); break;
        case 5: ApplyViewMode(VM_LIST, 0); break;
        case 6: ApplyViewMode(VM_DETAILS, 0); break;
        case 7: ApplyViewMode(VM_TILES, 0); break;
        case 8: ApplyViewMode(VM_CONTENT, 0); break;
    }
}

void ChooseColumns();
void TogglePane(PaneMode m);
void ApplyViewToAll();
void ResetFolderView();

MenuExtra ViewMenu() {
    MenuExtra m;
    m.text = L"Affichage";
    const bool icons = g_viewMode == VM_ICONS;
    m.sub = {{L"Très grandes icônes\tCtrl+Maj+1", [] { ViewByKey(1); }, false, icons && g_iconPx >= 192},
             {L"Grandes icônes\tCtrl+Maj+2", [] { ViewByKey(2); }, false, icons && g_iconPx >= 80 && g_iconPx < 192},
             {L"Icônes moyennes\tCtrl+Maj+3", [] { ViewByKey(3); }, false, icons && g_iconPx < 80},
             {L"Petites icônes\tCtrl+Maj+4", [] { ViewByKey(4); }, false, g_viewMode == VM_SMALL},
             {L"Liste\tCtrl+Maj+5", [] { ViewByKey(5); }, false, g_viewMode == VM_LIST},
             {L"Détails\tCtrl+Maj+6", [] { ViewByKey(6); }, false, g_viewMode == VM_DETAILS},
             {L"Mosaïques\tCtrl+Maj+7", [] { ViewByKey(7); }, false, g_viewMode == VM_TILES},
             {L"Contenu\tCtrl+Maj+8", [] { ViewByKey(8); }, false, g_viewMode == VM_CONTENT},
             {},
             {L"Mode compact", [] { SetCompact(!g_compact); }, false, g_compact},
             {},
             {L"Volet d'aperçu\tAlt+P", [] { TogglePane(PaneMode::Preview); }, false, Pane_Mode() == PaneMode::Preview},
             {L"Volet de détails\tAlt+Maj+P", [] { TogglePane(PaneMode::Details); }, false,
              Pane_Mode() == PaneMode::Details},
             {L"Double panneau\tF9", [] { SetDual(!g_dual); }, false, g_dual},
             {L"File d'attente des copies…", [] { CopyQueue_Show(g_hwnd); }},
             {},
             {L"Choisir les colonnes…", [] { ChooseColumns(); }},
             {L"Appliquer à tous les dossiers", [] { ApplyViewToAll(); }},
             {L"Réinitialiser l'affichage de ce dossier", [] { ResetFolderView(); }}};
    return m;
}
// ───────────────────────────── Colonnes de propriétés ─────────────────────────────

// Valeurs d'une propriété pour la vue (créées vides à la première demande).
PropValues* PropsOf(const PROPERTYKEY& pk) {
    if (!HasView()) return nullptr;
    for (auto& [k, v] : g_view.props)
        if (IsEqualPropertyKey(k, pk)) return &v;
    g_view.props.emplace_back(pk, PropValues{});
    g_view.props.back().second.Reset(g_view.data->entries.size());
    return &g_view.props.back().second;
}

std::vector<uint8_t> g_propSortWait;  // tri sur une propriété : entrées dont on attend la valeur
size_t g_propWaiting = 0;

// Lit les propriétés manquantes d'une entrée (colonnes affichées, et colonne de tri).
void RequestRowProps(int idx, bool urgent) {
    std::vector<PROPERTYKEY> keys;
    auto want = [&](const PROPERTYKEY& pk) {
        PropValues* pv = PropsOf(pk);
        if (!pv || pv->state[idx] != 0) return;
        pv->state[idx] = 1;
        keys.push_back(pk);
    };
    for (const ColSpec& c : g_cols)
        if (c.key == COL_PROP) want(c.pk);
    if (g_sort.key == COL_PROP) want(g_sort.pk);
    if (keys.empty()) return;
    constexpr uint32_t kNotLocal =
        FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    Props_Request(g_view.serial, (uint32_t)idx, FullPath(idx), !(E(idx).attr & kNotLocal), std::move(keys),
                  urgent);
}

// Tri sur une propriété : toutes les valeurs sont lues en arrière-plan, puis on retrie.
void RequestSortProps() {
    g_propWaiting = 0;
    g_propSortWait.clear();
    if (g_sort.key != COL_PROP || !HasView() || g_view.data->isRoot() || g_view.data->listOnly()) return;
    PropValues* pv = PropsOf(g_sort.pk);
    const size_t n = g_view.data->entries.size();
    g_propSortWait.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pv->state[i] == 2) continue;
        g_propSortWait[i] = 1;
        ++g_propWaiting;
        if (pv->state[i] == 0) RequestRowProps((int)i, false);
    }
}

void SortView();

void OnProps(PropResult* raw) {
    std::unique_ptr<PropResult> r(raw);
    if (!HasView() || r->serial != g_view.serial || r->idx >= g_view.data->entries.size()) return;
    for (size_t k = 0; k < r->keys.size(); ++k)
        if (PropValues* pv = PropsOf(r->keys[k])) {
            pv->text[r->idx] = std::move(r->text[k]);
            pv->num[r->idx] = r->num[k];
            pv->state[r->idx] = 2;
        }
    if (const int row = g_view.rowOf[r->idx]; row >= 0) ListView_RedrawItems(g_list, row, row);
    if (g_propWaiting && r->idx < g_propSortWait.size() && g_propSortWait[r->idx]) {
        g_propSortWait[r->idx] = 0;
        if (--g_propWaiting == 0) SortView();  // tout est lu : on retrie
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
            if (!g_view.data->isRoot() && !g_view.data->listOnly() && NeedsOwnIcon(e.attr, NameOf(idx) + e.extOff, e.nameLen - e.extOff)) {
                st = ICON_PENDING;
                Icons_Request(g_view.serial, (uint32_t)idx, FullPath(idx));
            }
        }
        const int sys = g_view.icons[idx];
        it.iImage = BigMode() ? BigImage(idx, sys) : RowMode() ? RowSlot(sys) : sys;
    }
    if (it.mask & LVIF_STATE) {  // surimpression (flèche de raccourci, état cloud), élément coupé
        // Élément caché : estompé, comme dans l'Explorateur (même rendu qu'un élément coupé).
        const bool dim = g_view.cut[idx] || (!g_view.data->isRoot() && (e.attr & FILE_ATTRIBUTE_HIDDEN));
        if (BigMode()) BigOverlay(g_view.overlays[idx]);
        else if (RowMode()) RowOverlay(g_view.overlays[idx]);
        it.state = INDEXTOOVERLAYMASK(g_view.overlays[idx]) | (dim ? LVIS_CUT : 0) |
                   (it.iItem == g_dropRow ? LVIS_DROPHILITED : 0);
        it.stateMask = LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED;
        if (g_checkboxes) {  // case cochée = élément sélectionné, comme l'Explorateur
            const bool on = ListView_GetItemState(g_list, it.iItem, LVIS_SELECTED) != 0;
            it.state |= INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
            it.stateMask |= LVIS_STATEIMAGEMASK;
        }
    }
    if (it.mask & LVIF_COLUMNS) {  // mosaïques, contenu : colonnes affichées sous le nom
        static UINT buf[3];
        const int want[3] = {g_viewMode == VM_CONTENT ? COL_DATE : COL_TYPE, COL_SIZE,
                             g_viewMode == VM_CONTENT ? COL_TYPE : -1};
        UINT n = 0;
        for (int k : want)
            if (const int c = k >= 0 ? ColIndex(k) : -1; c > 0) buf[n++] = (UINT)c;
        if (it.puColumns) {  // tampon fourni par le ListView (cColumns = sa capacité)
            it.cColumns = std::min<UINT>(n, it.cColumns);
            for (UINT i = 0; i < it.cColumns; ++i) it.puColumns[i] = buf[i];
        } else {
            it.cColumns = n;
            it.puColumns = buf;
        }
        if ((it.mask & LVIF_COLFMT) && it.piColFmt)
            for (UINT i = 0; i < it.cColumns; ++i) it.piColFmt[i] = 0;
    }
    if (!(it.mask & LVIF_TEXT) || !it.pszText || it.cchTextMax <= 0) return;
    wchar_t* out = it.pszText;
    const int cch = it.cchTextMax;
    out[0] = 0;
    if (it.iSubItem < 0 || it.iSubItem >= (int)g_cols.size()) return;
    const bool root = g_view.data->isRoot();
    const bool virt = g_view.data->isVirtual();
    switch (const ColSpec& col = g_cols[it.iSubItem]; col.key) {
        case COL_NAME:
            if (const DriveInfo* di = DriveOfEntry(idx))
                wcsncpy_s(out, cch, DriveDisplayName(NameOf(idx), e.extra, *di).c_str(), _TRUNCATE);
            else {
                const uint32_t hide = e.isDir() || root
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
            else if (virt) {  // Corbeille : taille totale du dossier supprimé ($I)
                if (e.size) wcsncpy_s(out, cch, BytesText(e.size).c_str(), _TRUNCATE);
            }
            else if (const uint8_t st = g_view.dirState[idx]; st & SIZE_KNOWN) {
                // Unités adaptées (« 106 Go ») ; dossier en partie illisible : au moins cette taille.
                const std::wstring s = ((st & SIZE_PARTIAL) ? L"≥ " : L"") + BytesText(g_view.dirBytes[idx]);
                wcsncpy_s(out, cch, s.c_str(), _TRUNCATE);
            } else if (st & SIZE_PENDING) wcsncpy_s(out, cch, L"…", _TRUNCATE);
            break;
        case COL_TYPE:
            if (root) wcsncpy_s(out, cch, DriveTypeText(e.extra), _TRUNCATE);
            else if (e.isDir())
                wcsncpy_s(out, cch,
                          g_view.data->server            ? L"Partage réseau"
                          : g_view.data->path == kNetPath ? L"Ordinateur"
                                                          : L"Dossier de fichiers",
                          _TRUNCATE);
            else if (e.extOff + 1 < e.nameLen) {
                swprintf_s(out, cch, L"Fichier %s", NameOf(idx) + e.extOff + 1);
                CharUpperBuffW(out + 8, (DWORD)wcslen(out + 8));
            } else wcsncpy_s(out, cch, L"Fichier", _TRUNCATE);
            break;
        case COL_DATE:
            if (!root) FormatDate(e.mtime, out, cch);  // Ce PC : jauge dessinée à la place
            break;
        case COL_CTIME:
            if (!root && e.ctime) FormatDate(e.ctime, out, cch);
            break;
        case COL_ATIME:
            if (!root && e.atime) FormatDate(e.atime, out, cch);
            break;
        case COL_ATTR:
            if (!root) wcsncpy_s(out, cch, AttrText(e.attr).c_str(), _TRUNCATE);
            break;
        case COL_ORIGIN:
            if ((size_t)idx < g_view.data->origin.size())
                wcsncpy_s(out, cch, g_view.data->origin[idx].c_str(), _TRUNCATE);
            break;
        case COL_PROP:
            if (root) break;
            if (PropValues* pv = PropsOf(col.pk)) {
                if (pv->state[idx] == 0) RequestRowProps(idx, true);  // ligne visible : lue en priorité
                wcsncpy_s(out, cch, pv->text[idx].c_str(), _TRUNCATE);
            }
            break;
    }
}

// Ce PC : colonnes adaptées aux lecteurs (pas celles des dossiers).
std::vector<ColSpec> RootColumns() {
    std::vector<ColSpec> v = {Columns_Builtin(COL_NAME), Columns_Builtin(COL_SIZE), Columns_Builtin(COL_TYPE),
                              Columns_Builtin(COL_DATE)};
    v[1].title = L"Espace libre";
    v[1].width96 = 200;
    v[3].title = L"Utilisation";
    return v;
}

// Corbeille et Accueil : colonnes fixes, comme l'Explorateur.
std::vector<ColSpec> VirtualColumns(const std::wstring& path) {
    std::vector<ColSpec> v = {Columns_Builtin(COL_NAME), Columns_Builtin(COL_ORIGIN), Columns_Builtin(COL_DATE),
                              Columns_Builtin(COL_SIZE), Columns_Builtin(COL_TYPE)};
    v[0].width96 = 300;
    if (path == kBinPath) {
        v[2].title = L"Date de suppression";
    } else if (path == kNetPath) {  // serveurs connus
        v = {v[0], v[1], v[2], v[4]};
        v[1].title = L"Lecteurs réseau";
        v[2].title = L"Dernière visite";
    } else {
        v[1].title = L"Emplacement";
        v[2].title = L"Utilisé le";
    }
    return v;
}

// Serveur : ses partages et leur commentaire.
std::vector<ColSpec> ServerColumns() {
    std::vector<ColSpec> v = {Columns_Builtin(COL_NAME), Columns_Builtin(COL_TYPE), Columns_Builtin(COL_ORIGIN)};
    v[0].width96 = 260;
    v[2].title = L"Commentaire";
    return v;
}

// Archive : ce que le répertoire du ZIP sait (pas de propriétés du Shell).
std::vector<ColSpec> ArchiveColumns() {
    return {Columns_Builtin(COL_NAME), Columns_Builtin(COL_DATE), Columns_Builtin(COL_TYPE), Columns_Builtin(COL_SIZE)};
}

// Colonnes imposées (Ce PC, Corbeille, Accueil, archives) : ni mémorisées ni appliquées aux dossiers.
bool FixedColumns() {
    return HasView() && (g_view.data->isRoot() || g_view.data->isVirtual() || g_view.data->listOnly());
}

bool g_applyingCols = false;  // colonnes reconstruites par le programme : rien à mémoriser
std::vector<ColSpec> g_folderCols = Columns_Default();  // colonnes des dossiers (hors Ce PC), dans l'ordre affiché

// Colonnes affichées, dans l'ordre de l'en-tête, avec leur largeur actuelle.
std::vector<ColSpec> CurrentColumns() {
    const int n = (int)g_cols.size();
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    if (n) ListView_GetColumnOrderArray(g_list, n, order.data());
    std::vector<ColSpec> v;
    for (int d = 0; d < n; ++d) {
        const int i = order[d];
        if (i < 0 || i >= n) continue;
        ColSpec c = g_cols[i];
        if (g_viewMode == VM_DETAILS) c.width96 = std::max(1, MulDiv(ListView_GetColumnWidth(g_list, i), 96, (int)g_dpi));
        else  // hors Détails, le ListView ne donne pas les largeurs : celles mémorisées
            for (const ColSpec& f : g_folderCols)
                if (SameCol(f, c)) c.width96 = f.width96;
        v.push_back(std::move(c));
    }
    return v;
}

std::wstring ColumnsKey(const std::vector<ColSpec>& cols) {
    std::wstring s;
    for (const auto& c : cols) s += Columns_Id(c) + L":" + std::to_wstring(c.width96) + L":" + c.title + L",";
    return s;
}

// Reconstruit les colonnes du ListView. `cols` : dans l'ordre d'affichage. Le
// Nom reste le sous-élément 0 (le ListView ne sait pas supprimer la colonne 0,
// et c'est elle qu'on renomme) ; l'ordre affiché passe par l'ordre des colonnes.
void ApplyColumns(std::vector<ColSpec> cols) {
    auto isName = [](const ColSpec& c) { return c.key == COL_NAME; };
    if (std::none_of(cols.begin(), cols.end(), isName)) cols.insert(cols.begin(), Columns_Builtin(COL_NAME));
    const int nameAt = (int)(std::find_if(cols.begin(), cols.end(), isName) - cols.begin());
    std::vector<ColSpec> sub{cols[nameAt]};
    for (int i = 0; i < (int)cols.size(); ++i)
        if (i != nameAt) sub.push_back(cols[i]);
    std::vector<int> order(cols.size());
    for (int d = 0; d < (int)cols.size(); ++d) order[d] = d == nameAt ? 0 : 1 + (d < nameAt ? d : d - 1);

    g_applyingCols = true;
    ListRedraw(false);
    HWND hdr = ListView_GetHeader(g_list);
    for (int i = Header_GetItemCount(hdr); i-- > 1;) ListView_DeleteColumn(g_list, i);
    for (int i = 0; i < (int)sub.size(); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.pszText = (LPWSTR)sub[i].title.c_str();
        c.cx = S(sub[i].width96);
        c.fmt = i > 0 && sub[i].right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.iSubItem = i;
        if (i == 0 && Header_GetItemCount(hdr) > 0) ListView_SetColumn(g_list, 0, &c);
        else ListView_InsertColumn(g_list, i, &c);
    }
    g_cols = std::move(sub);
    ListView_SetColumnOrderArray(g_list, (int)order.size(), order.data());
    UpdateSortArrow();
    ListRedraw(true);
    InvalidateRect(g_list, nullptr, TRUE);
    g_applyingCols = false;
}

// Ce PC ↔ dossier : colonnes à changer ?
void UpdateColumnsForView() {
    const bool root = HasView() && g_view.data->isRoot();
    const std::vector<ColSpec> want = root                               ? RootColumns()
                                      : InVirtual()                        ? VirtualColumns(g_view.data->path)
                                      : HasView() && g_view.data->inArchive() ? ArchiveColumns()
                                      : HasView() && g_view.data->server      ? ServerColumns()
                                                                           : g_folderCols;
    if (ColumnsKey(want) != ColumnsKey(CurrentColumns())) ApplyColumns(want);
}

// Ce PC : jauge d'occupation de chaque lecteur (rouge au-delà de 90 %, comme l'Explorateur).
HFONT g_groupFont = nullptr, g_groupFontOf = nullptr;  // police des en-têtes de groupes (et celle dont elle dérive)

void DrawGroupHeader(NMLVCUSTOMDRAW* cd, int g) {
    const GroupInfo& gi = g_view.groups[(size_t)g];
    const int row = (int)cd->nmcd.dwItemSpec;
    HDC dc = cd->nmcd.hdc;
    RECT r;
    if (!ListView_GetItemRect(g_list, row, &r, LVIR_BOUNDS)) return;
    RECT client;
    GetClientRect(g_list, &client);
    r.left = client.left;
    r.right = std::max(r.right, client.right);
    const COLORREF bk = ListView_GetBkColor(g_list);
    HBRUSH b = CreateSolidBrush(bk == CLR_NONE ? GetSysColor(COLOR_WINDOW) : bk);
    FillRect(dc, &r, b);
    DeleteObject(b);
    const bool focused = (ListView_GetItemState(g_list, row, LVIS_FOCUSED) & LVIS_FOCUSED) && GetFocus() == g_list;
    if (focused) {
        RECT f = r;
        InflateRect(&f, -S(1), -S(1));
        DrawFocusRect(dc, &f);
    }
    HFONT base = (HFONT)SendMessageW(g_list, WM_GETFONT, 0, 0);
    if (g_groupFontOf != base || !g_groupFont) {
        if (g_groupFont) DeleteObject(g_groupFont);
        LOGFONTW lf{};
        GetObjectW(base ? base : GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_SEMIBOLD;
        g_groupFont = CreateFontIndirectW(&lf);
        g_groupFontOf = base;
    }
    HGDIOBJ old = SelectObject(dc, g_groupFont);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF accent = RGB(0, 84, 153);
    SetTextColor(dc, accent);
    const std::wstring text = std::wstring(gi.collapsed ? L"›  " : L"⌄  ") + gi.label + L"  (" + std::to_wstring(gi.count) + L")";
    RECT tr = r;
    tr.left += S(8);
    DrawTextW(dc, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
    tr.top = r.top;
    tr.bottom = r.bottom;
    DrawTextW(dc, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    // Trait jusqu'au bord, comme l'Explorateur.
    RECT line{tr.right + S(10), (r.top + r.bottom) / 2, r.right - S(8), 0};
    line.bottom = line.top + std::max(1, S(1));
    if (line.right > line.left) {
        HBRUSH lb = CreateSolidBrush(RGB(204, 220, 236));
        FillRect(dc, &line, lb);
        DeleteObject(lb);
    }
}

LRESULT OnListCustomDraw(NMLVCUSTOMDRAW* cd) {
    if (HasView() && !g_view.groups.empty()) {
        switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                if (const int g = GroupAt((int)cd->nmcd.dwItemSpec); g >= 0) {
                    DrawGroupHeader(cd, g);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
        }
        return CDRF_DODEFAULT;
    }
    if (!HasView() || !g_view.data->isRoot() || g_viewMode != VM_DETAILS) return CDRF_DODEFAULT;
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT: {
            const int row = (int)cd->nmcd.dwItemSpec;
            const DriveInfo* di = DriveOfEntry(EntryAt(row));
            const int col = ColIndex(COL_DATE);
            RECT r;
            if (!di || !di->total || col < 1 || !ListView_GetSubItemRect(g_list, row, col, LVIR_BOUNDS, &r)) break;
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
// ───────────────────────────── Affichage mémorisé par dossier ─────────────────────────────
//
// HKCU\Software\FastExplorer\Views : une valeur par dossier dont on a changé
// l'affichage (chemin en minuscules ; « ::pc » pour Ce PC), et « Default » pour
// tous les autres. Texte : « mode|px|tri|croissant|colonne:largeur,… ».

constexpr wchar_t kViewsKey[] = L"Software\\FastExplorer\\Views";
constexpr DWORD kViewsMax = 5000;  // au-delà, on oublie les plus anciens

std::wstring ViewName(const std::wstring& path) {
    if (path.empty()) return L"::pc";
    std::wstring s = path;
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring EncodeView(const FolderView& v) {
    std::wstring s = std::to_wstring(v.mode) + L"|" + std::to_wstring(v.px) + L"|" + Columns_Id(v.sort) + L"|" +
                     (v.asc ? L"1" : L"0") + L"|";
    for (size_t i = 0; i < v.cols.size(); ++i) s += (i ? L"," : L"") + Columns_Id(v.cols[i]) + L":" + std::to_wstring(v.cols[i].width96);
    if (v.group >= 0) s += L"|" + std::to_wstring(v.group);
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> v;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::wstring::npos; a = b + 1) v.push_back(s.substr(a, b - a));
    v.push_back(s.substr(a));
    return v;
}

bool DecodeView(const std::wstring& s, FolderView& v) {
    const auto f = Split(s, L'|');
    if (f.size() != 5 && f.size() != 6) return false;
    FolderView r;
    r.mode = _wtoi(f[0].c_str());
    r.px = std::clamp(_wtoi(f[1].c_str()), 32, 256);
    if (r.mode < VM_DETAILS || r.mode > VM_CONTENT) return false;
    if (!Columns_FromId(f[2], r.sort)) r.sort = Columns_Builtin(COL_NAME);
    r.asc = f[3] != L"0";
    r.cols.clear();
    for (const auto& c : Split(f[4], L',')) {
        const size_t colon = c.rfind(L':');
        ColSpec spec;
        if (colon == std::wstring::npos || !Columns_FromId(c.substr(0, colon), spec)) continue;
        spec.width96 = std::clamp(_wtoi(c.c_str() + colon + 1), 1, 2000);
        r.cols.push_back(std::move(spec));
    }
    if (r.cols.empty()) r.cols = Columns_Default();
    if (f.size() == 6 && Groupable(_wtoi(f[5].c_str())) && !f[5].empty()) r.group = _wtoi(f[5].c_str());
    v = std::move(r);
    return true;
}

bool ReadView(const std::wstring& name, FolderView& v) {
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kViewsKey, name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &cb) != ERROR_SUCCESS || !cb)
        return false;
    std::wstring s(cb / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kViewsKey, name.c_str(), RRF_RT_REG_SZ, nullptr, s.data(), &cb) != ERROR_SUCCESS)
        return false;
    s.resize(wcsnlen(s.c_str(), s.size()));
    return DecodeView(s, v);
}

void WriteView(const std::wstring& name, const FolderView& v) {
    const std::wstring s = EncodeView(v);
    RegSetKeyValueW(HKEY_CURRENT_USER, kViewsKey, name.c_str(), REG_SZ, s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
}

FolderView DefaultView() {
    FolderView v;
    if (ReadView(L"Default", v)) return v;
    // Avant la mémoire par dossier, seul le mode était gardé (pour tous les dossiers).
    const DWORD mode = RegDword(L"Software\\FastExplorer", L"ViewMode", VM_DETAILS);
    v.mode = mode <= VM_CONTENT ? (int)mode : VM_DETAILS;
    v.px = std::clamp((int)RegDword(L"Software\\FastExplorer", L"IconPx", 96), 32, 256);
    return v;
}

FolderView ViewFor(const std::wstring& path) {
    FolderView v;
    if (ReadView(ViewName(path), v)) return v;
    v = DefaultView();
    if (IsVirtualPath(path)) {  // Corbeille, Accueil : en détails, les plus récents d'abord
        v.mode = VM_DETAILS;
        v.sort = Columns_Builtin(COL_DATE);
        v.asc = false;
    }
    return v;
}

FolderView CurrentView() {
    FolderView v;
    v.mode = g_viewMode;
    v.px = g_iconPx;
    v.cols = FixedColumns() ? g_folderCols : CurrentColumns();  // Ce PC, Corbeille, Accueil : colonnes fixes
    v.sort = g_sort;
    v.asc = g_sortAsc;
    v.group = g_groupKey;
    return v;
}

// Trop de dossiers mémorisés : on repart des plus récents (l'ordre des valeurs du registre suit leur création).
void PruneViews() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kViewsKey, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    DWORD n = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &n, nullptr, nullptr, nullptr, nullptr);
    if (n > kViewsMax) {
        std::vector<std::wstring> old;
        wchar_t name[2048];
        for (DWORD i = 0; i < n - kViewsMax * 4 / 5; ++i) {
            DWORD cch = 2048;
            if (RegEnumValueW(k, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (wcscmp(name, L"Default") != 0) old.emplace_back(name);
        }
        for (const auto& s : old) RegDeleteValueW(k, s.c_str());
    }
    RegCloseKey(k);
}

void SaveFolderView() {
    if (!HasView() || !g_list) return;
    const FolderView v = CurrentView();
    if (!FixedColumns()) g_folderCols = v.cols;
    const std::wstring name = ViewName(g_path);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kViewsKey, name.c_str());  // réécrite à la fin : la plus récente
    WriteView(name, v);
    static int writes = 0;
    if (++writes % 64 == 1) PruneViews();
}

void RequestSortProps();

// Applique l'affichage d'un dossier (mode, colonnes, tri) ; le tri de la vue suit.
void ApplyFolderView(const FolderView& v) {
    g_folderCols = v.cols;
    g_sort = v.sort;
    g_sortAsc = v.asc;
    g_groupKey = v.group;
    ApplyViewMode(v.mode, v.px, false);
    UpdateColumnsForView();
    UpdateSortArrow();
}

void Flash(const std::wstring& msg);

// ───────────────────────────── Type de dossier ─────────────────────────────

FolderKind g_folderKind = FK_GENERAL;  // type du dossier affiché (détecté ou choisi)

std::vector<ColSpec> KindColumns(FolderKind k) {
    auto prop = [](const PROPERTYKEY& pk, int w, bool right = false) {
        ColSpec c = Columns_Prop(pk);
        c.width96 = w;
        c.right = right;
        return c;
    };
    ColSpec name = Columns_Builtin(COL_NAME);
    switch (k) {
        case FK_MUSIC:
            name.width96 = 300;
            return {name, prop(PKEY_Music_TrackNumber, 50, true), prop(PKEY_Title, 240), prop(PKEY_Music_Artist, 200),
                    prop(PKEY_Music_AlbumTitle, 200)};
        case FK_PICTURES:
            return {name, prop(PKEY_Photo_DateTaken, 170), prop(PKEY_Image_Dimensions, 110), Columns_Builtin(COL_TYPE),
                    Columns_Builtin(COL_SIZE)};
        case FK_VIDEOS:
            return {name, Columns_Builtin(COL_DATE), Columns_Builtin(COL_TYPE), Columns_Builtin(COL_SIZE),
                    prop(PKEY_Media_Duration, 90, true)};
        default: return Columns_Default();
    }
}

// Type imposé par desktop.ini ([ViewState] FolderType), sinon d'après le contenu.
FolderKind KindOf(const DirData& d) {
    if (d.isRoot() || d.isVirtual() || d.listOnly() || !d.query.empty() || d.entries.size() > 20000) return FK_GENERAL;
    for (const Entry& e : d.entries)
        if (!e.isDir() && e.nameLen == 11 && _wcsicmp(d.name(e), L"desktop.ini") == 0) {
            wchar_t type[64] = L"";
            GetPrivateProfileStringW(L"ViewState", L"FolderType", L"", type, 64, Join(d.path, L"desktop.ini").c_str());
            if (const int k = FolderKindFromName(type); k >= 0) return (FolderKind)k;
            break;
        }
    return DetectFolderKind(d);
}

bool ReadView(const std::wstring& name, FolderView& v);

// Nouveau dossier : sans affichage mémorisé, colonnes de son type (musique, images, vidéos).
void AutoFolderKind(const DirData& d) {
    g_folderKind = KindOf(d);
    if (FixedColumns() || g_folderKind == FK_GENERAL || g_folderKind == FK_DOCUMENTS) return;
    FolderView saved;
    if (ReadView(ViewName(d.path), saved)) return;  // l'utilisateur a choisi ses colonnes
    std::vector<ColSpec> cols = KindColumns(g_folderKind);
    g_folderCols = cols;
    if (ColumnsKey(cols) != ColumnsKey(CurrentColumns())) ApplyColumns(cols);
}

// « Optimiser ce dossier pour » : colonnes du type choisi, mémorisées pour ce dossier.
void SetFolderKind(FolderKind k) {
    if (!HasView() || FixedColumns()) return;
    g_folderKind = k;
    ApplyColumns(KindColumns(k));
    SaveFolderView();
    RequestSortProps();
    if (g_viewMode == VM_TILES || g_viewMode == VM_CONTENT) InvalidateRect(g_list, nullptr, TRUE);
}

// L'affichage courant devient celui de tous les dossiers (les réglages propres à chacun sont oubliés).
void ApplyViewToAll() {
    FolderView v = CurrentView();
    v.cols = FixedColumns() ? g_folderCols : v.cols;
    RegDeleteTreeW(HKEY_CURRENT_USER, kViewsKey);
    WriteView(L"Default", v);
    Flash(L"Cet affichage s'applique maintenant à tous les dossiers");
}

// Retour à l'affichage par défaut pour ce dossier.
void ResetFolderView() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kViewsKey, ViewName(g_path).c_str());
    ApplyFolderView(DefaultView());
    RequestSortProps();
    SortView();
}
// ───────────────────────────── Choix des colonnes ─────────────────────────────

bool g_saveViewPosted = false;
void PostSaveView() {  // regroupe les changements d'une même action (ex. : ajuster toutes les colonnes)
    if (g_applyingCols || g_saveViewPosted || !HasView()) return;
    g_saveViewPosted = true;
    PostMessageW(g_hwnd, WM_APP_SAVEVIEW, 0, 0);
}

void SetFolderColumns(const std::vector<ColSpec>& cols) {
    ApplyColumns(cols);
    SaveFolderView();
    if (g_viewMode == VM_TILES || g_viewMode == VM_CONTENT) InvalidateRect(g_list, nullptr, TRUE);
}

// Ajuster toutes les colonnes à leur contenu (Ctrl+pavé numérique +).
void FitColumns() {
    if (g_viewMode != VM_DETAILS) return;
    for (int i = 0; i < (int)g_cols.size(); ++i) {
        ListView_SetColumnWidth(g_list, i, LVSCW_AUTOSIZE);
        const int content = ListView_GetColumnWidth(g_list, i);
        const int header = ListView_GetStringWidth(g_list, g_cols[i].title.c_str()) + S(24);
        ListView_SetColumnWidth(g_list, i, std::max(content, header));
    }
}

void ChooseColumns() {
    auto cols = CurrentColumns();
    if (Columns_ChooseDialog(g_hwnd, cols)) SetFolderColumns(cols);
}

// Clic droit sur l'en-tête : colonnes usuelles à cocher, « Plus… ».
void ShowHeaderMenu(POINT pt) {
    if (!HasView()) return;
    const bool root = FixedColumns();  // Ce PC, Corbeille, Accueil : colonnes fixes
    std::vector<ColSpec> items = Columns_Common();
    for (const ColSpec& c : g_cols)
        if (c.key == COL_PROP && std::none_of(items.begin(), items.end(), [&](const ColSpec& x) { return SameCol(x, c); }))
            items.push_back(c);
    constexpr UINT kFit = 900, kMore = 901, kReset = 902;
    HMENU m = CreatePopupMenu();
    if (!root) {
        AppendMenuW(m, MF_STRING | MF_CHECKED | MF_GRAYED, 0, L"Nom");
        for (size_t i = 0; i < items.size(); ++i)
            AppendMenuW(m, MF_STRING | (ColIndex(items[i]) >= 0 ? MF_CHECKED : 0), 1 + (UINT)i, items[i].title.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, kFit, L"Ajuster toutes les colonnes\tCtrl+Num +");
    if (!root) {
        AppendMenuW(m, MF_STRING, kReset, L"Colonnes par défaut");
        AppendMenuW(m, MF_STRING, kMore, L"Plus…");
    }
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == kFit) FitColumns();
    else if (cmd == kMore) ChooseColumns();
    else if (cmd == kReset) SetFolderColumns(Columns_Default());
    else if (cmd >= 1 && cmd <= items.size()) {
        auto cols = CurrentColumns();
        const ColSpec& c = items[cmd - 1];
        auto it = std::find_if(cols.begin(), cols.end(), [&](const ColSpec& x) { return SameCol(x, c); });
        if (it != cols.end()) cols.erase(it);
        else cols.push_back(c);
        SetFolderColumns(cols);
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
        const int e = EntryAt(r);  // en-tête de groupe : sauté
        if (e >= 0 && _wcsnicmp(NameOf(e), f->lvfi.psz, len) == 0) return r;
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

// Recherche par l'index du service (instantanée). false s'il ne peut pas
// répondre : pas de service, index en construction, dossier non indexé.
bool SearchIndexed(DirData& d, const std::wstring& q, const ViewPrefs& prefs, const CancelToken& ct) {
    const double t0 = NowMs();
    // Le service ne connaît que les noms : on lui envoie les mots du nom (ou
    // « *.ext » par extension demandée) ; les autres filtres s'appliquent ici.
    const SearchQuery sq(q);
    if (sq.hasContent()) return false;  // contenu : l'index des noms ne sait pas, le parcours (et Windows Search) oui
    const std::vector<std::wstring> asks = sq.NameAsks();
    if (asks.empty()) return false;  // « taille:>1Go » ou « -tmp » seuls : tous les fichiers, le parcours convient mieux
    IndexQuery iq;
    iq.scope = d.path;
    iq.showHidden = prefs.showHidden;
    iq.showSuperHidden = prefs.showSuperHidden;
    iq.max = (uint32_t)kSearchMaxResults;
    std::vector<IndexHit> hits;
    bool truncated = false;
    for (const auto& a : asks) {
        iq.query = a;
        std::vector<IndexHit> part;
        bool t = false;
        if (Index_Search(iq, part, t) != IndexStatus::Ok || ct.cancelled()) return false;
        truncated |= t;
        for (auto& h : part) hits.push_back(std::move(h));
    }
    // Plusieurs demandes (OU) : un même élément peut revenir plusieurs fois.
    if (asks.size() > 1) {
        std::unordered_set<std::wstring> seen;
        std::erase_if(hits, [&](const IndexHit& h) { return !seen.insert(h.path).second; });
    }
    // Sans taille ni date dans la requête : tout se décide sans le disque, avant de lire quoi que ce soit.
    if (!sq.needsStat())
        std::erase_if(hits, [&](const IndexHit& h) {
            return !sq.Match(h.path.c_str(), (uint32_t)h.path.size(), h.attr, 0, 0);
        });
    // L'index ne garde que les noms : taille et date lues sur le disque, pour
    // les premiers résultats (au-delà, la liste reste utilisable sans elles).
    struct Info {
        uint64_t size = 0;
        int64_t mtime = 0, ctime = 0, atime = 0;
        uint32_t attr = 0;
        bool ok = false;
    };
    std::vector<Info> info(hits.size());
    // Taille ou date demandée : il faut les lire toutes.
    const size_t statN = sq.needsStat() ? hits.size() : std::min<size_t>(hits.size(), 5000);
    const unsigned T = statN > 64 ? 8u : 1u;
    auto work = [&](unsigned t) {
        for (size_t i = statN * t / T; i < statN * (t + 1) / T && !ct.cancelled(); ++i) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (!GetFileAttributesExW(ToNtPath(Join(d.path, hits[i].path.c_str())).c_str(), GetFileExInfoStandard, &fa))
                continue;
            auto ft = [](const FILETIME& f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime); };
            info[i] = {((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, ft(fa.ftLastWriteTime), ft(fa.ftCreationTime),
                       ft(fa.ftLastAccessTime), fa.dwFileAttributes, true};
        }
    };
    std::vector<std::thread> th;
    for (unsigned t = 1; t < T; ++t) th.emplace_back(work, t);
    work(0);
    for (auto& x : th) x.join();
    if (ct.cancelled()) return false;
    d.query = q;
    for (size_t i = 0; i < hits.size(); ++i) {
        const Info& x = info[i];
        const uint32_t attr = x.ok ? x.attr : hits[i].attr;
        if (sq.needsStat() &&
            (!x.ok || !sq.Match(hits[i].path.c_str(), (uint32_t)hits[i].path.size(), attr, x.size, x.mtime)))
            continue;
        DirData_Add(d, hits[i].path.c_str(), (uint32_t)hits[i].path.size(), attr, x.size, x.mtime, x.ctime, x.atime);
    }
    d.truncated = truncated;
    d.fromIndex = true;
    d.enumMs = NowMs() - t0;
    const double t1 = NowMs();
    DirData_BuildKeys(d);
    d.keyMs = NowMs() - t1;
    d.searchDone = true;
    return true;
}

// Ce PC sans index : chaque disque local, l'un après l'autre (chemins complets).
void SearchAllDrives(DirData& d, const std::wstring& q, const ViewPrefs& prefs, const CancelToken& ct) {
    const double t0 = NowMs();
    d.query = q;
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26 && !d.truncated; ++i) {
        const wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (!(drives & (1u << i)) || GetDriveTypeW(root) != DRIVE_FIXED) continue;
        DirData one;
        one.path = root;
        SearchTree(one, q, prefs, ct);
        if (ct.cancelled()) return;
        d.scanned += one.scanned;
        for (const Entry& e : one.entries) {
            if (d.entries.size() >= kSearchMaxResults) { d.truncated = true; break; }
            const std::wstring full = Join(root, one.name(e));
            DirData_Add(d, full.c_str(), (uint32_t)full.size(), e.attr, e.size, e.mtime, e.ctime, e.atime);
        }
        d.truncated |= one.truncated;
    }
    d.enumMs = NowMs() - t0;
    DirData_BuildKeys(d);
    d.searchDone = true;
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
    // Autre dossier : son affichage mémorisé (le tri se fait déjà dans la tâche).
    if (nav != Nav::Refresh) g_pending.view = ViewFor(path);
    const int col = g_pending.view ? g_pending.view->sort.key : g_sort.key;
    const bool asc = g_pending.view ? g_pending.view->asc : g_sortAsc;
    const bool climb = nav == Nav::Refresh && q.empty();  // dossier affiché disparu : on remonte, comme l'Explorateur
    std::vector<NavFolder> pins;
    if (path == kHomePath) pins = NavPane_ShownPins();  // Accueil : l'Accès rapide déjà lu par le volet
    Jobs_Spawn([ct, path, col, asc, climb, q, prefs = g_prefs, filter = g_filter, pins = std::move(pins)] {
        auto d = std::make_shared<DirData>();
        d->path = path;
        if (path == kBinPath) {
            Recycle_Enumerate(*d, ct);
        } else if (path == kHomePath) {
            Home_Enumerate(*d, pins, ct);
        } else if (path == kNetPath) {
            Net_EnumServers(*d, ct);
        } else if (Net_IsServerPath(path)) {
            Net_EnumShares(*d, ct);  // « \\serveur » : ses partages
        } else if (std::wstring arc, inner; Archive_Split(path, arc, inner)) {
            Archive_Enumerate(*d, ct);  // ZIP : parcouru comme un dossier (pas de recherche dedans)
        } else if (!q.empty()) {
            if (SearchIndexed(*d, q, prefs, ct)) {
                // index : tout est là d'un coup
            } else if (ct.cancelled()) {
                return;
            } else if (path.empty()) {
                SearchAllDrives(*d, q, prefs, ct);
            } else {
                // Résultats au fil de l'eau : chaque copie partielle s'affiche comme un rafraîchissement.
                SearchTree(*d, q, prefs, ct, [&](std::shared_ptr<DirData> s) {
                    auto* r = new Result{ct.gen, JobKind::Navigate};
                    r->order = SortOrder(*s, col, asc, prefs, filter);
                    r->data = std::move(s);
                    Post(r);
                });
            }
        } else if (climb) EnumerateNearest(*d, ct);
        else Enumerate(*d, ct);
        if (ct.cancelled()) return;
        if (!d->error && q.empty() && !Net_ServerOf(path).empty()) Net_Remember(path);  // serveur visité : dans Réseau
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
    g_view.sorted = std::move(order);
    g_view.sortMs = ms;
    ArrangeView();
    RebuildRowOf();
    ListView_SetItemCountEx(g_list, RowCount(), LVSICF_NOSCROLL);  // change si des éléments sont masqués / affichés
    ApplySelection(sel, focus, true);
    InvalidateRect(g_list, nullptr, FALSE);
    ShowSummary();
}

void SortView();

void Resort(const ColSpec& col) {
    if (SameCol(col, g_sort)) g_sortAsc = !g_sortAsc;
    else {
        g_sort = col;
        g_sortAsc = col.key == COL_NAME || col.key == COL_TYPE || col.key == COL_ATTR || (col.key == COL_PROP && !col.right);
    }
    UpdateSortArrow();
    SaveFolderView();
    RequestSortProps();
    SortView();
}

// Tri sur une propriété : ses valeurs (copiées pour un tri en arrière-plan).
const PropValues* SortProps() { return g_sort.key == COL_PROP ? PropsOf(g_sort.pk) : nullptr; }

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
        auto order = SortOrder(*data, g_sort.key, g_sortAsc, g_prefs, g_filter, &g_view.dirBytes, SortProps());
        ApplyOrder(std::move(order), NowMs() - t);
        return;
    }
    const CancelToken ct = Jobs_NewGeneration();
    const int c = g_sort.key;
    const bool asc = g_sortAsc;
    std::optional<PropValues> props;
    if (const PropValues* pv = SortProps()) props = *pv;
    SetStatus(L"Tri…");
    Jobs_Spawn([ct, data, c, asc, prefs = g_prefs, filter = g_filter, sizes = g_view.dirBytes, props = std::move(props)] {
        const double t = NowMs();
        auto order = SortOrder(*data, c, asc, prefs, filter, &sizes, props ? &*props : nullptr);
        if (ct.cancelled()) return;
        auto* r = new Result{ct.gen, JobKind::Sort, data, std::move(order), NowMs() - t};
        Post(r);
    });
}

// « 3 éléments sélectionnés  4,52 Mo ». Une seule fois par rafale de
// changements (Ctrl+A sur 100 000 éléments : un seul parcours).
void ShowSelectionSummary() {
    g_selPosted = false;
    UpdateCommandBar();
    if (!HasView()) return SetStatusPart(SB_SEL, L"");
    size_t count = 0;
    bool anyDir = false;
    uint64_t bytes = 0;
    for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0;) {
        const int e = EntryAt(r);
        if (e < 0) continue;
        ++count;
        if (!E(e).isDir()) bytes += E(e).size;
        else if (!g_view.dirState.empty() && (g_view.dirState[e] & SIZE_KNOWN)) bytes += g_view.dirBytes[e];
        else anyDir = true;  // taille inconnue : pas de total
    }
    SetStatusPart(SB_SEL, SelectionText(count, anyDir, bytes, !g_view.data->isRoot()));
    if (Pane_Mode() != PaneMode::None) SetTimer(g_hwnd, TIMER_PANE, 60, nullptr);  // flèches maintenues : une lecture
}

// ── Volet d'aperçu / de détails ──

// Sélection (100 éléments au plus) ou, sans sélection, le dossier affiché.
void UpdatePane() {
    KillTimer(g_hwnd, TIMER_PANE);
    if (Pane_Mode() == PaneMode::None) return;
    std::vector<PaneItem> items;
    std::wstring title, text;
    if (HasView() && !g_view.data->isRoot() && !g_view.data->listOnly()) {
        for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0 && items.size() < 100;) {
            const int e = EntryAt(r);
            if (e < 0) continue;
            items.push_back({FullPath(e), E(e).attr, E(e).size, E(e).mtime});
        }
    }
    if (items.empty()) {
        if (!HasView() || g_view.data->isRoot() || InVirtual()) title = PlaceText(HasView() ? g_path : L"");
        else if (!g_query.empty()) title = L"Résultats de la recherche";
        else {
            title = g_path;
            while (title.size() > 3 && title.back() == L'\\') title.pop_back();
            if (title.size() > 3) title = PathFindFileNameW(title.c_str());
        }
        const int n = HasView() ? ListView_GetItemCount(g_list) : 0;
        text = std::to_wstring(n) + (n > 1 ? L" éléments" : L" élément");
    }
    Pane_Show(title, text, std::move(items));
}

void SavePane() {
    const DWORD mode = (DWORD)Pane_Mode(), w = (DWORD)g_paneW96;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"PaneMode", REG_DWORD, &mode, sizeof mode);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"PaneWidth", REG_DWORD, &w, sizeof w);
}

void LoadPane() {
    g_paneW96 = (int)std::clamp<DWORD>(RegDword(L"Software\\FastExplorer", L"PaneWidth", 320), 160, 2000);
    const DWORD m = RegDword(L"Software\\FastExplorer", L"PaneMode", 0);
    Pane_SetMode(m == 1 ? PaneMode::Preview : m == 2 ? PaneMode::Details : PaneMode::None);
}

void Layout();

// Alt+P : aperçu ; Alt+Maj+P : détails ; la même touche referme le volet.
void TogglePane(PaneMode m) {
    Pane_SetMode(Pane_Mode() == m ? PaneMode::None : m);
    Layout();
    UpdatePane();
    SavePane();
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

// ── Recherches récentes : liste déroulante sous la zone de recherche (comme l'Explorateur) ──

class HistoryEnum : public IEnumString {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumString) {
            *ppv = static_cast<IEnumString*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    IFACEMETHODIMP Next(ULONG celt, LPOLESTR* out, ULONG* fetched) override {
        ULONG n = 0;
        for (; n < celt && pos_ < items_.size(); ++n, ++pos_) {
            const std::wstring& s = items_[pos_];
            out[n] = (LPOLESTR)CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t));
            if (!out[n]) break;
            wcscpy_s(out[n], s.size() + 1, s.c_str());
        }
        if (fetched) *fetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG celt) override {
        pos_ = std::min(items_.size(), pos_ + celt);
        return pos_ < items_.size() ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {  // relu à chaque ouverture de la liste
        items_ = SearchHistory_Get();
        pos_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumString**) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> ref_{1};
    std::vector<std::wstring> items_;
    size_t pos_ = 0;
};

void AttachSearchHistory() {
    IAutoComplete2* ac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AutoComplete, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ac)))) return;
    auto* e = new HistoryEnum;
    e->Reset();
    if (SUCCEEDED(ac->Init(g_searchBox, e, nullptr, nullptr)))
        ac->SetOptions(ACO_AUTOSUGGEST | ACO_UPDOWNKEYDROPSLIST);  // ↓ : toute la liste
    e->Release();
    ac->Release();  // la zone de saisie garde l'objet
}

void SearchFor(const std::wstring& q) {
    SetWindowTextW(g_searchBox, q.c_str());
    g_filter.clear();
    if (HasView()) Navigate(g_path, Nav::Refresh, {}, {}, {}, false, &q);
}

// Entrée dans la zone : recherche dans les sous-dossiers (texte vide : on en sort).
void RunSearch() {
    if (!HasView()) return;
    const std::wstring q = SearchText();
    if (!q.empty()) SearchHistory_Add(q);
    const std::wstring current = g_pending.active ? g_pending.query : g_query;
    if (q == current) return;
    g_filter.clear();  // les résultats correspondent déjà au texte
    Navigate(g_path, Nav::Refresh, {}, {}, {}, false, &q);  // q vide : retour au contenu du dossier ; Ce PC : tout le PC
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
        r->order = SortOrder(d, g_sort.key, g_sortAsc, g_prefs, g_filter, nullptr, SortProps());

    std::wstring d_errorNote;
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
        // Partage ou serveur protégé : Windows demande les identifiants, puis on réessaie.
        if (!Net_ServerOf(d.path).empty() && Net_IsAuthError(d.error) && !p.credsAsked) {
            const DWORD c = Net_Connect(g_hwnd, d.path);
            if (c == NO_ERROR) {
                Navigate(p.path, p.nav, p.focus, p.selected);
                g_pending.credsAsked = true;
                g_pending.typed = p.typed;
                return;
            }
            if (c != ERROR_CANCELLED) d_errorNote = Net_ErrorText(c);
        }
        SetStatus(L"Impossible d'ouvrir « " + PlaceText(d.path) + L" » : " +
                  (Net_ServerOf(d.path).empty() ? ErrorText(d.error) : Net_ErrorText(d.error)) +
                  (d_errorNote.empty() ? L"" : L" (" + d_errorNote + L")"));
        if (!HasView()) Navigate(L"", Nav::Normal);  // démarrage raté → Ce PC
        return;
    }

    if (!p.typed.empty()) Places_RememberTyped(p.typed);

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
    g_frozen = freeze;  // les changements d'affichage ne relâchent pas le gel

    // Icônes : préréglées (lecteurs), sinon à résoudre à l'affichage. Sur un
    // rafraîchissement du même dossier, on reprend celles déjà connues (pas
    // de clignotement générique → propre, pas de nouvelle demande).
    const size_t n = d.entries.size();
    std::vector<int32_t> icons(n, -1);
    std::vector<uint8_t> overlays(n, 0), states(n, ICON_NONE);
    std::vector<int32_t> thumbs(n, THUMB_UNKNOWN);
    std::deque<std::pair<PROPERTYKEY, PropValues>> props;  // rafraîchissement : valeurs déjà lues reprises
    for (size_t i = 0; i < n; ++i)
        if (d.entries[i].icon >= 0) { icons[i] = d.entries[i].icon; states[i] = ICON_OWN; }
    if (refresh && HasView() && g_view.data->path == d.path) {
        const DirData& old = *g_view.data;
        for (const auto& [k, v] : g_view.props) {
            props.emplace_back(k, PropValues{});
            props.back().second.Reset(n);
        }
        std::unordered_map<std::wstring_view, int> oldIdx;
        oldIdx.reserve(old.entries.size());
        for (size_t i = 0; i < old.entries.size(); ++i)
            oldIdx.emplace(std::wstring_view(old.name(old.entries[i]), old.entries[i].nameLen), (int)i);
        for (size_t j = 0; j < n; ++j) {
            auto it = oldIdx.find(std::wstring_view(d.name(d.entries[j]), d.entries[j].nameLen));
            if (it == oldIdx.end()) continue;
            if ((size_t)it->second < g_view.thumb.size() && g_view.thumb[it->second] >= 0)
                thumbs[j] = g_view.thumb[it->second];
            for (size_t k = 0; k < props.size(); ++k) {
                const PropValues& o = g_view.props[k].second;
                if (o.state[it->second] != 2) continue;
                props[k].second.text[j] = o.text[it->second];
                props[k].second.num[j] = o.num[it->second];
                props[k].second.state[j] = 2;
            }
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
    g_view.thumb = std::move(thumbs);
    g_view.props = std::move(props);
    g_view.serial = ++g_viewSerial;
    Icons_SetSerial(g_view.serial);
    Thumbs_SetSerial(g_view.serial);
    Props_SetSerial(g_view.serial);
    if (!refresh) TrimBigList();  // miniatures du dossier précédent : place libérée au besoin
    InitDirSizes();
    if (g_sort.key == COL_SIZE && d.entries.size() <= kSyncSortMax &&
        std::any_of(g_view.dirState.begin(), g_view.dirState.end(), [](uint8_t s) { return s != 0; }))
        g_view.order = SortOrder(d, g_sort.key, g_sortAsc, g_prefs, g_filter, &g_view.dirBytes);
    if (g_sort.key == COL_PROP && d.entries.size() <= kSyncSortMax && SortProps())
        g_view.order = SortOrder(d, g_sort.key, g_sortAsc, g_prefs, g_filter, nullptr, SortProps());  // valeurs reprises
    if (_wcsicmp(g_path.c_str(), d.path.c_str()) != 0 || g_query != d.query) {  // autre dossier : filtres oubliés
        g_colFilter.clear();
        g_collapsed.clear();
    }
    g_view.sorted = std::move(g_view.order);
    ArrangeView();
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
    RequestDirSizes();
    if (p.view) ApplyFolderView(*p.view);  // autre dossier : son affichage
    else UpdateColumnsForView();
    if (!refresh) AutoFolderKind(d);
    ArrangeView();  // regroupement de ce dossier (son affichage vient d'être appliqué)
    RebuildRowOf();
    UpdateSortArrow();
    RequestSortProps();
    if (Pane_Mode() != PaneMode::None) SetTimer(g_hwnd, TIMER_PANE, 30, nullptr);  // dossier changé, ou fichier modifié
    // Surveillance du dossier (pas de « Ce PC » : les lecteurs viendront avec le bloc 4).
    if (g_path.empty() || IsVirtualPath(g_path) || d.listOnly()) {
        Watch_Stop();
        g_watchPath.clear();
        g_watchFailed.clear();
    } else if (g_watchPath != g_path && g_watchFailed != g_path) {
        g_watchPath = g_path;
        g_watchFailed.clear();
        Watch_Start(g_hwnd, WM_APP_DIRCHANGED, g_path, ++g_watchId);
    }

    SetWindowTextW(g_addr, PlaceText(g_path).c_str());
    Crumbs_SetPath(g_path);
    SetWindowTextW(g_hwnd, (L"FastExplorer — " + PlaceText(g_path)).c_str());
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

    g_frozen = false;
    if (freeze) SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    UpdateButtons();
    SyncTab();
    if (g_startupMs < 0) g_startupMs = MsSinceProcessStart();  // premier affichage réellement peint
    NavPane_ShowPath(g_path);
    NavPane_FolderListed(d);  // sous-dossiers affichés dans le volet : à jour sans relecture
    ShellMenu_Discard();  // contenu du dossier relu : le menu préparé peut ne plus être exact
    if (!d.vanished.empty()) g_goneNote = L"« " + d.vanished + L" » n'est plus accessible";
    else if (p.nav != Nav::Refresh) g_goneNote.clear();
    ShowSummary();
    if (!g_path.empty() && !d.isVirtual() && !d.listOnly()) WarmUpMenus(d);
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
    // Double panneau de la session précédente : une fois la liste remplie (elle devient le panneau inactif).
    if (g_dualAtStart && HasView() && !g_pending.active) {
        g_dualAtStart = false;
        SetDual(true);
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
    if (path.empty() || IsVirtualPath(path)) return PlaceText(path);
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
    if (HasView() && IsVirtualPath(g_path)) Navigate(L"", Nav::Normal);  // Corbeille, Accueil : vers Ce PC
    else if (HasView() && ParentOf(g_path, parent, child)) Navigate(parent, Nav::Normal, child);
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
        SetWindowTextW(g_addr, PlaceText(g_path).c_str());
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
    SetWindowTextW(g_addr, PlaceText(g_path).c_str());
}

void NavigateAddress() {
    const int n = GetWindowTextLengthW(g_addr);
    std::wstring t(n + 1, L'\0');
    GetWindowTextW(g_addr, t.data(), n + 1);
    t.resize(n);
    std::wstring trimmed = t;
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(0, 1);
    if (const std::wstring place = Places_FromAlias(trimmed); !place.empty()) {  // « corbeille », « shell:home »…
        Navigate(place, Nav::Normal);
        g_pending.typed = trimmed;
        SetFocus(g_list);
        return;
    }
    if (_wcsicmp(trimmed.c_str(), L"Ce PC") == 0 || _wcsicmp(trimmed.c_str(), L"shell:MyComputerFolder") == 0) {
        Navigate(L"", Nav::Normal);
        g_pending.typed = trimmed;
        SetFocus(g_list);
        return;
    }
    // « shell:downloads », « shell:startup »… : dossiers connus de Windows.
    if (std::wstring known; const int sc = Places_ShellCommand(trimmed, known)) {
        if (sc == 1) {
            Navigate(known, Nav::Normal);
            g_pending.typed = trimmed;
            SetFocus(g_list);
        } else if (sc == -2) {  // sans chemin (Applications, Panneau de configuration…) : Windows l'ouvre
            ShellExecuteW(g_hwnd, nullptr, trimmed.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Places_RememberTyped(trimmed);
            SetStatus(L"« " + trimmed + L" » n'est pas un dossier de fichiers : ouvert par Windows.");
            SetFocus(g_list);
        } else {
            SetStatus(L"Commande inconnue : « " + trimmed + L" » (exemples : shell:downloads, shell:startup, shell:sendto).");
        }
        return;
    }
    const std::wstring path = NormalizePath(t, g_path);
    Navigate(path, Nav::Normal, {}, {}, path);  // un fichier tapé dans la barre s'ouvre
    g_pending.typed = trimmed;
    // « cmd », « wt », « notepad x.txt », « code . » : lancés dans le dossier
    // affiché si ce n'est pas un dossier, comme dans l'Explorateur.
    std::wstring cmd = t;
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    while (!cmd.empty() && iswspace(cmd.front())) cmd.erase(0, 1);
    const std::wstring first = cmd.substr(0, std::min(cmd.find(L' '), cmd.size()));
    if (!first.empty() && first.find_first_of(L"\\/:") == std::wstring::npos) g_pending.command = cmd;
    SetFocus(g_list);
}

// F4 : chemins tapés récemment, sous la barre d'adresse.
void ShowTypedHistory() {
    BeginAddressEdit();
    const std::vector<std::wstring> typed = Places_Typed();
    if (typed.empty()) return;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < typed.size(); ++i) {
        std::wstring text;
        for (wchar_t c : typed[i]) text += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c);
        AppendMenuW(m, MF_STRING, i + 1, text.c_str());
    }
    RECT r;
    GetWindowRect(g_addr, &r);
    const UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= typed.size()) {
        SetWindowTextW(g_addr, typed[cmd - 1].c_str());
        NavigateAddress();
    } else {
        SetFocus(g_addr);
    }
}

// F11 : plein écran (sans cadre, sur tout l'écran), et retour.
WINDOWPLACEMENT g_fsPlace{sizeof(WINDOWPLACEMENT)};
bool g_fullscreen = false;
void ToggleFullscreen() {
    const LONG style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(g_hwnd, &g_fsPlace) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongW(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_fsPlace);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

bool IsLnk(int idx) {
    const Entry& e = E(idx);
    return e.nameLen - e.extOff == 4 && _wcsicmp(NameOf(idx) + e.extOff, L".lnk") == 0;
}

// ───────────────────────────── Archives ─────────────────────────────
//
// Un ZIP se parcourt comme un dossier (« C:\x\a.zip\sous-dossier »), en lecture
// seule. Ouvrir, copier ou glisser un élément l'extrait d'abord dans un dossier
// temporaire propre à l'archive ; F5 / « Extraire vers… » l'extraient ailleurs.

void StartOp(FileOp op);
std::vector<std::wstring> NamesOf(const std::vector<int>& entries);

bool InArchive() { return HasView() && g_view.data->inArchive(); }

// Dossier interne affiché (« sous\dossier », "" à la racine de l'archive).
std::wstring ArchiveInner() {
    const DirData& d = *g_view.data;
    return d.path.size() > d.archive.size() + 1 ? d.path.substr(d.archive.size() + 1) : std::wstring();
}

// Dossier temporaire des éléments ouverts depuis `archive` (un par archive).
std::wstring ArchiveTemp(const std::wstring& archive) {
    wchar_t tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    uint32_t h = 2166136261u;  // FNV-1a du chemin, sans la casse
    for (wchar_t c : archive) {
        h ^= (uint32_t)towlower(c);
        h *= 16777619u;
    }
    wchar_t hex[16];
    swprintf_s(hex, L"%08x", h);
    std::wstring parent, leaf;
    if (!ParentOf(archive, parent, leaf)) leaf = L"archive";
    return (n ? std::wstring(tmp, n) : std::wstring(L"C:\\Windows\\Temp\\")) + L"FastExplorer\\Archives\\" +
           Archive_Stem(leaf) + L"-" + hex;
}

// Sélection (sinon l'élément ciblé) de la vue d'archive.
std::vector<int> ArchivePicks() {
    std::vector<int> sel = SelectedEntries();
    if (sel.empty())
        if (const int f = FocusedEntry(); f >= 0) sel.push_back(f);
    return sel;
}

// Extrait des éléments de l'archive affichée. dest vide : dossier temporaire (remplacé).
void ExtractEntries(const std::vector<int>& entries, ThenDo then, const std::wstring& dest = {}) {
    if (!InArchive() || entries.empty()) return;
    FileOp op;
    op.kind = FileOpKind::Extract;
    op.paths = {g_view.data->archive};
    op.innerBase = ArchiveInner();
    op.inner = NamesOf(entries);
    op.then = then;
    op.overwrite = dest.empty();
    op.folder = dest.empty() ? ArchiveTemp(g_view.data->archive) : dest;
    StartOp(std::move(op));
}

// Toute l'archive. newFolder : dans « dest\nom de l'archive » (« Extraire tout »).
void ExtractArchive(const std::wstring& archive, const std::wstring& dest, bool newFolder, ThenDo then) {
    FileOp op;
    op.kind = FileOpKind::Extract;
    op.paths = {archive};
    op.folder = dest;
    op.newFolder = newFolder;
    op.then = then;
    StartOp(std::move(op));
    std::wstring parent, leaf;
    if (!ParentOf(archive, parent, leaf)) leaf = archive;
    Flash(L"Extraction de « " + leaf + L" »…");
}

// Choix d'un dossier (« Extraire vers… »). Vide : annulé.
std::wstring PickFolder(const std::wstring& initial, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    IShellItem* f = nullptr;
    if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
        dlg->SetFolder(f);
        f->Release();
    }
    std::wstring out;
    IShellItem* r = nullptr;
    if (dlg->Show(g_hwnd) == S_OK && SUCCEEDED(dlg->GetResult(&r))) {
        PWSTR s = nullptr;
        if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) out = s;
        CoTaskMemFree(s);
        r->Release();
    }
    dlg->Release();
    return out;
}

// « Compresser dans un fichier ZIP » : à côté des éléments, nommé d'après le premier.
void CompressEntries(const std::vector<int>& entries) {
    if (entries.empty() || !HasView() || g_view.data->isRoot() || g_view.data->isVirtual() || g_view.data->inArchive())
        return;
    FileOp op;
    op.kind = FileOpKind::Compress;
    op.folder = g_path;
    for (int e : entries) op.paths.push_back(Join(g_path, NameOf(e)));
    std::wstring stem = NameOf(entries[0]);
    if (!E(entries[0]).isDir())
        if (const size_t dot = stem.rfind(L'.'); dot != std::wstring::npos && dot > 0) stem.resize(dot);
    op.newName = stem + L".zip";
    StartOp(std::move(op));
    Flash(L"Compression dans « " + stem + L".zip »…");
}

bool BrokenShortcut(const std::wstring& lnk, const std::wstring& target);

void ActivateEntry(int idx) {
    if (idx < 0) return;
    if (InArchive() && !E(idx).isDir()) {  // extrait (dossier temporaire) puis ouvert
        ExtractEntries({idx}, ThenDo::Open);
        return;
    }
    if (g_view.data->path == kBinPath) {  // comme l'Explorateur : un élément supprimé ne s'ouvre pas
        Flash(L"Restaurez l'élément pour l'ouvrir (clic droit ▸ Restaurer)");
        return;
    }
    const std::wstring full = FullPath(idx);
    if (E(idx).isDir()) {
        Navigate(full, Nav::Normal);
        return;
    }
    // Raccourci : on navigue DANS FastExplorer si la cible est un dossier
    // (le Shell ouvrirait l'Explorateur Windows). Sinon le Shell l'ouvre,
    // y compris pour une cible disparue (il propose de la rechercher).
    if (!InArchive() && Archive_IsZip(NameOf(idx))) {  // ZIP : on entre dedans (illisible : le Shell l'ouvre)
        Navigate(full, Nav::Normal, {}, {}, full, true);
        return;
    }
    std::wstring target;
    if (IsLnk(idx) && ResolveShortcut(full, target)) {
        if (BrokenShortcut(full, target)) return;  // cible disparue : retrouver, chercher ou supprimer
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

bool CanModify() {
    return HasView() && !g_view.data->isRoot() && !g_view.data->isVirtual() && !g_view.data->listOnly();
}

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
    const bool fast = FileOps_Progress(p);
    // Une copie vient se mettre en file derrière une autre : la file s'affiche (sans prendre le focus).
    static uint64_t shownFor = 0;
    for (const FileOpJob& j : FileOps_Jobs())
        if (!j.running && j.id > shownFor) {
            shownFor = j.id;
            CopyQueue_Show(g_hwnd, false);
        }
    if (!fast) {
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
    std::wstring s = p.label ? std::wstring(p.label) + L" : " : p.move ? L"Déplacement : " : L"Copie : ";
    s += std::to_wstring(p.filesDone) + L" / " + std::to_wstring(p.filesTotal) + L" fichiers";
    if (bytes) s += L"   •   " + SizeText(p.bytesDone) + L" / " + SizeText(p.bytesTotal);
    if (total) s += L"   •   " + std::to_wstring(done * 100 / total) + L" %";
    if (p.waiting) s += L"   •   " + std::to_wstring(p.waiting) + L" en attente";
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
    bool file = false;                   // création : un fichier (pas un dossier)
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
        case FileOpKind::Rename:
            return s.from.size() > 1 ? count(L"renommage") : L"renommage de « " + LeafOf(s.from[0]) + L" »";
        case FileOpKind::NewFolder: return L"création de « " + LeafOf(s.to[0]) + L" »";
        case FileOpKind::Link:
            return s.to.size() == 1 ? L"création de « " + LeafOf(s.to[0]) + L" »"
                                    : L"création de " + std::to_wstring(s.to.size()) + L" raccourcis";
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
            if (!r.renamedFrom.empty()) {  // plusieurs éléments (même en partie : ceux qui ont changé)
                for (size_t i = 0; i < r.renamedFrom.size() && i < r.created.size(); ++i) {
                    s.from.push_back(r.renamedFrom[i]);
                    s.to.push_back(Join(ParentPath(r.renamedFrom[i]), r.created[i].c_str()));
                }
                return true;
            }
            if (!ok || r.sources.size() != 1 || r.newName.empty()) return false;
            s.from = {r.sources[0]};
            s.to = {Join(ParentPath(r.sources[0]), r.newName.c_str())};
            return _wcsicmp(s.from[0].c_str(), s.to[0].c_str()) != 0 || s.from[0] != s.to[0];
        case FileOpKind::NewFolder:
            if (r.created.empty()) return false;
            s.to = {Join(r.folder, r.created[0].c_str())};
            s.file = r.newFile;
            return true;
        case FileOpKind::Link:
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
            if (s.from.size() > 1) {  // renommage multiple : une seule opération
                FileOp* op = add(FileOpKind::Rename, ParentPath(s.from[0]), undo ? s.to : s.from);
                for (const auto& p : undo ? s.from : s.to) op->newNames.push_back(LeafOf(p));
                break;
            }
            const std::wstring& src = undo ? s.to[0] : s.from[0];
            add(FileOpKind::Rename, ParentPath(src), {src})->newName = LeafOf(undo ? s.from[0] : s.to[0]);
            break;
        }
        case FileOpKind::NewFolder:
            if (undo) add(FileOpKind::Delete, ParentPath(s.to[0]), s.to);
            else {
                FileOp* op = add(FileOpKind::NewFolder, ParentPath(s.to[0]), {});
                op->newName = LeafOf(s.to[0]);
                op->newFile = s.file;
            }
            break;
        case FileOpKind::Copy:
        case FileOpKind::Link:
            if (undo) add(FileOpKind::Delete, ParentPath(s.to[0]), s.to);
            else add(s.kind, ParentPath(s.to[0]), s.from);
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

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many);

void OnFileOp(FileOpResult* raw) {
    std::unique_ptr<FileOpResult> r(raw);
    RecordUndo(*r);
    if (g_dual && !g_passive.path.empty()) {  // l'autre panneau est touché (copie vers lui, déplacement depuis lui)
        bool touched = _wcsicmp(r->folder.c_str(), g_passive.path.c_str()) == 0;
        for (const auto& s : r->sources) {
            std::wstring parent, leaf;
            touched = touched || (ParentOf(s, parent, leaf) && _wcsicmp(parent.c_str(), g_passive.path.c_str()) == 0);
        }
        if (touched) ReloadPassive();
    }
    // Tailles des dossiers touchés périmées (le dossier affiché ne change pas
    // forcément : copier dans un sous-dossier ne le modifie pas).
    ForgetSizes(r->folder);
    for (const std::wstring& s : r->sources) ForgetSizes(s);
    if (HasView() && !g_pending.active) RequestDirSizes();
    if (FileOps_IsFailure(*r))
        SetStatus(L"Opération impossible : " + ErrorText((DWORD)r->hr) + (r->detail.empty() ? L"" : L" (" + r->detail + L")"));
    else if ((r->kind == FileOpKind::Copy || r->kind == FileOpKind::Move) && !r->aborted) {
        std::wstring note;
        if (r->verifyFailed)
            note = Plural(r->verifyFailed, L"copie différente", L"copies différentes") + L" de l'original, refaite(s) par Windows";
        else if (r->verified && !r->created.empty())
            note = L"Copie vérifiée : identique à l'original";
        if (r->retried)
            note += (note.empty() ? L"" : L" ; ") + Plural(r->retried, L"fichier repris", L"fichiers repris") +
                    L" après une erreur passagère";
        if (!note.empty()) Flash(note);
    }
    if (r->clipboardCut && SUCCEEDED(r->hr) && !r->aborted) Clipboard_ClearAfterMove(r->clipSeq);
    if (g_quitWhenIdle) {
        if (FileOps_Running() == 0) DestroyWindow(g_hwnd);
        return;
    }
    const auto& c = r->created;
    // Corbeille, Accueil : pas de surveillance du dossier, on relit après chaque opération.
    if (IsVirtualPath(r->folder) || (r->kind == FileOpKind::Delete && !r->permanent)) {
        if (HasView() && InVirtual() && !g_pending.active) Refresh();
        if (r->kind == FileOpKind::RestoreBin && SUCCEEDED(r->hr) && !c.empty())
            Flash(c.size() == 1 ? L"Restauré : " + c[0] : std::to_wstring(c.size()) + L" éléments restaurés");
        if (IsVirtualPath(r->folder)) return;
    }
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
        case FileOpKind::Link:
            if (!c.empty()) AfterOpSelect(r->folder, c, c[0]);  // sinon la surveillance suffit
            break;
        case FileOpKind::Compress:
            if (!c.empty()) AfterOpSelect(r->folder, {c[0]}, c[0], c[0]);  // nom proposé, à modifier aussitôt
            break;
        case FileOpKind::Extract: {
            if (FileOps_IsFailure(*r) || r->aborted || c.empty()) break;
            const std::wstring first = Join(r->folder, c[0].c_str());
            switch (r->then) {
                case ThenDo::Open: {
                    const DWORD a = GetFileAttributesW(first.c_str());
                    if (Archive_IsZip(c[0]) || (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)))
                        Navigate(first, Nav::Normal);  // ZIP dans le ZIP : parcouru aussi
                    else
                        OpenWithShell(first);
                    break;
                }
                case ThenDo::Clipboard: {
                    std::vector<std::wstring> full;
                    for (const auto& n : c) full.push_back(Join(r->folder, n.c_str()));
                    if (Clipboard_Put(g_hwnd, L"", full, false))
                        SetStatus(std::to_wstring(full.size()) + (full.size() > 1 ? L" éléments copiés" : L" élément copié") +
                                  L" dans le presse-papiers.");
                    else
                        SetStatus(L"Impossible d'utiliser le presse-papiers.");
                    break;
                }
                case ThenDo::Show:
                    Navigate(first, Nav::Normal);
                    SetFocus(g_list);
                    break;
                default:
                    if (HasView() && _wcsicmp(g_path.c_str(), r->folder.c_str()) == 0) AfterOpSelect(r->folder, c, c[0]);
                    else Flash(L"Extrait dans " + r->folder);
                    break;
            }
            break;
        }
        default: break;
    }
}

// ── Corbeille et Accueil : les actions portent sur les entrées (les noms affichés peuvent se répéter) ──

std::vector<std::wstring> TargetsOf(const std::vector<int>& entries) {
    std::vector<std::wstring> v;
    for (int e : entries)
        if (std::wstring p = FullPath(e); !p.empty()) v.push_back(std::move(p));
    return v;
}

bool InBin() { return HasView() && g_view.data->path == kBinPath; }

void BinRestore(bool all) {
    if (!InBin()) return;
    std::vector<int> sel;
    if (all)
        for (uint32_t i : g_view.order)
            if (!IsGroupRow(i)) sel.push_back((int)i);
    else
        sel = SelectedEntries();
    if (sel.empty()) return;
    FileOp op;
    op.kind = FileOpKind::RestoreBin;
    op.folder = kBinPath;
    op.paths = TargetsOf(sel);
    StartOp(std::move(op));
}

void BinPurge() {
    if (!InBin()) return;
    const auto sel = SelectedEntries();
    if (sel.empty() || !ConfirmDelete(g_hwnd, NamesOf(sel), true)) return;
    FileOp op;
    op.kind = FileOpKind::Delete;
    op.folder = kBinPath;
    op.permanent = true;
    for (const auto& r : TargetsOf(sel)) {
        op.paths.push_back(r);
        if (std::wstring i = Recycle_InfoOf(r); !i.empty()) op.paths.push_back(std::move(i));
    }
    StartOp(std::move(op));
}

void BinEmpty() {
    if (!InBin() || g_view.data->entries.empty()) {
        Flash(L"La Corbeille est vide.");
        return;
    }
    FileOp op;
    op.kind = FileOpKind::EmptyBin;
    op.folder = kBinPath;
    StartOp(std::move(op));
}

void CopyNames(const std::vector<std::wstring>& names, bool cut) {
    if (HasView() && (g_view.data->path == kNetPath || g_view.data->server)) return;  // serveurs, partages : rien à copier
    if (InArchive()) {  // extraits dans le dossier temporaire, puis mis dans le presse-papiers
        if (cut) Flash(L"Archive en lecture seule : copiez (Ctrl+C) puis collez ailleurs");
        else ExtractEntries(ArchivePicks(), ThenDo::Clipboard);
        return;
    }
    if (InVirtual() && !InBin() && !cut) {  // Accueil : copie des fichiers eux-mêmes (dossiers différents)
        const auto t = TargetsOf(SelectedEntries());
        if (!t.empty() && Clipboard_Put(g_hwnd, L"", t, false))
            SetStatus(std::to_wstring(t.size()) + (t.size() > 1 ? L" éléments copiés" : L" élément copié") +
                      L" dans le presse-papiers.");
        return;
    }
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
    if (InVirtual()) {  // Accueil (Corbeille : chemins d'origine)
        for (int e : SelectedEntries())
            full.push_back(InBin() && (size_t)e < g_view.data->origin.size() ? Join(g_view.data->origin[e], NameOf(e)) : FullPath(e));
        if (full.empty()) return;
    } else if (names.empty()) {
        if (g_path.empty()) return;
        full.push_back(g_path);
    }
    if (!InVirtual())
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
    UpdateCommandBar();
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

void NetForget(const std::vector<int>& sel);

// Supprime ; la cible passe ensuite à l'élément suivant (ou précédent), comme dans l'Explorateur.
void DeleteNames(const std::vector<std::wstring>& names, bool permanent) {
    if (InBin()) {  // dans la Corbeille, supprimer = définitivement
        BinPurge();
        return;
    }
    if (HasView() && g_view.data->path == kNetPath) {  // Réseau : retirer de la liste
        NetForget(SelectedEntries());
        return;
    }
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

void NewFolder(bool file = false) {
    if (!CanModify()) return;
    FileOp op;
    op.kind = FileOpKind::NewFolder;
    op.folder = g_path;
    op.newName = file ? L"Nouveau document texte.txt" : L"Nouveau dossier";
    op.newFile = file;
    StartOp(std::move(op));
}

// Renommage multiple (F2 sur plusieurs éléments) : noms concernés, dans l'ordre affiché.
std::vector<std::wstring> g_multiRename;
bool g_inBeginRename = false;  // la saisie s'ouvre depuis BeginRename (sinon : pas de renommage multiple)

void BeginRename() {
    if (!CanModify()) return;
    int row = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (row < 0) row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (row < 0) return;
    g_multiRename.clear();
    if (ListView_GetSelectedCount(g_list) > 1)
        for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0;)
            if (const int e = EntryAt(r); e >= 0) g_multiRename.emplace_back(NameOf(e));
    SetFocus(g_list);
    g_inBeginRename = true;
    const HWND ed = ListView_EditLabel(g_list, row);
    g_inBeginRename = false;
    if (!ed) {  // saisie refusée (fenêtre inactive, dossier en cours de lecture)
        g_multiRename.clear();
        return;
    }
    if (g_multiRename.size() > 1)
        SetStatus(L"Renommer " + std::to_wstring(g_multiRename.size()) +
                  L" éléments → « nom (1) », « nom (2) »…");
}

// Noms d'un renommage multiple : « base (1).jpg », « base (2).png »… (dossiers : sans extension).
// base : texte saisi, sans l'extension de l'élément édité s'il la contenait.
std::vector<std::wstring> MultiRenameNames(const std::wstring& typed, const std::wstring& edited,
                                           const std::vector<std::wstring>& names, const std::vector<bool>& dirs) {
    std::wstring base = typed;
    const wchar_t* ext = PathFindExtensionW(edited.c_str());
    const size_t el = wcslen(ext);
    if (el > 1 && base.size() > el && _wcsicmp(base.c_str() + base.size() - el, ext) == 0) base.resize(base.size() - el);
    std::vector<std::wstring> out;
    for (size_t i = 0; i < names.size(); ++i) {
        const size_t slash = names[i].rfind(L'\\');  // résultat de recherche : le nom seul
        const std::wstring leaf = slash == std::wstring::npos ? names[i] : names[i].substr(slash + 1);
        const std::wstring e = dirs[i] ? std::wstring() : std::wstring(PathFindExtensionW(leaf.c_str()));
        out.push_back(base + L" (" + std::to_wstring(i + 1) + L")" + e);
    }
    return out;
}

BOOL OnBeginLabelEdit(const NMLVDISPINFOW* di) {
    const int idx = EntryAt(di->item.iItem);
    if (!CanModify() || idx < 0 || g_pending.active) return TRUE;  // refusé
    if (!g_inBeginRename) g_multiRename.clear();  // nouveau dossier, clic sur le nom : un seul élément
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
    std::vector<std::wstring> multi = std::move(g_multiRename);
    g_multiRename.clear();
    if (!di->item.pszText || old.empty()) {  // Échap
        if (multi.size() > 1) ShowSummary();
        return;
    }
    const std::wstring typed = di->item.pszText;
    if (multi.size() > 1) {  // F2 sur plusieurs éléments : tous renommés d'après ce nom
        const std::wstring base = CleanFileName(typed);
        if (base == old || base + g_editExt == old) {  // validé sans changement : rien à renommer
            ShowSummary();
            return;
        }
        if (const wchar_t* err = FileNameError(base.empty() ? std::wstring() : base + L" (1)")) {
            g_reedit = Reedit{old, typed, err};
            g_multiRename = std::move(multi);  // la saisie rouverte reste un renommage multiple
            PostMessageW(g_hwnd, WM_APP_REEDIT, 0, 0);
            return;
        }
        std::vector<bool> dirs;
        FileOp op;
        op.kind = FileOpKind::Rename;
        op.folder = g_path;
        std::vector<std::wstring> kept;
        for (const auto& n : multi) {
            const int e = FindEntryByName(n);
            if (e < 0) continue;  // disparu entre-temps
            kept.push_back(n);
            dirs.push_back(E(e).isDir());
            op.paths.push_back(Join(g_path, n.c_str()));
        }
        // L'élément édité en premier, puis les autres dans l'ordre affiché (comme l'Explorateur).
        if (auto it = std::find(kept.begin(), kept.end(), old); it != kept.end()) {
            const size_t k = (size_t)(it - kept.begin());
            std::rotate(kept.begin(), kept.begin() + (ptrdiff_t)k, kept.begin() + (ptrdiff_t)k + 1);
            std::rotate(op.paths.begin(), op.paths.begin() + (ptrdiff_t)k, op.paths.begin() + (ptrdiff_t)k + 1);
            const bool d = dirs[k];
            dirs.erase(dirs.begin() + (ptrdiff_t)k);
            dirs.insert(dirs.begin(), d);
        }
        if (kept.empty()) return;
        op.newNames = MultiRenameNames(base, old, kept, dirs);
        StartOp(std::move(op));
        return;
    }
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
    if (!HasView() || g_view.data->path == kNetPath || g_view.data->server) return;  // serveurs, partages entiers
    if (InArchive()) {  // extraction immédiate dans le dossier temporaire, puis glisser de ces fichiers
        const auto sel = SelectedEntries();
        if (sel.empty()) return;
        const std::wstring archive = g_view.data->archive, tmp = ArchiveTemp(archive);
        ExtractOptions o;
        o.base = ArchiveInner();
        o.items = NamesOf(sel);
        o.overwrite = true;
        if (Archive_SizeOf(archive, o) > 200ull << 20) {
            Flash(L"Trop volumineux pour un glisser : utilisez « Extraire vers… » (clic droit)");
            return;
        }
        FastProgress prog;
        std::vector<std::wstring> created;
        std::wstring detail;
        const HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        const DWORD e = Archive_Extract(archive, tmp, o, prog, &created, &detail);
        SetCursor(old);
        if (e || created.empty()) {
            SetStatus(L"Extraction impossible : " + ErrorText(e ? e : ERROR_FILE_NOT_FOUND) +
                      (detail.empty() ? L"" : L" (" + detail + L")"));
            return;
        }
        DragDrop_Start(g_list, tmp, created, client);
        ShellMenu_Discard();
        return;
    }
    if (InVirtual()) {  // Accueil : les vrais fichiers (copie) ; Corbeille : rien à glisser
        if (InBin()) return;
        const auto t = TargetsOf(SelectedEntries());
        if (!t.empty()) DragDrop_Start(g_list, L"", t, client);
        ShellMenu_Discard();
        return;
    }
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

void RegisterDropOn(int k) {
    DropHost h;
    // Liste inactive (double panneau) : dépôt dans son dossier, ou dans un de ses sous-dossiers.
    h.folder = [k] {
        const std::wstring& p = k == g_side ? g_path : g_passive.path;
        const View& v = k == g_side ? g_view : g_passive.view;
        if (v.data && v.data->listOnly()) return std::wstring();  // archive, serveur : lecture seule
        return IsVirtualPath(p) ? std::wstring() : p;  // Corbeille, Accueil : pas de dépôt
    };
    h.hit = [k](POINT c, std::wstring& name) {
        if (k != g_side) {
            const View& v = g_passive.view;
            if (!g_dual || !v.data || !g_passive.query.empty() || v.data->isVirtual() || v.data->listOnly()) return -1;
            LVHITTESTINFO hit{};
            hit.pt = c;
            const int row = ListView_HitTest(g_lists[k], &hit);
            if (row < 0 || row >= (int)v.order.size()) return -1;
            name = v.data->name(v.data->entries[v.order[row]]);
            return row;
        }
        if (!HasView() || g_pending.active || InVirtual()) return -1;
        LVHITTESTINFO hit{};
        hit.pt = c;
        const int row = ListView_HitTest(g_list, &hit);
        const int e = EntryAt(row);
        if (e < 0) return -1;
        name = NameOf(e);
        return row;
    };
    h.dragged = [k](const std::wstring& n) { return k == g_side && g_dragNames.count(n) > 0; };
    h.spring = [k](int row, const std::wstring&) {  // dossier survolé : on y entre
        if (k != g_side || !HasView() || g_pending.active) return false;
        const int e = EntryAt(row);
        if (e < 0 || !E(e).isDir()) return false;
        Navigate(FullPath(e), Nav::Normal);
        return true;
    };
    h.highlight = [k](int row) {
        if (k == g_side) SetDropRow(row);
    };
    h.dropped = [k](bool intoFolder) {
        if (k == g_side && intoFolder && HasView()) g_expectNew = {g_view.data, GetTickCount64() + 15000, true};
    };
    h.transfer = [](const std::wstring& dest, std::vector<std::wstring> paths, bool move) {
        FileOp op;
        op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
        op.folder = dest;
        op.paths = std::move(paths);
        StartOp(std::move(op));
    };
    DragDrop_RegisterTarget(g_lists[k], std::move(h));
}

void RegisterTabDrop();

void RegisterDrop() {
    RegisterDropOn(0);
    RegisterDropOn(1);
    // Fil d'Ariane : déposer sur un segment copie / déplace vers ce dossier.
    DropHost c;
    c.fullPaths = true;
    c.hit = [](POINT client, std::wstring& path) { return Crumbs_DropHit(client, path); };
    c.highlight = [](int part) { Crumbs_DropHighlight(part); };
    c.transfer = [](const std::wstring& dest, std::vector<std::wstring> paths, bool move) {
        FileOp op;
        op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
        op.folder = dest;
        op.paths = std::move(paths);
        StartOp(std::move(op));
    };
    DragDrop_RegisterTarget(g_crumbs, std::move(c));
    RegisterTabDrop();
}

// ───────────────────────────── Onglets, fenêtres ─────────────────────────────
//
// Chaque onglet garde son dossier, son historique, son tri, son filtre, sa
// sélection, sa position et le contenu déjà lu : y revenir l'affiche aussitôt,
// puis le dossier est relu en arrière-plan (comme F5). Seul l'onglet affiché
// est surveillé.

uint64_t g_tabIds = 0;

struct Tab {
    uint64_t id = ++g_tabIds;  // stable : double panneau (onglet affiché dans l'autre panneau)
    std::wstring path, query, filter, focus;
    std::vector<std::wstring> back, fwd, selected;
    int top = 0;
    std::shared_ptr<const DirData> data;
};
std::vector<Tab> g_tabList(1);
size_t g_tab = 0;
int g_tabDrag = -1;  // onglet que l'on fait glisser (réorganisation)
uint64_t g_otherTabId = 0;  // double panneau : onglet affiché dans le panneau inactif
uint64_t g_restoreOther = 0;  // onglets rouverts : onglet actif, montré quand le double panneau se rétablit
bool g_tabOut = false;  // onglet glissé hors de la barre : détacher ou fusionner au relâchement

int TabIndexOf(uint64_t id) {
    for (size_t i = 0; i < g_tabList.size(); ++i)
        if (g_tabList[i].id == id) return (int)i;
    return -1;
}

void SetDual(bool on);
void SwitchSide();
void UpdatePaneHeaders();

std::wstring TabText(const Tab& t) {
    std::wstring s = t.query.empty() ? HistoryText(t.path) : L"Recherche « " + t.query + L" »";
    if (s.size() > 32) s = s.substr(0, 31) + L"…";
    return s;
}

void UpdateTabStrip() {
    if (g_dual) {  // l'onglet de l'autre panneau a été fermé, ou est devenu l'onglet affiché
        const int o = TabIndexOf(g_otherTabId);
        if (o < 0 || o == (int)g_tab) SetDual(false);
    }
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
    UpdatePaneHeaders();
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
    ApplyFolderView(ViewFor(t.path));  // affichage mémorisé de son dossier
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
    r->order = SortOrder(*t.data, g_sort.key, g_sortAsc, g_prefs, g_filter);
    OnResult(r);
    if (t.query.empty()) Refresh();  // changements faits pendant qu'on était ailleurs
}

void SwitchTab(size_t i) {
    if (g_dual && (int)i == TabIndexOf(g_otherTabId)) {  // onglet de l'autre panneau : on y passe
        SwitchSide();
        SetFocus(g_list);
        return;
    }
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

void RegisterTabDrop() {
    // Onglets : déposer dans le dossier d'un onglet ; le survoler l'affiche.
    DropHost t;
    t.fullPaths = true;
    t.hit = [](POINT client, std::wstring& path) {
        TCHITTESTINFO ht{client, 0};
        const int i = TabCtrl_HitTest(g_tabBar, &ht);
        if (i < 0 || i >= (int)g_tabList.size()) return -1;
        path = (size_t)i == g_tab ? g_path : g_tabList[(size_t)i].path;
        return i;
    };
    t.highlight = [](int i) {
        static int hot = -1;
        if (i == hot) return;
        if (hot >= 0) TabCtrl_HighlightItem(g_tabBar, hot, FALSE);
        hot = i;
        if (i >= 0) TabCtrl_HighlightItem(g_tabBar, i, TRUE);
    };
    t.spring = [](int i, const std::wstring&) {
        if (i >= 0 && (size_t)i != g_tab) SwitchTab((size_t)i);
        return false;
    };
    t.transfer = [](const std::wstring& dest, std::vector<std::wstring> paths, bool move) {
        FileOp op;
        op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
        op.folder = dest;
        op.paths = std::move(paths);
        StartOp(std::move(op));
    };
    DragDrop_RegisterTarget(g_tabBar, std::move(t));
}

// ───────────────────────────── Double panneau ─────────────────────────────


std::wstring PaneTitle(const std::wstring& path, const std::wstring& query) {
    if (!query.empty()) return L"Recherche « " + query + L" » dans " + PlaceText(path);
    return PlaceText(path);
}

void UpdatePaneHeaders() {
    if (!g_dual) return;
    for (int s = 0; s < 2; ++s) {
        const std::wstring t = L" " + (s == g_side ? PaneTitle(g_pending.active ? g_pending.path : g_path, g_query)
                                                    : PaneTitle(g_passive.path, g_passive.query));
        SetWindowTextW(g_paneHdr[s], t.c_str());
        InvalidateRect(g_paneHdr[s], nullptr, TRUE);
    }
}

// Liste du panneau inactif : rendu à partir de l'image gardée, sans rien lire.
void FillPassive(LVITEMW& it) {
    View& v = g_passive.view;
    if (!v.data || it.iItem < 0 || it.iItem >= (int)v.order.size()) return;
    const int idx = (int)v.order[it.iItem];
    const Entry& e = v.data->entries[idx];
    // Lignes espacées : la liste de lignes (commune) ; sinon les listes système.
    const bool rows = g_rowList && g_passive.mode <= VM_SMALL &&
                      (HIMAGELIST)SendMessageW(g_lists[g_side ^ 1], LVM_GETIMAGELIST, LVSIL_SMALL, 0) == g_rowList;
    if (it.mask & LVIF_IMAGE) {
        const int sys = v.iconState[idx] != ICON_NONE ? v.icons[idx] : IconOf(*v.data, e);
        it.iImage = rows ? RowSlot(sys) : sys;
    }
    if (it.mask & LVIF_STATE) {
        const bool dim = v.cut[idx] || (!v.data->isRoot() && (e.attr & FILE_ATTRIBUTE_HIDDEN));
        if (rows) RowOverlay(v.overlays[idx]);
        it.state = INDEXTOOVERLAYMASK(v.overlays[idx]) | (dim ? LVIS_CUT : 0);
        it.stateMask = LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED;
    }
    if (!(it.mask & (LVIF_TEXT | LVIF_COLUMNS))) return;
    // Texte et colonnes : le rendu ordinaire, sur l'état du panneau inactif.
    std::swap(g_view, v);
    std::swap(g_cols, g_passive.cols);
    std::swap(g_path, g_passive.path);
    std::swap(g_viewMode, g_passive.mode);
    LVITEMW sub = it;
    sub.mask &= LVIF_TEXT | LVIF_COLUMNS;
    FillDispInfo(sub);
    it.cColumns = sub.cColumns;
    it.puColumns = sub.puColumns;
    std::swap(g_viewMode, g_passive.mode);
    std::swap(g_path, g_passive.path);
    std::swap(g_cols, g_passive.cols);
    std::swap(g_view, v);
}

// Image d'un contenu lu : vue triée, sans icônes propres ni tailles de dossiers.
View PassiveView(std::shared_ptr<const DirData> d, const ColSpec& sort, bool asc, const std::wstring& filter) {
    View v;
    const size_t n = d->entries.size();
    v.order = SortOrder(*d, sort.key == COL_PROP ? COL_NAME : sort.key, asc, g_prefs, filter);
    v.rowOf.assign(n, -1);
    for (size_t r = 0; r < v.order.size(); ++r) v.rowOf[v.order[r]] = (int)r;
    v.icons.assign(n, 0);
    v.overlays.assign(n, 0);
    v.iconState.assign(n, ICON_NONE);
    v.cut.assign(n, 0);
    v.dirBytes.assign(n, 0);
    v.dirState.assign(n, 0);
    v.thumb.assign(n, THUMB_UNKNOWN);
    v.data = std::move(d);
    return v;
}

// Relit le dossier du panneau inactif en arrière-plan (après une copie vers lui…).
void ReloadPassive() {
    if (!g_dual || g_passive.path.empty() || !g_passive.query.empty()) return;
    std::vector<NavFolder> pins;
    if (g_passive.path == kHomePath) pins = NavPane_ShownPins();
    std::thread([path = g_passive.path, hwnd = g_hwnd, pins = std::move(pins)] {
        auto* d = new DirData;
        d->path = path;
        if (path == kBinPath) Recycle_Enumerate(*d);
        else if (path == kHomePath) Home_Enumerate(*d, pins);
        else if (path == kNetPath) Net_EnumServers(*d);
        else if (Net_IsServerPath(path)) Net_EnumShares(*d);
        else if (std::wstring arc, inner; Archive_Split(path, arc, inner)) Archive_Enumerate(*d);
        else Enumerate(*d);
        if (!PostMessageW(hwnd, WM_APP_PASSIVE, 0, (LPARAM)d)) delete d;
    }).detach();
}

void OnPassiveData(DirData* raw) {
    std::shared_ptr<const DirData> d(raw);
    if (!g_dual || d->error || _wcsicmp(d->path.c_str(), g_passive.path.c_str()) != 0 || !g_passive.query.empty()) return;
    g_passive.view = PassiveView(d, g_passive.sort, g_passive.asc, g_passive.filter);
    if (const int o = TabIndexOf(g_otherTabId); o >= 0) g_tabList[o].data = d;  // y revenir l'affichera tel quel
    HWND lv = g_lists[g_side ^ 1];
    ListView_SetItemCountEx(lv, (int)g_passive.view.order.size(), LVSICF_NOSCROLL);
    InvalidateRect(lv, nullptr, TRUE);
}

// Le panneau inactif devient actif (clic, Tab, onglet), et inversement.
void SwitchSide() {
    const int other = g_dual ? TabIndexOf(g_otherTabId) : -1;
    if (other < 0 || other == (int)g_tab) return;
    if (g_editing) SendMessageW(g_list, LVM_CANCELEDITLABEL, 0, 0);
    SaveFolderView();
    SaveTab();
    HWND old = g_list;
    Passive p;
    p.view = std::move(g_view);
    g_view = View{};
    if (p.view.arranged) {  // le panneau inactif montre l'ordre du tri, sans groupes ni filtres
        p.view.order = p.view.sorted;
        p.view.groups.clear();
        p.view.arranged = false;
        p.view.rowOf.assign(p.view.data ? p.view.data->entries.size() : 0, -1);
        for (size_t r = 0; r < p.view.order.size(); ++r) p.view.rowOf[p.view.order[r]] = (int)r;
        ListView_SetItemCountEx(old, (int)p.view.order.size(), LVSICF_NOSCROLL);
    }
    p.cols = g_cols;
    p.path = g_path;
    p.query = g_query;
    p.filter = g_filter;
    p.mode = g_viewMode;
    p.sort = g_sort;
    p.asc = g_sortAsc;
    // Ses listes d'images propres (lignes espacées, miniatures) peuvent être recréées : listes système.
    if ((HIMAGELIST)SendMessageW(old, LVM_GETIMAGELIST, LVSIL_SMALL, 0) != g_rowList) ListView_SetImageList(old, g_smallPick, LVSIL_SMALL);
    ListView_SetImageList(old, g_imgLarge, LVSIL_NORMAL);
    g_listCols[g_side] = g_cols;
    g_otherTabId = g_tabList[g_tab].id;
    g_passive = std::move(p);
    g_side ^= 1;
    g_list = g_lists[g_side];
    g_cols = g_listCols[g_side];
    ShowTab((size_t)other);
    UpdateTabStrip();
    UpdatePaneHeaders();
    InvalidateRect(old, nullptr, TRUE);
}

void Layout();

// F9 : un second panneau à côté (le même dossier, ou l'onglet suivant), ou retour à un seul.
void SetDual(bool on) {
    if (on == g_dual || !g_lists[1]) return;
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"Dual", REG_DWORD, &v, sizeof v);
    if (on) {
        if (!HasView()) {  // rien encore d'affiché : au premier dossier
            g_dualAtStart = true;
            return;
        }
        SaveTab();
        // Onglets rouverts : le panneau actif de la dernière fois existe déjà.
        const int keep = g_restoreOther ? TabIndexOf(g_restoreOther) : -1;
        g_restoreOther = 0;
        if (keep >= 0 && keep != (int)g_tab) {
            g_otherTabId = g_tabList[(size_t)keep].id;
        } else {
            Tab t;
            t.path = g_path;
            t.query = g_query;
            t.data = g_view.data;  // même contenu : affiché aussitôt, puis relu
            g_tabList.insert(g_tabList.begin() + (ptrdiff_t)g_tab + 1, std::move(t));
            g_otherTabId = g_tabList[g_tab + 1].id;
        }
        g_dual = true;
        for (HWND h : {g_lists[0], g_lists[1], g_paneHdr[0], g_paneHdr[1]}) ShowWindow(h, SW_SHOW);
        Layout();
        SwitchSide();  // le nouvel onglet s'ouvre dans l'autre liste, qui devient active
        SetFocus(g_list);
    } else {
        g_dual = false;
        HWND other = g_lists[g_side ^ 1];
        ListView_SetItemCountEx(other, 0, 0);
        for (HWND h : {other, g_paneHdr[0], g_paneHdr[1]}) ShowWindow(h, SW_HIDE);
        g_passive = Passive{};
        Layout();
        UpdateTabStrip();
    }
}

// F5 / F6 en double panneau : copier / déplacer la sélection vers l'autre panneau.
void TransferToOther(bool move) {
    if (!g_dual || !HasView() || g_view.data->isRoot() || InBin()) return;  // Corbeille : restaurer d'abord
    if (g_view.data->path == kNetPath || g_view.data->server) {
        Flash(L"Ouvrez un partage pour copier ses fichiers");
        return;
    }
    if (move && InVirtual()) move = false;  // Accueil : on copie (les originaux restent où ils sont)
    const std::wstring dest = g_passive.path;
    if (dest.empty() || IsVirtualPath(dest) || !g_passive.query.empty()) {
        Flash(L"L'autre panneau ne montre pas un dossier");
        return;
    }
    if (g_passive.view.data && g_passive.view.data->listOnly()) {
        Flash(L"L'autre panneau ne montre pas un dossier modifiable");
        return;
    }
    if (InArchive()) {  // F5 / F6 depuis une archive : extraction vers l'autre panneau
        const auto sel = ArchivePicks();
        if (sel.empty()) return;
        ExtractEntries(sel, ThenDo::Nothing, dest);
        Flash(L"Extraction de " + std::to_wstring(sel.size()) + (sel.size() > 1 ? L" éléments" : L" élément") + L" vers " + dest);
        return;
    }
    if (_wcsicmp(dest.c_str(), g_path.c_str()) == 0 && g_query.empty()) {
        Flash(L"Les deux panneaux montrent le même dossier");
        return;
    }
    std::vector<int> sel = SelectedEntries();
    if (sel.empty())
        if (const int f = FocusedEntry(); f >= 0) sel.push_back(f);
    if (sel.empty()) return;
    FileOp op;
    op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
    op.folder = dest;
    for (int e : sel) op.paths.push_back(FullPath(e));
    const size_t n = op.paths.size();
    StartOp(std::move(op));
    Flash(std::wstring(move ? L"Déplacement de " : L"Copie de ") + std::to_wstring(n) + (n > 1 ? L" éléments" : L" élément") +
          L" vers " + dest);
}

// Nouvelle fenêtre (Ctrl+N), un peu décalée par rapport à celle-ci, sur le même écran ;
// at : sous ce point (onglet détaché), sur son écran.
void NewWindow(const std::wstring& path, const POINT* under = nullptr) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(g_hwnd, &wp);
    // Décalée de 30 px, sans déborder de la zone de travail de l'écran.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(under ? MonitorFromPoint(*under, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST),
                    &mi);
    const RECT& wa = mi.rcWork;
    const LONG w = std::min(wp.rcNormalPosition.right - wp.rcNormalPosition.left, wa.right - wa.left);
    const LONG h = std::min(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, wa.bottom - wa.top);
    LONG x = wp.rcNormalPosition.left + S(30), y = wp.rcNormalPosition.top + S(30);
    if (under) {  // l'onglet reste sous le curseur
        x = under->x - S(80);
        y = under->y - S(12);
    }
    if (x + w > wa.right) x = under ? wa.right - w : wa.left;
    if (y + h > wa.bottom) y = under ? wa.bottom - h : wa.top;
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

constexpr ULONG_PTR kCopyTab = 0x46454254;  // WM_COPYDATA « FEBT » : onglet glissé depuis une autre fenêtre

// Onglet relâché hors de la barre : sur une autre fenêtre FastExplorer, il la
// rejoint ; ailleurs, il part dans une nouvelle fenêtre, sous le curseur.
void DetachTab(size_t i, POINT s) {
    if (i >= g_tabList.size()) return;
    if (i == g_tab) SaveTab();
    const std::wstring path = g_tabList[i].path;
    HWND w = GetAncestor(WindowFromPoint(s), GA_ROOT);
    if (w == g_hwnd) return;
    wchar_t cls[64] = {};
    if (w && GetClassNameW(w, cls, 64) && wcscmp(cls, L"FastExplorerWnd") == 0) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cd{kCopyTab, (DWORD)((path.size() + 1) * sizeof(wchar_t)), (void*)path.c_str()};
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(w, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r) && r)
            CloseTab(i);  // c'était le seul onglet : cette fenêtre se ferme
        else
            Flash(L"L'autre fenêtre ne répond pas.");
        return;
    }
    if (g_tabList.size() < 2) return;  // seul onglet : c'est la fenêtre qu'on déplace
    NewWindow(path, &s);
    CloseTab(i);
}

// Onglet reçu d'une autre fenêtre (WM_COPYDATA).
bool ReceiveTab(const COPYDATASTRUCT* cd) {
    if (!cd || cd->dwData != kCopyTab || !cd->lpData || cd->cbData < sizeof(wchar_t) || cd->cbData % sizeof(wchar_t))
        return false;
    const auto* s = (const wchar_t*)cd->lpData;
    const std::wstring path(s, wcsnlen(s, cd->cbData / sizeof(wchar_t)));
    NewTab(path, true);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    return true;
}

// Onglets ouverts (dossier de chacun), pour la réouverture et les sessions.
SessionTabs CurrentTabs() {
    SaveTab();
    SessionTabs s;
    for (const Tab& t : g_tabList) s.paths.push_back(t.path);
    s.active = (int)g_tab;
    s.other = g_dual ? TabIndexOf(g_otherTabId) : -1;
    return s;
}

// Remplace les onglets par ceux d'une session.
void LoadTabs(const SessionTabs& s) {
    if (s.paths.empty()) return;
    if (g_dual) SetDual(false);
    std::vector<Tab> v;
    for (const auto& p : s.paths) {
        Tab t;
        t.path = p;
        v.push_back(std::move(t));
    }
    g_tabList = std::move(v);
    g_tab = std::min<size_t>((size_t)std::max(s.active, 0), g_tabList.size() - 1);
    UpdateTabStrip();
    ShowTab(g_tab);
}

std::wstring MenuText(std::wstring s) {  // « & » affiché tel quel dans un menu
    for (size_t i = 0; (i = s.find(L'&', i)) != std::wstring::npos; i += 2) s.insert(i, 1, L'&');
    return s;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n > 1 ? many : one);
}

void SaveSessionAs() {
    const SessionTabs s = CurrentTabs();
    std::wstring name = TabText(g_tabList[g_tab]);
    if (!AskText(g_hwnd, L"Enregistrer la session",
                 L"Nom de la session (" + Plural(s.paths.size(), L"onglet", L"onglets") + L") :", name))
        return;
    const auto names = Session_Names();
    const bool existed = std::any_of(names.begin(), names.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; });
    if (!Session_Save(name, s)) {
        SetStatus(L"Impossible d'enregistrer la session « " + name + L" ».");
        return;
    }
    Flash((existed ? L"Session remplacée : « " : L"Session enregistrée : « ") + name + L" » (" +
          Plural(s.paths.size(), L"onglet", L"onglets") + L")");
}

void OpenSession(const std::wstring& name) {
    SessionTabs s;
    if (!Session_Load(name, s)) {
        SetStatus(L"Session « " + name + L" » illisible.");
        return;
    }
    LoadTabs(s);
    Flash(L"Session « " + name + L" » : " + Plural(s.paths.size(), L"onglet", L"onglets"));
}

std::vector<MenuExtra> SessionItems() {
    std::vector<MenuExtra> v, del;
    for (const auto& n : Session_Names()) {
        v.push_back({MenuText(n), [n] { OpenSession(n); }});
        del.push_back({MenuText(n), [n] {
                           if (Session_Delete(n)) Flash(L"Session supprimée : « " + n + L" »");
                       }});
    }
    if (v.empty()) v.push_back({L"Aucune session enregistrée", nullptr, false, false, {}, true});
    v.push_back({});
    v.push_back({L"Enregistrer les onglets…", SaveSessionAs});
    if (!del.empty()) v.push_back({L"Supprimer", nullptr, false, false, std::move(del)});
    v.push_back({});
    v.push_back({L"Rouvrir les onglets au démarrage", [] { Session_SetRestore(!Session_RestoreEnabled()); }, false,
                 Session_RestoreEnabled()});
    return v;
}

// Signets : Alt+1…9 pour y aller, Alt+Maj+1…9 pour y ranger le dossier affiché.
void GoBookmark(int n) {
    std::wstring path;
    if (!Bookmark_Get(n, path)) {
        Flash(L"Signet " + std::to_wstring(n) + L" vide : Alt+Maj+" + std::to_wstring(n) + L" pour y ranger ce dossier");
        return;
    }
    Navigate(path, Nav::Normal);
}

void SetBookmark(int n) {
    const std::wstring path = g_pending.active ? g_pending.path : g_path;
    if (!Bookmark_Set(n, path)) {
        SetStatus(L"Impossible d'enregistrer le signet.");
        return;
    }
    Flash(L"Signet " + std::to_wstring(n) + L" (Alt+" + std::to_wstring(n) + L") : " + HistoryText(path));
}

std::vector<MenuExtra> BookmarkItems() {
    std::vector<MenuExtra> v, del;
    int free = 0;
    for (int n = 1; n <= kBookmarks; ++n) {
        std::wstring path;
        if (!Bookmark_Get(n, path)) {
            if (!free) free = n;
            continue;
        }
        const std::wstring k = std::to_wstring(n);
        v.push_back({L"&" + k + L"  " + MenuText(HistoryText(path)) + L"\tAlt+" + k, [n] { GoBookmark(n); }});
        del.push_back({MenuText(HistoryText(path)), [n] { Bookmark_Clear(n); }});
    }
    if (v.empty()) v.push_back({L"Aucun signet", nullptr, false, false, {}, true});
    v.push_back({});
    v.push_back({free ? L"Ajouter ce dossier aux signets\tAlt+Maj+" + std::to_wstring(free)
                      : std::wstring(L"Ajouter ce dossier aux signets"),
                 [free] { SetBookmark(free); }, false, false, {}, !free});
    if (!del.empty()) v.push_back({L"Retirer", nullptr, false, false, std::move(del)});
    return v;
}

void ShowTabMenu(int hit, POINT pt) {    HMENU m = CreatePopupMenu();
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
            copy.id = ++g_tabIds;
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

// « Connecter un lecteur réseau » de Windows, partage proposé.
void MapDriveDialog(const std::wstring& share) {
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_DISK;
    std::wstring remote = share;
    nr.lpRemoteName = remote.empty() ? nullptr : remote.data();
    CONNECTDLGSTRUCTW c{};
    c.cbStructure = sizeof c;
    c.hwndOwner = g_hwnd;
    c.lpConnRes = &nr;
    c.dwFlags = CONNDLG_USE_MRU | (remote.empty() ? 0 : CONNDLG_RO_PATH);
    const DWORD r = WNetConnectionDialog1W(&c);
    if (r == NO_ERROR && c.dwDevNum != (DWORD)-1 && c.dwDevNum > 0)
        Flash(L"Lecteur " + std::wstring(1, (wchar_t)(L'A' + c.dwDevNum - 1)) + L": connecté à " + share);
    else if (r != NO_ERROR && r != (DWORD)-1 && r != ERROR_CANCELLED)
        SetStatus(L"Connexion impossible : " + Net_ErrorText(r));
}

// Réseau : serveurs connus.
void NetForget(const std::vector<int>& sel) {
    if (!HasView() || g_view.data->path != kNetPath) return;
    for (int e : sel) Net_Forget(FullPath(e));
    Refresh();
}

void ShowNetMenu(POINT pt, const std::vector<int>& sel) {
    enum : UINT { kOpen = 1, kTab, kPaths, kForget, kGo, kMap, kRefresh };
    HMENU m = CreatePopupMenu();
    if (!sel.empty()) {
        AppendMenuW(m, MF_STRING, kOpen, L"Ouvrir");
        if (sel.size() == 1) AppendMenuW(m, MF_STRING, kTab, L"Ouvrir dans un nouvel onglet");
        AppendMenuW(m, MF_STRING, kPaths, L"Copier en tant que chemin d'accès\tCtrl+Maj+C");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kForget, L"Retirer de la liste\tSuppr");
        SetMenuDefaultItem(m, kOpen, FALSE);
    } else {
        AppendMenuW(m, MF_STRING, kGo, L"Ouvrir un serveur (\\\\nom)…");
        AppendMenuW(m, MF_STRING, kMap, L"Connecter un lecteur réseau…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRefresh, L"Actualiser\tF5");
    }
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kOpen: if (!sel.empty()) ActivateEntry(sel[0]); break;
        case kTab: NewTab(FullPath(sel[0]), true); break;
        case kPaths: CopyPaths({}); break;
        case kForget: NetForget(sel); break;
        case kGo:
            BeginAddressEdit();
            SetWindowTextW(g_addr, L"\\\\");
            Edit_SetSel(g_addr, 2, 2);
            break;
        case kMap: MapDriveDialog(L""); break;
        case kRefresh: Refresh(); break;
    }
}

// Corbeille, Accueil : nos propres commandes ; un élément de l'Accueil (ou des éléments
// d'un même dossier) garde le menu du Shell de son vrai dossier.
void ShowVirtualMenu(POINT pt, const std::vector<int>& sel) {
    const DirData& d = *g_view.data;
    if (d.path == kNetPath) {
        ShowNetMenu(pt, sel);
        return;
    }
    const bool bin = d.path == kBinPath;
    if (!bin && !sel.empty()) {
        std::wstring parent;
        std::vector<std::wstring> leaves;
        bool same = true;
        for (int e : sel) {
            std::wstring par, leaf;
            if (!ParentOf(FullPath(e), par, leaf) || leaf.empty()) { same = false; break; }
            if (parent.empty()) parent = par;
            else if (_wcsicmp(parent.c_str(), par.c_str()) != 0) { same = false; break; }
            leaves.push_back(leaf);
        }
        if (same && !parent.empty()) {
            std::vector<MenuExtra> extras;
            if (leaves.size() == 1) {
                const std::wstring leaf = leaves[0];
                extras.push_back({L"Ouvrir l'emplacement du fichier", [parent, leaf] {
                                      Navigate(parent, Nav::Normal, leaf, {leaf});
                                      SetFocus(g_list);
                                  }});
            }
            extras.push_back({L"Copier en tant que chemin d'accès\tCtrl+Maj+C", [] { CopyPaths({}); }});
            const std::vector<std::wstring> full = TargetsOf(sel);
            const bool ok = ShellMenu_Show(g_hwnd, parent, leaves, pt, GetKeyState(VK_SHIFT) < 0,
                                           [full](const std::wstring& verb) {
                auto is = [&](const wchar_t* v) { return _wcsicmp(verb.c_str(), v) == 0; };
                if (full.size() == 1 && (is(L"open") || is(L"explore"))) {
                    const DWORD a = GetFileAttributesW(full[0].c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) Navigate(full[0], Nav::Normal);
                    else OpenWithShell(full[0]);
                    return true;
                }
                return false;  // le reste : le Shell, sur les vrais fichiers
            }, extras);
            if (!ok) SetStatus(L"Menu contextuel indisponible pour cet élément.");
            return;
        }
    }
    enum : UINT { kOpen = 1, kRestore, kRestoreAll, kPurge, kEmpty, kCopy, kPaths, kRefresh };
    HMENU m = CreatePopupMenu();
    if (bin) {
        if (!sel.empty()) {
            AppendMenuW(m, MF_STRING, kRestore, L"Restaurer");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, kPurge, L"Supprimer définitivement\tSuppr");
        } else {
            AppendMenuW(m, MF_STRING | (d.entries.empty() ? MF_GRAYED : 0), kEmpty, L"Vider la Corbeille");
            AppendMenuW(m, MF_STRING | (d.entries.empty() ? MF_GRAYED : 0), kRestoreAll, L"Restaurer tous les éléments");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, kRefresh, L"Actualiser\tF5");
        }
    } else if (!sel.empty()) {
        AppendMenuW(m, MF_STRING, kOpen, L"Ouvrir");
        AppendMenuW(m, MF_STRING, kCopy, L"Copier\tCtrl+C");
        AppendMenuW(m, MF_STRING, kPaths, L"Copier en tant que chemin d'accès\tCtrl+Maj+C");
    } else {
        AppendMenuW(m, MF_STRING, kRefresh, L"Actualiser\tF5");
    }
    if (bin && !sel.empty()) SetMenuDefaultItem(m, kRestore, FALSE);
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kOpen:
            for (int e : sel) ActivateEntry(e);
            break;
        case kRestore: BinRestore(false); break;
        case kRestoreAll: BinRestore(true); break;
        case kPurge: BinPurge(); break;
        case kEmpty: BinEmpty(); break;
        case kCopy: CopyNames({}, false); break;
        case kPaths: CopyPaths({}); break;
        case kRefresh: Refresh(); break;
    }
}

// Vue d'archive : nos commandes (le Shell ne connaît pas ces chemins).
void ShowArchiveMenu(POINT pt, const std::vector<int>& sel) {
    const std::wstring archive = g_view.data->archive;
    std::wstring parent, leaf;
    if (!ParentOf(archive, parent, leaf)) return;
    enum : UINT { kOpen = 1, kExtractTo, kCopy, kPaths, kAll, kAllTo, kRefresh, kProps };
    HMENU m = CreatePopupMenu();
    if (!sel.empty()) {
        AppendMenuW(m, MF_STRING, kOpen, L"Ouvrir");
        AppendMenuW(m, MF_STRING, kExtractTo, L"Extraire vers…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kCopy, L"Copier\tCtrl+C");
        AppendMenuW(m, MF_STRING, kPaths, L"Copier en tant que chemin d'accès\tCtrl+Maj+C");
        SetMenuDefaultItem(m, kOpen, FALSE);
    } else {
        AppendMenuW(m, MF_STRING, kAll, (L"Extraire tout dans « " + Archive_Stem(leaf) + L" »").c_str());
        AppendMenuW(m, MF_STRING, kAllTo, L"Extraire tout vers…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRefresh, L"Actualiser\tF5");
        AppendMenuW(m, MF_STRING, kProps, L"Propriétés de l'archive\tAlt+Entrée");
    }
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kOpen:
            for (int e : sel) ActivateEntry(e);
            break;
        case kExtractTo:
            if (const std::wstring to = PickFolder(parent, L"Extraire vers"); !to.empty()) ExtractEntries(sel, ThenDo::Nothing, to);
            break;
        case kCopy: CopyNames({}, false); break;
        case kPaths: CopyPaths(NamesOf(sel)); break;
        case kAll: ExtractArchive(archive, parent, true, ThenDo::Show); break;
        case kAllTo:
            if (const std::wstring to = PickFolder(parent, L"Extraire tout vers"); !to.empty())
                ExtractArchive(archive, to, false, ThenDo::Nothing);
            break;
        case kRefresh: Refresh(); break;
        case kProps:
            if (!ShellMenu_Invoke(g_hwnd, parent, {leaf}, "properties")) SetStatus(L"Propriétés indisponibles.");
            break;
    }
}

bool QuickMenuApplies(bool shift);
void ShowQuickMenu(POINT pt, const std::vector<int>& sel);
void ShowFullMenu(POINT pt, std::vector<int> sel);

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
    if (InVirtual()) {
        ShowVirtualMenu(pt, sel);
        return;
    }
    if (InArchive()) {
        ShowArchiveMenu(pt, sel);
        return;
    }
    if (QuickMenuApplies(GetKeyState(VK_SHIFT) < 0)) {
        ShowQuickMenu(pt, sel);
        return;
    }
    ShowFullMenu(pt, sel);
}

// Menu complet du Shell (extensions comprises), précédé de nos commandes.
void ShowFullMenu(POINT pt, std::vector<int> sel) {
    // Les noms (et non des index) : la vue peut être rafraîchie pendant que le menu est ouvert.
    const std::vector<std::wstring> names = NamesOf(sel);
    std::vector<MenuExtra> extras;
    if (sel.size() == 1 && E(sel[0]).isDir()) {
        const std::wstring dir = g_path.empty() ? names[0] : Join(g_path, names[0].c_str());  // Ce PC : un lecteur
        extras = PinExtras(dir, true);
    } else if (sel.empty() && !g_path.empty()) {  // fond du dossier : annuler / rétablir, options d'affichage
        if (!g_undo.empty()) extras.push_back({L"Annuler " + StepText(g_undo.back()) + L"\tCtrl+Z", Undo});
        if (!g_redo.empty()) extras.push_back({L"Rétablir " + StepText(g_redo.back()) + L"\tCtrl+Y", Redo});
        extras.push_back(ViewMenu());
        extras.push_back({L"Afficher les éléments masqués", ToggleHidden, false, g_prefs.showHidden});
        extras.push_back({L"Afficher les extensions de noms de fichiers", ToggleExtensions, false, g_prefs.showExtensions});
    }
    if (g_view.data->server && names.size() == 1) {  // partage : en faire un lecteur
        const std::wstring share = Join(g_path, names[0].c_str());
        extras.push_back({L"Connecter un lecteur réseau…", [share] { MapDriveDialog(share); }});
    }
    if (!names.empty() && !g_path.empty() && !g_view.data->isVirtual() && !g_view.data->listOnly()) {  // archives
        if (names.size() == 1 && !E(sel[0]).isDir() && Archive_IsArchive(names[0])) {
            const std::wstring arc = Join(g_path, names[0].c_str()), here = g_path;
            extras.push_back({L"Extraire tout", [arc, here] { ExtractArchive(arc, here, true, ThenDo::Nothing); }});
            extras.push_back({L"Extraire vers…", [arc, here] {
                                  const std::wstring to = PickFolder(here, L"Extraire vers");
                                  if (!to.empty()) ExtractArchive(arc, to, false, ThenDo::Nothing);
                              }});
        }
        extras.push_back({L"Compresser dans un fichier ZIP", [names] { CompressEntries(FindEntriesByNames(names)); }});
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
            Paste(single >= 0 && E(single).isDir() ? FullPath(single) : g_path);
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
    if (!HasView() || InVirtual() || g_view.data->inArchive()) return;
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
    if (InArchive()) {  // celles du fichier d'archive lui-même
        std::wstring parent, leaf;
        if (!ParentOf(g_view.data->archive, parent, leaf) || !ShellMenu_Invoke(g_hwnd, parent, {leaf}, "properties"))
            SetStatus(L"Propriétés indisponibles.");
        return;
    }
    if (InVirtual()) {  // élément réel (Accueil) ; Corbeille : celui qui est dans $Recycle.Bin
        const auto sel = SelectedEntries();
        std::wstring parent, leaf;
        if (sel.empty() || !ParentOf(FullPath(sel[0]), parent, leaf) ||
            !ShellMenu_Invoke(g_hwnd, parent, {leaf}, "properties"))
            SetStatus(L"Propriétés indisponibles.");
        return;
    }
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

// ───────────────────────────── Barre de commandes, menu rapide ─────────────────────────────
// Les commandes courantes en un clic, comme la barre de l'Explorateur de
// Windows 11. Le menu contextuel « rapide » ne contient que nos commandes :
// il s'affiche sans attendre les gestionnaires du Shell ; « Afficher plus
// d'options » (ou Maj+clic droit, Maj+F10) ouvre le menu complet du Shell.

// Menu de nos seules commandes. Séparateur : texte vide.
void ShowOwnMenu(const std::vector<MenuExtra>& items, POINT pt, UINT flags = TPM_RIGHTBUTTON) {
    std::vector<const MenuExtra*> flat;
    std::function<void(HMENU, const std::vector<MenuExtra>&)> add = [&](HMENU m, const std::vector<MenuExtra>& v) {
        for (const MenuExtra& x : v) {
            if (x.text.empty()) {
                if (GetMenuItemCount(m) > 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            const UINT f = MF_STRING | (x.checked ? MF_CHECKED : 0) | (x.disabled ? MF_GRAYED : 0);
            if (!x.sub.empty()) {
                HMENU s = CreatePopupMenu();
                add(s, x.sub);
                AppendMenuW(m, f | MF_POPUP, (UINT_PTR)s, x.text.c_str());
                continue;
            }
            flat.push_back(&x);
            AppendMenuW(m, f, flat.size(), x.text.c_str());
            if (x.isDefault) SetMenuDefaultItem(m, (UINT)flat.size(), FALSE);
        }
    };
    HMENU m = CreatePopupMenu();
    add(m, items);
    const UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | flags, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd && cmd <= flat.size() && flat[cmd - 1]->run) flat[cmd - 1]->run();
}

bool ClipboardHasFiles() {
    static const UINT idList = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    return IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(idList);
}

bool InNet() { return HasView() && (g_view.data->path == kNetPath || g_view.data->server); }

// Ouvre un terminal dans le dossier : Windows Terminal, sinon PowerShell.
void OpenTerminal(const std::wstring& dir) {
    if (dir.empty() || InVirtual() || InNet()) return;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, found, nullptr)) {
        std::wstring d = dir;
        if (d.back() == L'\\') d += L'.';  // « "C:\" » : le \ final échapperait le guillemet
        RunCommand(L"wt.exe -d \"" + d + L"\"", dir);
    } else {
        RunCommand(L"powershell.exe -NoExit", dir);
    }
}

void InvertSelection() {
    if (!HasView()) return;
    const int n = RowCount();
    std::vector<uint8_t> was(n, 0);
    for (int r = -1; (r = ListView_GetNextItem(g_list, r, LVNI_SELECTED)) >= 0;)
        if (r < n) was[r] = 1;
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    for (int r = 0; r < n; ++r)
        if (!was[r]) ListView_SetItemState(g_list, r, LVIS_SELECTED, LVIS_SELECTED);
    ScheduleSelectionSummary();
}

void SetVerifyCopies(bool on);

void SetFullMenu(bool on) {
    g_fullMenu = on;
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"FullMenu", REG_DWORD, &v, sizeof v);
    ShellMenu_Discard();
}

MenuExtra NewMenu() {
    const bool can = CanModify();
    MenuExtra m;
    m.text = L"Nouveau";
    m.sub = {{L"Dossier\tCtrl+Maj+N", [] { NewFolder(); }, false, false, {}, !can},
             {L"Document texte", [] { NewFolder(true); }, false, false, {}, !can},
             {},
             {L"Autres éléments…", [] {  // le sous-menu « Nouveau ▸ » du Shell (Word, raccourci…)
                  POINT pt;
                  GetCursorPos(&pt);
                  ShowFullMenu(pt, {});
              }, false, false, {}, !can}};
    return m;
}

MenuExtra SortMenu() {
    MenuExtra m;
    m.text = L"Trier par";
    for (const ColSpec& c : g_cols) {
        const ColSpec col = c;
        m.sub.push_back({col.title, [col] {
                             if (!SameCol(col, g_sort)) Resort(col);
                         }, false, SameCol(col, g_sort)});
    }
    m.sub.push_back({});
    m.sub.push_back({L"Croissant", [] { if (!g_sortAsc) Resort(g_sort); }, false, g_sortAsc});
    m.sub.push_back({L"Décroissant", [] { if (g_sortAsc) Resort(g_sort); }, false, !g_sortAsc});
    return m;
}

void SetGroupBy(int key) {
    g_groupKey = key;
    g_collapsed.clear();
    SaveFolderView();
    ReArrange();
    if (key >= 0 && !GroupsShown() && HasView() && !g_view.data->isRoot())
        Flash(L"Les groupes s'affichent en Détails et en Contenu");
}

MenuExtra GroupMenu() {
    MenuExtra m;
    m.text = L"Regrouper par";
    m.sub.push_back({L"(Aucun)", [] { SetGroupBy(-1); }, false, g_groupKey < 0});
    for (int k : {COL_NAME, COL_DATE, COL_TYPE, COL_SIZE, COL_CTIME, COL_ATIME})
        m.sub.push_back({Columns_Builtin(k).title, [k] { SetGroupBy(k); }, false, g_groupKey == k});
    if (!g_view.groups.empty()) {
        m.sub.push_back({});
        m.sub.push_back({L"Tout réduire", [] {
                             for (const auto& g : g_view.groups) g_collapsed.insert(g.label);
                             ReArrange();
                         }});
        m.sub.push_back({L"Tout développer", [] {
                             g_collapsed.clear();
                             ReArrange();
                         }});
    }
    return m;
}

MenuExtra KindMenu() {
    MenuExtra m;
    m.text = L"Optimiser ce dossier pour";
    const bool can = HasView() && !FixedColumns() && g_query.empty();
    const std::pair<FolderKind, const wchar_t*> kinds[] = {{FK_GENERAL, L"Éléments généraux"}, {FK_DOCUMENTS, L"Documents"},
                                                           {FK_PICTURES, L"Images"},           {FK_MUSIC, L"Musique"},
                                                           {FK_VIDEOS, L"Vidéos"}};
    for (const auto& [k, text] : kinds) {
        const FolderKind kk = k;
        m.sub.push_back({text, [kk] { SetFolderKind(kk); }, false, g_folderKind == kk, {}, !can});
    }
    return m;
}

void SetCheckboxes(bool on, bool save = true);

void ToggleColumnFilter(int key, const std::wstring& label) {
    auto& keep = g_colFilter[key];
    if (!keep.erase(label)) keep.insert(label);
    if (keep.empty()) g_colFilter.erase(key);
    ReArrange();
}

// Flèche ▾ d'un en-tête : tranches de valeurs présentes, à cocher (plusieurs : l'une ou l'autre).
void ShowColumnFilter(int sub, POINT pt) {
    if (!HasView() || g_view.data->isRoot() || sub < 0 || sub >= (int)g_cols.size()) return;
    const int key = g_cols[(size_t)sub].key;
    if (!Groupable(key)) return;
    const auto f = g_colFilter.find(key);
    const std::set<std::wstring> keep = f != g_colFilter.end() ? f->second : std::set<std::wstring>{};
    std::vector<MenuExtra> v;
    for (const auto& [b, n] : BucketsOf(g_view.sorted, BucketFor(key))) {
        const std::wstring label = b.label;
        v.push_back({MenuText(label) + L"\t" + std::to_wstring(n), [key, label] { ToggleColumnFilter(key, label); }, false,
                     keep.count(label) > 0});
    }
    if (v.empty()) v.push_back({L"Aucune valeur", nullptr, false, false, {}, true});
    if (!keep.empty()) {
        v.push_back({});
        v.push_back({L"Effacer le filtre", [key] {
                         g_colFilter.erase(key);
                         ReArrange();
                     }});
    }
    ShowOwnMenu(v, pt, TPM_LEFTALIGN | TPM_TOPALIGN);
}

// Contenu d'un dossier local pour son info-bulle : « Dossiers : a, b ; Fichiers : c, d… ».
std::wstring FolderPeek(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':' || GetDriveTypeW(path.substr(0, 3).c_str()) != DRIVE_FIXED) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(ToNtPath(Join(path, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::vector<std::wstring> dirs, files;
    bool more = false;
    int seen = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !IsShown(fd.dwFileAttributes, g_prefs)) continue;
        auto& v = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? dirs : files;
        if (v.size() < 5) v.push_back(fd.cFileName);
        else more = true;
        if (++seen > 200) {
            more = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (dirs.empty() && files.empty()) return L"Dossier vide";
    auto list = [&](const std::vector<std::wstring>& v) {
        std::wstring s;
        for (const auto& n : v) s += (s.empty() ? L"" : L", ") + n;
        return s;
    };
    std::wstring s;
    if (!dirs.empty()) s += L"Dossiers : " + list(dirs) + (more && files.empty() ? L"…" : L"");
    if (!files.empty()) s += (s.empty() ? L"" : L"\n") + std::wstring(L"Fichiers : ") + list(files) + (more ? L"…" : L"");
    return s;
}

bool ExplorerFlag(const wchar_t* name, DWORD def) {
    return RegDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", name, def) != 0;
}

// Info-bulle au survol : type, taille, date ; pour un dossier, sa taille si connue et son contenu.
void OnInfoTip(NMLVGETINFOTIPW* t) {
    if (!HasView() || !t->pszText || t->cchTextMax <= 0 || t->iItem < 0 || t->iItem >= RowCount()) return;
    std::wstring s;
    if (const int g = GroupAt(t->iItem); g >= 0) {
        const GroupInfo& gi = g_view.groups[(size_t)g];
        s = gi.label + L" : " + Plural(gi.count, L"élément", L"éléments") +
            (gi.collapsed ? L"\nClic : développer" : L"\nClic : réduire");
        wcsncpy_s(t->pszText, t->cchTextMax, s.c_str(), _TRUNCATE);
        return;
    }
    const int idx = EntryAt(t->iItem);
    const DirData& d = *g_view.data;
    if (idx < 0 || d.isRoot()) return;
    const Entry& e = E(idx);
    wchar_t date[64] = L"";
    FormatDate(e.mtime, date, 64);
    s = L"Type : " + TypeLabel(d, e);
    if (!e.isDir()) s += L"\nTaille : " + BytesText(e.size);
    else if (idx < (int)g_view.dirState.size() && (g_view.dirState[idx] & SIZE_KNOWN))
        s += L"\nTaille : " + BytesText(g_view.dirBytes[idx]);
    if (*date) s += L"\nModifié le : " + std::wstring(date);
    if (e.isDir() && !d.isVirtual() && !d.listOnly() && ExplorerFlag(L"FolderContentsInfoTip", 1))
        if (std::wstring peek = FolderPeek(FullPath(idx)); !peek.empty()) s += L"\n" + peek;
    // Nom tronqué : le ListView l'a mis en tête (dwFlags = 0).
    std::wstring out = (t->dwFlags & LVGIT_UNFOLDED) ? L"" : std::wstring(t->pszText);
    if (!out.empty()) out += L"\n";
    out += s;
    wcsncpy_s(t->pszText, t->cchTextMax, out.c_str(), _TRUNCATE);
}

// Cases à cocher des éléments (case cochée = élément sélectionné).
void SetCheckboxes(bool on, bool save) {
    g_checkboxes = on;
    if (save) {
        const DWORD v = on ? 1 : 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"Checkboxes", REG_DWORD, &v, sizeof v);
    }
    for (HWND lv : g_lists) {
        if (!lv) continue;
        ListView_SetExtendedListViewStyleEx(lv, LVS_EX_CHECKBOXES, on ? LVS_EX_CHECKBOXES : 0);
        ListView_SetCallbackMask(lv, LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED | (on ? LVIS_STATEIMAGEMASK : 0));
        InvalidateRect(lv, nullptr, TRUE);
    }
}

std::vector<MenuExtra> DisplayItems() {
    std::vector<MenuExtra> v = ViewMenu().sub;
    v.push_back({});
    v.push_back({L"Éléments masqués\tCtrl+H", ToggleHidden, false, g_prefs.showHidden});
    v.push_back({L"Extensions de noms de fichiers", ToggleExtensions, false, g_prefs.showExtensions});
    v.push_back({L"Cases à cocher des éléments", [] { SetCheckboxes(!g_checkboxes); }, false, g_checkboxes});
    v.push_back({});
    v.push_back(KindMenu());
    return v;
}

std::vector<MenuExtra> MoreItems() {
    const bool view = HasView();
    std::vector<MenuExtra> v;
    v.push_back({L"Tout sélectionner\tCtrl+A", [] { ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED); },
                 false, false, {}, !view});
    v.push_back({L"Ne rien sélectionner", [] { ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED); }, false, false, {},
                 !view});
    v.push_back({L"Inverser la sélection", InvertSelection, false, false, {}, !view});
    v.push_back({});
    v.push_back({g_undo.empty() ? std::wstring(L"Annuler\tCtrl+Z") : L"Annuler " + StepText(g_undo.back()) + L"\tCtrl+Z",
                 Undo, false, false, {}, g_undo.empty()});
    v.push_back({g_redo.empty() ? std::wstring(L"Rétablir\tCtrl+Y") : L"Rétablir " + StepText(g_redo.back()) + L"\tCtrl+Y",
                 Redo, false, false, {}, g_redo.empty()});
    v.push_back({});
    v.push_back({L"Copier en tant que chemin d'accès\tCtrl+Maj+C",
                 [] { CopyPaths(NamesOf(SelectedEntries())); }, false, false, {}, !view || g_path.empty()});
    v.push_back({L"Ouvrir dans le Terminal", [] { OpenTerminal(g_path); }, false, false, {},
                 !view || g_path.empty() || InVirtual() || InNet()});
    v.push_back({L"Propriétés\tAlt+Entrée", ShowProperties, false, false, {}, !view});
    v.push_back({});
    v.push_back({L"Signets", nullptr, false, false, BookmarkItems()});
    v.push_back({L"Sessions", nullptr, false, false, SessionItems()});
    {
        std::vector<MenuExtra> rec;
        for (const auto& q : SearchHistory_Get()) rec.push_back({MenuText(q), [q] { SearchFor(q); }});
        if (rec.empty()) rec.push_back({L"Aucune recherche récente", nullptr, false, false, {}, true});
        else {
            rec.push_back({});
            rec.push_back({L"Effacer l'historique des recherches", [] {
                               SearchHistory_Clear();
                               Flash(L"Historique des recherches effacé");
                           }});
        }
        v.push_back({L"Recherches récentes", nullptr, false, false, std::move(rec)});
    }
    v.push_back({});
    v.push_back({L"Menu contextuel complet par défaut", [] { SetFullMenu(!g_fullMenu); }, false, g_fullMenu});
    v.push_back({L"Vérifier les copies (relire et comparer)", [] { SetVerifyCopies(!FileOps_Verify()); }, false,
                 FileOps_Verify()});
    return v;
}

// Boutons de la barre : identifiant, texte, info-bulle, menu déroulant.
struct CmdButton {
    int id;
    const wchar_t* text;
    const wchar_t* tip;
    bool dropdown;
};
constexpr CmdButton kCmdButtons[] = {
    {CMD_NEW, L"Nouveau", L"Nouveau dossier ou document", true},
    {0, nullptr, nullptr, false},
    {CMD_CUT, L"Couper", L"Couper (Ctrl+X)", false},
    {CMD_COPY, L"Copier", L"Copier (Ctrl+C)", false},
    {CMD_PASTE, L"Coller", L"Coller (Ctrl+V)", false},
    {CMD_RENAME, L"Renommer", L"Renommer (F2) ; plusieurs éléments : « nom (1) », « nom (2) »…", false},
    {CMD_DELETE, L"Supprimer", L"Supprimer (Suppr) ; Maj+Suppr : définitivement", false},
    {0, nullptr, nullptr, false},
    {CMD_SORT, L"Trier", L"Trier les éléments", true},
    {CMD_VIEW, L"Afficher", L"Disposition et options d'affichage", true},
    {CMD_MORE, L"···", L"Voir plus", true},
};

void UpdateCommandBar();

void CreateCommandBar(HWND parent) {
    g_cmdBar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | CCS_NORESIZE |
                                   CCS_NODIVIDER | CCS_NOPARENTALIGN,
                               0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_CMDBAR, g_inst, nullptr);
    SendMessageW(g_cmdBar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_cmdBar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_HIDECLIPPEDBUTTONS);
    SendMessageW(g_cmdBar, TB_SETIMAGELIST, 0, 0);  // texte seul
}

// (Re)crée les boutons : leur largeur se calcule avec la police du moment.
void FillCommandBar() {
    if (!g_cmdBar) return;
    while (SendMessageW(g_cmdBar, TB_DELETEBUTTON, 0, 0)) {}
    std::vector<TBBUTTON> b;
    for (const CmdButton& c : kCmdButtons) {
        TBBUTTON t{};
        if (!c.id) {
            t.fsStyle = BTNS_SEP;
            t.iBitmap = S(8);
        } else {
            t.iBitmap = I_IMAGENONE;
            t.idCommand = c.id;
            t.fsState = TBSTATE_ENABLED;
            t.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT | (c.dropdown ? BTNS_WHOLEDROPDOWN : 0);
            t.iString = (INT_PTR)c.text;
        }
        b.push_back(t);
    }
    SendMessageW(g_cmdBar, TB_ADDBUTTONSW, b.size(), (LPARAM)b.data());
    // Largeurs mesurées avec notre police (la mesure du contrôle tronque les libellés).
    if (HDC dc = GetDC(g_cmdBar)) {
        HGDIOBJ old = SelectObject(dc, g_font);
        for (const CmdButton& c : kCmdButtons) {
            if (!c.id) continue;
            SIZE sz{};
            GetTextExtentPoint32W(dc, c.text, (int)wcslen(c.text), &sz);
            TBBUTTONINFOW bi{sizeof bi};
            bi.dwMask = TBIF_SIZE;
            bi.cx = (WORD)(sz.cx + S(18) + (c.dropdown ? S(14) : 0));
            SendMessageW(g_cmdBar, TB_SETBUTTONINFOW, c.id, (LPARAM)&bi);
        }
        SelectObject(dc, old);
        ReleaseDC(g_cmdBar, dc);
    }
    SendMessageW(g_cmdBar, TB_AUTOSIZE, 0, 0);
    UpdateCommandBar();
}

// Grise ce qui ne s'applique pas ici (sélection, presse-papiers, emplacement).
void UpdateCommandBar() {
    if (!g_cmdBar) return;
    const bool sel = HasView() && ListView_GetSelectedCount(g_list) > 0;
    const bool mod = CanModify();
    auto on = [](int id, bool e) { SendMessageW(g_cmdBar, TB_ENABLEBUTTON, id, MAKELONG(e ? TRUE : FALSE, 0)); };
    on(CMD_NEW, mod);
    on(CMD_CUT, sel && mod);
    on(CMD_COPY, sel && !InNet());
    on(CMD_PASTE, mod && ClipboardHasFiles());
    on(CMD_RENAME, sel && mod);
    on(CMD_DELETE, sel && (mod || InBin() || (HasView() && g_view.data->path == kNetPath)));
    on(CMD_SORT, HasView());
    on(CMD_VIEW, true);
    on(CMD_MORE, true);
}

int CommandBarHeight() {
    if (!g_cmdBar) return 0;
    const DWORD sz = (DWORD)SendMessageW(g_cmdBar, TB_GETBUTTONSIZE, 0, 0);
    return std::max<int>(HIWORD(sz), g_fontH + S(10));
}

// Clic sur un bouton de la barre.
bool OnCommandBar(int id) {
    switch (id) {
        case CMD_CUT: CopyNames(NamesOf(SelectedEntries()), true); break;
        case CMD_COPY: CopyNames(NamesOf(SelectedEntries()), false); break;
        case CMD_PASTE: Paste(g_path); break;
        case CMD_RENAME: BeginRename(); break;
        case CMD_DELETE: DeleteNames(NamesOf(SelectedEntries()), GetKeyState(VK_SHIFT) < 0); break;
        default: return false;
    }
    return true;
}

// Bouton à menu : le menu s'ouvre sous le bouton.
void OnCommandDropdown(int id) {
    RECT r{};
    SendMessageW(g_cmdBar, TB_GETRECT, id, (LPARAM)&r);
    MapWindowPoints(g_cmdBar, nullptr, (POINT*)&r, 2);
    const POINT pt{r.left, r.bottom};
    std::vector<MenuExtra> items;
    switch (id) {
        case CMD_NEW: items = NewMenu().sub; break;
        case CMD_SORT:
            items = SortMenu().sub;
            items.push_back({});
            items.push_back(GroupMenu());
            break;
        case CMD_VIEW: items = DisplayItems(); break;
        case CMD_MORE: items = MoreItems(); break;
        default: return;
    }
    SendMessageW(g_cmdBar, TB_PRESSBUTTON, id, MAKELONG(TRUE, 0));
    ShowOwnMenu(items, pt, TPM_LEFTALIGN | TPM_TOPALIGN);
    if (IsWindow(g_cmdBar)) SendMessageW(g_cmdBar, TB_PRESSBUTTON, id, MAKELONG(FALSE, 0));
    if (GetFocus() != g_addr && GetFocus() != g_searchBox) SetFocus(g_list);
}

const wchar_t* CommandTip(int id) {
    for (const CmdButton& c : kCmdButtons)
        if (c.id == id) return c.tip;
    return nullptr;
}

// Le menu rapide s'applique-t-il ? (dossier ordinaire, sans Maj, réglage par défaut)
// ───────────────────────────── Bloc 28 : opérations ─────────────────────────────

// « Copier vers… » / « Déplacer vers… » : choix d'un dossier (le dernier choisi est proposé).
void CopyMoveTo(const std::vector<int>& entries, bool move) {
    if (entries.empty() || !HasView() || (move && !CanModify() && g_query.empty())) return;
    std::vector<std::wstring> paths;
    for (int e : entries)
        if (std::wstring p = FullPath(e); !p.empty() && !IsVirtualPath(p)) paths.push_back(std::move(p));
    if (paths.empty()) return;
    wchar_t last[1024] = {};
    DWORD cb = sizeof last;
    std::wstring initial = g_path;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"LastCopyTo", RRF_RT_REG_SZ, nullptr, last, &cb) ==
            ERROR_SUCCESS &&
        GetFileAttributesW(last) != INVALID_FILE_ATTRIBUTES)
        initial = last;
    const std::wstring to = PickFolder(initial, move ? L"Déplacer vers" : L"Copier vers");
    if (to.empty()) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"LastCopyTo", REG_SZ, to.c_str(),
                    (DWORD)((to.size() + 1) * sizeof(wchar_t)));
    FileOp op;
    op.kind = move ? FileOpKind::Move : FileOpKind::Copy;
    op.folder = to;
    op.paths = std::move(paths);
    const size_t n = op.paths.size();
    StartOp(std::move(op));
    Flash((move ? L"Déplacement de " : L"Copie de ") + Plural(n, L"élément", L"éléments") + L" vers " + to);
}

// « Coller en tant que raccourci » : un raccourci vers chaque élément du presse-papiers.
void PasteShortcut(const std::wstring& dest) {
    if (!CanModify() || dest.empty()) return;
    FileOp op;
    bool cut = false;
    DWORD seq = 0;
    if (!Clipboard_Get(op.items, cut, seq)) {
        SetStatus(L"Rien à coller : le presse-papiers ne contient pas de fichiers.");
        return;
    }
    op.kind = FileOpKind::Link;
    op.folder = dest;
    StartOp(std::move(op));
}

// ── Qui utilise ce fichier ? ──

// Fenêtre principale visible d'un processus (pour y basculer ou lui demander de se fermer).
std::vector<HWND> WindowsOf(DWORD pid) {
    struct Ctx {
        DWORD pid;
        std::vector<HWND> out;
    } c{pid, {}};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) c->out.push_back(h);
            return TRUE;
        },
        (LPARAM)&c);
    return c.out;
}

void WhoUses(const std::vector<int>& entries) {
    std::vector<std::wstring> paths;
    for (int e : entries)
        if (std::wstring p = FullPath(e); !p.empty() && !IsVirtualPath(p)) paths.push_back(std::move(p));
    if (paths.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::vector<Locker> lockers = FileOps_LockerList(paths);
    const std::wstring what = paths.size() == 1 ? L"« " + LeafOf(paths[0]) + L" »" : Plural(paths.size(), L"élément", L"éléments");
    if (lockers.empty()) {
        Flash(L"Aucune application n'utilise " + what + L" (selon le Gestionnaire de redémarrage de Windows)");
        return;
    }
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> radios;
    for (size_t i = 0; i < lockers.size(); ++i) {
        const Locker& l = lockers[i];
        std::wstring s = l.app + L"  (processus " + std::to_wstring(l.pid) + L")";
        if (l.service) s += L" — service";
        if (l.explorer) s += L" — Explorateur Windows";
        labels.push_back(std::move(s));
    }
    for (size_t i = 0; i < labels.size(); ++i) radios.push_back({(int)(1000 + i), labels[i].c_str()});
    enum { ID_SWITCH = 100, ID_CLOSE = 101 };
    TASKDIALOG_BUTTON buttons[] = {{ID_SWITCH, L"Basculer vers l'application"},
                                   {ID_CLOSE, L"Demander la fermeture"},
                                   {IDCANCEL, L"Fermer"}};
    const std::wstring main = what + (paths.size() == 1 ? L" est utilisé par " : L" sont utilisés par ") +
                              Plural(lockers.size(), L"application", L"applications");
    const std::wstring content =
        L"Ces applications gardent le fichier ouvert : il ne peut pas être supprimé, déplacé ou renommé tant "
        L"qu'elles ne l'ont pas fermé. « Demander la fermeture » fait comme la croix de la fenêtre : "
        L"l'application peut proposer d'enregistrer.";
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = g_hwnd;
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"Qui utilise ce fichier ?";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = _countof(buttons);
    c.pButtons = buttons;
    c.nDefaultButton = ID_SWITCH;
    c.cRadioButtons = (UINT)radios.size();
    c.pRadioButtons = radios.data();
    c.nDefaultRadioButton = 1000;
    int pressed = IDCANCEL, radio = 1000;
    if (FAILED(TaskDialogIndirect(&c, &pressed, &radio, nullptr)) || pressed == IDCANCEL) return;
    const size_t k = (size_t)(radio - 1000);
    if (k >= lockers.size()) return;
    const Locker& l = lockers[k];
    const std::vector<HWND> wins = WindowsOf(l.pid);
    if (pressed == ID_SWITCH) {
        if (wins.empty()) {
            Flash(l.app + L" n'a pas de fenêtre visible");
            return;
        }
        if (IsIconic(wins[0])) ShowWindow(wins[0], SW_RESTORE);
        SetForegroundWindow(wins[0]);
    } else if (pressed == ID_CLOSE) {
        if (l.service || l.explorer || wins.empty()) {
            Flash(l.app + L" ne se ferme pas depuis ici (pas de fenêtre, service ou Explorateur)");
            return;
        }
        for (HWND w : wins) PostMessageW(w, WM_CLOSE, 0, 0);
        Flash(L"Fermeture demandée à " + l.app);
    }
}

// ── Raccourci dont la cible a disparu ──

// true : traité (cible retrouvée et ouverte, recherche lancée, raccourci supprimé ou abandon).
bool BrokenShortcut(const std::wstring& lnk, const std::wstring& target) {
    const DWORD a = GetFileAttributesW(target.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && err != ERROR_INVALID_DRIVE &&
        err != ERROR_NOT_READY && err != ERROR_BAD_NETPATH && err != ERROR_BAD_NET_NAME)
        return false;  // accès refusé… : le Shell saura le dire
    std::wstring root = target.size() >= 3 && target[1] == L':' ? target.substr(0, 3) : std::wstring();
    const bool driveGone = !root.empty() && GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
    const std::wstring leaf = LeafOf(target);
    enum { ID_RESOLVE = 100, ID_SEARCH = 101, ID_DELETE = 102 };
    const std::wstring searchText = L"Chercher « " + leaf + L" » dans tout le PC";
    TASKDIALOG_BUTTON buttons[] = {{ID_RESOLVE, L"Retrouver la cible\nComme Windows : suivi des fichiers déplacés"},
                                   {ID_SEARCH, searchText.c_str()},
                                   {ID_DELETE, L"Supprimer le raccourci"},
                                   {IDCANCEL, L"Annuler"}};
    const std::wstring content = (driveGone ? L"Le lecteur " + root + L" n'est pas disponible.\n\n" : L"") +
                                 L"Cible : " + target;
    const std::wstring main = L"La cible du raccourci « " + Archive_Stem(LeafOf(lnk)) + L" » a disparu";
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = g_hwnd;
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    c.pszWindowTitle = L"Raccourci";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = _countof(buttons);
    c.pButtons = buttons;
    c.nDefaultButton = ID_RESOLVE;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return false;
    if (pressed == ID_RESOLVE) {
        IShellLinkW* sl = nullptr;
        IPersistFile* pf = nullptr;
        bool found = false;
        std::wstring now;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
            SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(lnk.c_str(), STGM_READWRITE))) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            // Sans fenêtre du Shell, 5 s au plus ; le raccourci est mis à jour s'il a retrouvé la cible.
            if (sl->Resolve(g_hwnd, SLR_NO_UI | SLR_UPDATE | SLR_ANY_MATCH | (5000u << 16)) == S_OK) {
                wchar_t buf[2048] = {};
                if (sl->GetPath(buf, _countof(buf), nullptr, 0) == S_OK && buf[0] &&
                    GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) {
                    now = buf;
                    found = true;
                }
            }
        }
        if (pf) pf->Release();
        if (sl) sl->Release();
        if (!found) {
            Flash(L"Cible introuvable : essayez « Chercher dans tout le PC »");
            return true;
        }
        Flash(L"Cible retrouvée : " + now);
        const DWORD na = GetFileAttributesW(now.c_str());
        if (na & FILE_ATTRIBUTE_DIRECTORY) Navigate(now, Nav::Normal);
        else OpenWithShell(now);
    } else if (pressed == ID_SEARCH) {
        SetWindowTextW(g_searchBox, leaf.c_str());
        g_filter.clear();
        Navigate(L"", Nav::Normal, {}, {}, {}, false, &leaf);  // Ce PC : tout le PC (index si disponible)
    } else if (pressed == ID_DELETE) {
        std::wstring parent, name;
        if (ParentOf(lnk, parent, name) && _wcsicmp(parent.c_str(), g_path.c_str()) == 0) DeleteNames({name}, false);
    }
    return true;
}

// ── Renommage par lot ──

struct BatchState {
    std::vector<RenameItem> items;
    std::vector<std::wstring> others;  // noms du dossier non renommés
    std::wstring folderName;
    RenameRule rule;
    std::vector<std::wstring> names, problems;
    std::wstring error;
    std::wstring dispBuf;
};

enum {
    ID_BR_FIND = 200, ID_BR_REPL, ID_BR_REGEX, ID_BR_MATCHCASE, ID_BR_WITHEXT, ID_BR_PATTERN, ID_BR_CASE, ID_BR_START,
    ID_BR_STEP, ID_BR_LIST, ID_BR_INFO, ID_BR_HELP
};

std::wstring DlgText(HWND dlg, int id) {
    HWND c = GetDlgItem(dlg, id);
    std::wstring t(GetWindowTextLengthW(c) + 1, L'\0');
    t.resize(GetWindowTextW(c, t.data(), (int)t.size()));
    return t;
}

void BatchUpdate(HWND dlg, BatchState& st) {
    RenameRule& r = st.rule;
    r.find = DlgText(dlg, ID_BR_FIND);
    r.replace = DlgText(dlg, ID_BR_REPL);
    r.regex = IsDlgButtonChecked(dlg, ID_BR_REGEX) == BST_CHECKED;
    r.matchCase = IsDlgButtonChecked(dlg, ID_BR_MATCHCASE) == BST_CHECKED;
    r.withExtension = IsDlgButtonChecked(dlg, ID_BR_WITHEXT) == BST_CHECKED;
    r.pattern = DlgText(dlg, ID_BR_PATTERN);
    r.nameCase = (NameCase)std::max(0, (int)SendDlgItemMessageW(dlg, ID_BR_CASE, CB_GETCURSEL, 0, 0));
    r.start = _wtoi(DlgText(dlg, ID_BR_START).c_str());
    const std::wstring step = DlgText(dlg, ID_BR_STEP);
    r.step = step.empty() ? 1 : _wtoi(step.c_str());
    st.names = BatchNames(r, st.items, st.folderName, &st.error);
    st.problems = BatchProblems(st.names, st.others);
    size_t changed = 0, bad = 0;
    for (size_t i = 0; i < st.names.size(); ++i) {
        if (st.names[i] == st.items[i].name) {
            st.problems[i].clear();  // inchangé : rien à vérifier
            continue;
        }
        if (!st.problems[i].empty()) ++bad;
        else ++changed;
    }
    std::wstring info;
    if (!st.error.empty()) info = st.error;
    else {
        info = Plural(changed, L"élément renommé", L"éléments renommés");
        if (st.names.size() - changed - bad) info += L", " + Plural(st.names.size() - changed - bad, L"inchangé", L"inchangés");
        if (bad) info += L", " + Plural(bad, L"nom refusé", L"noms refusés") + L" (voir « État »)";
    }
    SetDlgItemTextW(dlg, ID_BR_INFO, info.c_str());
    EnableWindow(GetDlgItem(dlg, IDOK), st.error.empty() && changed > 0 && bad == 0);
    HWND list = GetDlgItem(dlg, ID_BR_LIST);
    ListView_SetItemCountEx(list, (int)st.items.size(), LVSICF_NOSCROLL);
    InvalidateRect(list, nullptr, FALSE);
}

INT_PTR CALLBACK BatchProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (BatchState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG: {
            st = (BatchState*)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            const HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                            DWORD ex = 0) {
                RECT r{x, y, x + w, y + h};
                MapDialogRect(dlg, &r);
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                                         r.bottom - r.top, dlg, (HMENU)(INT_PTR)id, nullptr, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
                return c;
            };
            const DWORD edit = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
            make(L"STATIC", L"Rechercher :", SS_LEFT, -1, 7, 9, 58, 10);
            HWND find = make(L"EDIT", L"", edit, ID_BR_FIND, 66, 7, 140, 13);
            make(L"STATIC", L"Remplacer par :", SS_LEFT, -1, 214, 9, 58, 10);
            make(L"EDIT", L"", edit, ID_BR_REPL, 273, 7, 140, 13);
            make(L"BUTTON", L"Expression régulière", WS_TABSTOP | BS_AUTOCHECKBOX, ID_BR_REGEX, 66, 24, 100, 12);
            make(L"BUTTON", L"Respecter la casse", WS_TABSTOP | BS_AUTOCHECKBOX, ID_BR_MATCHCASE, 170, 24, 90, 12);
            make(L"BUTTON", L"Inclure l'extension", WS_TABSTOP | BS_AUTOCHECKBOX, ID_BR_WITHEXT, 264, 24, 100, 12);
            make(L"STATIC", L"Nouveau nom :", SS_LEFT, -1, 7, 43, 58, 10);
            make(L"EDIT", L"{nom}", edit, ID_BR_PATTERN, 66, 41, 140, 13);
            make(L"STATIC", L"Casse :", SS_LEFT, -1, 214, 43, 58, 10);
            HWND cs = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_BR_CASE, 273, 41, 140, 90);
            for (const wchar_t* s : {L"Inchangée", L"minuscules", L"MAJUSCULES", L"Majuscule À Chaque Mot",
                                     L"Majuscule au début"})
                SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(cs, CB_SETCURSEL, 0, 0);
            make(L"STATIC", L"{nom}  {n}  {n:3}  {ext}  {date}  {dossier}", SS_LEFT | SS_NOPREFIX, ID_BR_HELP, 66, 57,
                 200, 10);
            make(L"STATIC", L"Compteur :", SS_LEFT, -1, 214, 59, 58, 10);
            make(L"EDIT", L"1", edit | ES_NUMBER, ID_BR_START, 273, 57, 40, 13);
            make(L"STATIC", L"pas", SS_RIGHT, -1, 318, 59, 20, 10);
            make(L"EDIT", L"1", edit, ID_BR_STEP, 342, 57, 30, 13);
            HWND list = make(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                             ID_BR_LIST, 7, 76, 406, 124);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(list, L"Explorer", nullptr);
            RECT lr;
            GetClientRect(list, &lr);
            const int w = lr.right - lr.left - GetSystemMetrics(SM_CXVSCROLL);
            LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH};
            col.cx = w * 38 / 100;
            col.pszText = (LPWSTR)L"Nom actuel";
            ListView_InsertColumn(list, 0, &col);
            col.pszText = (LPWSTR)L"Nouveau nom";
            ListView_InsertColumn(list, 1, &col);
            col.cx = w - 2 * (w * 38 / 100);
            col.pszText = (LPWSTR)L"État";
            ListView_InsertColumn(list, 2, &col);
            make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_BR_INFO, 7, 211, 290, 10);
            make(L"BUTTON", L"Renommer", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 309, 208, 50, 14);
            make(L"BUTTON", L"Annuler", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 363, 208, 50, 14);
            BatchUpdate(dlg, *st);
            // Centré sur la fenêtre, mais jamais hors de l'écran où elle se trouve.
            RECT dr, ow;
            MONITORINFO mi{sizeof mi};
            if (GetWindowRect(dlg, &dr) && GetWindowRect(g_hwnd, &ow) &&
                GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                const int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
                int x = (ow.left + ow.right - dw) / 2, y = (ow.top + ow.bottom - dh) / 2;
                x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - dw));
                y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - dh));
                SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            SetFocus(find);
            return FALSE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDOK) {
                BatchUpdate(dlg, *st);
                if (IsWindowEnabled(GetDlgItem(dlg, IDOK))) EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            if ((code == EN_CHANGE && id != ID_BR_INFO) || code == BN_CLICKED || code == CBN_SELCHANGE)
                BatchUpdate(dlg, *st);
            break;
        }
        case WM_NOTIFY: {
            auto* nm = (NMHDR*)lp;
            if (nm->idFrom == ID_BR_LIST && nm->code == LVN_GETDISPINFOW) {
                auto* di = (NMLVDISPINFOW*)lp;
                const size_t i = (size_t)di->item.iItem;
                if (!(di->item.mask & LVIF_TEXT) || i >= st->items.size()) break;
                const bool same = i < st->names.size() && st->names[i] == st->items[i].name;
                switch (di->item.iSubItem) {
                    case 0: st->dispBuf = st->items[i].name; break;
                    case 1: st->dispBuf = i < st->names.size() ? st->names[i] : L""; break;
                    default:
                        st->dispBuf = same ? L"inchangé" : (i < st->problems.size() && !st->problems[i].empty() ? st->problems[i] : L"✓");
                        break;
                }
                di->item.pszText = st->dispBuf.data();
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

// Renommage par lot : éléments sélectionnés (sinon tout le dossier), dans l'ordre affiché.
void BatchRename() {
    if (!CanModify()) return;
    std::vector<int> sel = SelectedEntries();
    if (sel.size() < 2) {
        sel.clear();
        for (uint32_t i : g_view.order)
            if (!IsGroupRow(i)) sel.push_back((int)i);
    } else {
        std::vector<int> ordered;
        std::unordered_set<int> picked(sel.begin(), sel.end());
        for (uint32_t i : g_view.order)
            if (!IsGroupRow(i) && picked.count((int)i)) ordered.push_back((int)i);
        sel = std::move(ordered);
    }
    if (sel.empty()) return;
    BatchState st;
    std::unordered_set<int> picked(sel.begin(), sel.end());
    for (int e : sel) st.items.push_back({NameOf(e), E(e).isDir(), E(e).mtime});
    for (int k = 0; k < (int)g_view.data->entries.size(); ++k)
        if (!picked.count(k)) st.others.emplace_back(NameOf(k));
    st.folderName = LeafOf(g_path);
    std::vector<WORD> t(sizeof(DLGTEMPLATE) / sizeof(WORD), 0);
    auto* d = (DLGTEMPLATE*)t.data();
    d->style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cx = 420;
    d->cy = 229;
    t.push_back(0);
    t.push_back(0);
    for (const wchar_t* s = L"Renommer par lot";; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    t.push_back(9);
    for (const wchar_t* s = L"Segoe UI";; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    if (DialogBoxIndirectParamW(g_inst, (DLGTEMPLATE*)t.data(), g_hwnd, BatchProc, (LPARAM)&st) != IDOK) return;
    FileOp op;
    op.kind = FileOpKind::Rename;
    op.folder = g_path;
    for (size_t i = 0; i < st.items.size() && i < st.names.size(); ++i)
        if (st.names[i] != st.items[i].name) {
            op.paths.push_back(Join(g_path, st.items[i].name.c_str()));
            op.newNames.push_back(st.names[i]);
        }
    if (op.paths.empty()) return;
    const size_t n = op.paths.size();
    StartOp(std::move(op));
    Flash(L"Renommage de " + Plural(n, L"élément", L"éléments") + L" (Ctrl+Z pour annuler)");
}

void SetVerifyCopies(bool on) {
    FileOps_SetVerify(on);
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"VerifyCopies", REG_DWORD, &v, sizeof v);
    Flash(on ? L"Les copies seront relues et comparées à l'original" : L"Copies non vérifiées (plus rapide)");
}

bool QuickMenuApplies(bool shift) {
    return !shift && !g_fullMenu && HasView() && !g_path.empty() && !g_view.data->isRoot() && !InVirtual() &&
           !g_view.data->listOnly() && g_query.empty();
}

void ShowQuickMenu(POINT pt, const std::vector<int>& sel) {
    const std::vector<std::wstring> names = NamesOf(sel);
    const bool mod = CanModify(), clip = ClipboardHasFiles();
    std::vector<MenuExtra> v;
    auto more = [names, pt] { ShowFullMenu(pt, FindEntriesByNames(names)); };
    if (sel.empty()) {  // fond du dossier
        MenuExtra view = ViewMenu();
        view.text = L"Affichage";
        v.push_back(view);
        v.push_back(SortMenu());
    v.push_back(GroupMenu());
        v.push_back({L"Actualiser\tF5", [] { g_forceSizes = true; Refresh(); }});
        v.push_back({});
        v.push_back({L"Coller\tCtrl+V", [] { Paste(g_path); }, false, false, {}, !mod || !clip});
        v.push_back({L"Coller en tant que raccourci", [] { PasteShortcut(g_path); }, false, false, {}, !mod || !clip});
        if (!g_undo.empty()) v.push_back({L"Annuler " + StepText(g_undo.back()) + L"\tCtrl+Z", Undo});
        if (!g_redo.empty()) v.push_back({L"Rétablir " + StepText(g_redo.back()) + L"\tCtrl+Y", Redo});
        v.push_back(NewMenu());
        v.push_back({});
        v.push_back({L"Ouvrir dans le Terminal", [] { OpenTerminal(g_path); }});
        v.push_back({L"Copier en tant que chemin d'accès\tCtrl+Maj+C", [] { CopyPaths({}); }});
        for (auto& x : PinExtras(g_path, false)) v.push_back(std::move(x));
        v.push_back({L"Propriétés\tAlt+Entrée", ShowProperties});
    } else {
        const int single = sel.size() == 1 ? sel[0] : -1;
        const bool dir = single >= 0 && E(single).isDir();
        v.push_back({L"Ouvrir", [names] {
                         const auto es = FindEntriesByNames(names);
                         if (es.size() == 1) ActivateEntry(es[0]);
                         else
                             for (int e : es)
                                 if (!E(e).isDir()) OpenWithShell(FullPath(e));
                     }, false, false, {}, false, true});
        if (dir) {
            const std::wstring path = FullPath(single);
            v.push_back({L"Ouvrir dans un nouvel onglet", [path] { NewTab(path, true); }});
            v.push_back({L"Ouvrir dans une nouvelle fenêtre", [path] { NewWindow(path); }});
            v.push_back({L"Ouvrir dans le Terminal", [path] { OpenTerminal(path); }});
        } else if (single >= 0) {
            const std::wstring here = g_path, name = names[0];
            v.push_back({L"Ouvrir avec…", [here, name] {
                             if (!ShellMenu_Invoke(g_hwnd, here, {name}, "openas")) SetStatus(L"« Ouvrir avec » indisponible.");
                         }});
        }
        v.push_back({});
        v.push_back({L"Couper\tCtrl+X", [names] { CopyNames(names, true); }, false, false, {}, !mod});
        v.push_back({L"Copier\tCtrl+C", [names] { CopyNames(names, false); }});
        v.push_back({L"Copier vers…", [names] { CopyMoveTo(FindEntriesByNames(names), false); }});
        v.push_back({L"Déplacer vers…", [names] { CopyMoveTo(FindEntriesByNames(names), true); }, false, false, {}, !mod});
        if (dir) {
            const std::wstring path = FullPath(single);
            v.push_back({L"Coller dans ce dossier", [path] { Paste(path); }, false, false, {}, !mod || !clip});
        }
        v.push_back({names.size() > 1 ? L"Renommer tout\tF2" : L"Renommer\tF2", [] { BeginRename(); }, false, false, {},
                     !mod});
        v.push_back({L"Renommer par lot…\tCtrl+Maj+F2", BatchRename, false, false, {}, !mod});
        v.push_back({L"Supprimer\tSuppr", [names] { DeleteNames(names, GetKeyState(VK_SHIFT) < 0); }, false, false, {},
                     !mod});
        v.push_back({});
        v.push_back({L"Copier en tant que chemin d'accès\tCtrl+Maj+C", [names] { CopyPaths(names); }});
        v.push_back({names.size() == 1 && !dir ? L"Qui utilise ce fichier ?" : L"Qui utilise ces éléments ?",
                     [names] { WhoUses(FindEntriesByNames(names)); }});
        if (single >= 0 && !dir && Archive_IsArchive(names[0])) {
            const std::wstring arc = FullPath(single), here = g_path;
            v.push_back({L"Extraire tout", [arc, here] { ExtractArchive(arc, here, true, ThenDo::Nothing); }});
            v.push_back({L"Extraire vers…", [arc, here] {
                             const std::wstring to = PickFolder(here, L"Extraire vers");
                             if (!to.empty()) ExtractArchive(arc, to, false, ThenDo::Nothing);
                         }});
        }
        v.push_back({L"Compresser dans un fichier ZIP", [names] { CompressEntries(FindEntriesByNames(names)); }, false,
                     false, {}, !mod});
        if (dir)
            for (auto& x : PinExtras(FullPath(single), false)) v.push_back(std::move(x));
        v.push_back({L"Propriétés\tAlt+Entrée", ShowProperties});
    }
    v.push_back({});
    v.push_back({L"Afficher plus d'options\tMaj+F10", more});
    ShellMenu_Prepare(g_hwnd, g_path, names, false);  // « Afficher plus d'options » : prêt d'avance
    ShowOwnMenu(v, pt);
    SetTimer(g_hwnd, TIMER_PINNED, 800, nullptr);
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

// ───────────────────────────── Recherche instantanée (service) ─────────────────────────────

constexpr wchar_t kAppKey[] = L"Software\\FastExplorer";

bool IndexAsked() {
    DWORD v = 0, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"IndexAsked", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v;
}

void SetIndexAsked() {
    DWORD v = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"IndexAsked", REG_DWORD, &v, sizeof v);
}

// Installe ou retire le service, dans un process élevé (une demande d'autorisation de Windows).
void RunIndexSetup(bool install) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring args = install ? L"--install-index " + Index_CurrentUserSid() : L"--uninstall-index";
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        SetStatus(e == ERROR_CANCELLED ? std::wstring(L"Recherche instantanée : autorisation refusée.")
                                       : L"Recherche instantanée : " + ErrorText(e));
        return;
    }
    SetStatus(install ? L"Installation du service de recherche…" : L"Retrait du service de recherche…");
    if (!sei.hProcess) return;
    std::thread([p = sei.hProcess, hwnd = g_hwnd, install] {
        WaitForSingleObject(p, 120000);
        DWORD code = ERROR_TIMEOUT;
        GetExitCodeProcess(p, &code);
        CloseHandle(p);
        PostMessageW(hwnd, WM_APP_INDEXDONE, code, install ? 1 : 0);
    }).detach();
}

void OnIndexDone(DWORD code, bool install) {
    if (code) {
        SetStatus(std::wstring(install ? L"Activation" : L"Désactivation") + L" de la recherche instantanée impossible : " +
                  ErrorText(code));
        return;
    }
    Flash(install ? L"Recherche instantanée activée : l'index se construit (quelques secondes), puis toute recherche "
                    L"est immédiate, même dans tout le PC (Ce PC)."
                  : L"Recherche instantanée désactivée : service retiré.");
}

HRESULT CALLBACK IndexAskCallback(HWND h, UINT n, WPARAM, LPARAM, LONG_PTR) {
    if (n == TDN_CREATED) SendMessageW(h, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, 100, TRUE);  // bouclier
    return S_OK;
}

// Une seule fois : proposer d'activer la recherche instantanée.
void AskIndex() {
    if (IndexAsked() || Index_ServiceInstalled()) return;
    SetIndexAsked();
    TASKDIALOG_BUTTON buttons[] = {
        {100, L"Activer la recherche instantanée\nWindows demandera une autorisation administrateur, une seule fois."},
        {101, L"Plus tard\nActivable à tout moment depuis le menu de la fenêtre (Alt+Espace)."}};
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = g_hwnd;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"FastExplorer";
    c.pszMainInstruction = L"Trouver n'importe quel fichier en un instant ?";
    c.pszContent =
        L"FastExplorer peut tenir à jour la liste des fichiers de vos disques, comme Everything : la recherche devient "
        L"instantanée, dans un dossier comme dans tout le PC.\n\nUn petit service Windows est installé. Il lit seulement "
        L"la liste des fichiers, ne modifie rien et ne répond qu'à votre compte.";
    c.pButtons = buttons;
    c.cButtons = 2;
    c.nDefaultButton = 100;
    c.pfCallback = IndexAskCallback;
    int pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr)) && pressed == 100) RunIndexSetup(true);
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
    for (HWND h : {g_lists[0], g_lists[1], g_paneHdr[0], g_paneHdr[1], g_addr, g_status, g_btnBack, g_btnFwd, g_btnUp,
                   g_btnRefresh, g_nav, g_crumbs, g_searchBox, g_tabBar, g_pane, g_cmdBar})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    FillCommandBar();  // boutons à la taille du texte
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
    g_smallPick = pick;
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
    HDWP dwp = BeginDeferWindowPos(16);
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
    // Barre de commandes sous l'adresse.
    const int cmdY = y0 + h + S(3), cmdH = CommandBarHeight();
    if (g_cmdBar) place(g_cmdBar, m, cmdY, std::max(0, (int)rc.right - 2 * m), cmdH);
    const int top = cmdY + cmdH + S(3);
    const int bodyH = std::max(0, (int)rc.bottom - top - statusH);
    // Volet de navigation, séparateur (déplaçable à la souris), liste.
    const int split = S(5);
    const int navW = std::clamp(S(g_navW96), S(120), std::max(S(120), (int)rc.right / 2));
    place(g_nav, 0, top, navW, bodyH);
    g_split = {navW, top, navW + split, top + bodyH};
    // Volet d'aperçu / de détails à droite, avec son propre séparateur.
    int paneW = 0;
    if (g_pane && Pane_Mode() != PaneMode::None) {
        const int room = std::max(0, (int)rc.right - navW - split - S(160) - split);
        paneW = std::min(std::clamp(S(g_paneW96), S(160), std::max(S(160), (int)rc.right * 2 / 3)), room);
    }
    const int listW = std::max(0, (int)rc.right - navW - split - (paneW ? paneW + split : 0));
    const int listX = navW + split;
    if (g_dual) {  // deux listes, chacune sous son chemin
        const int hh = g_fontH + S(8), gap = S(4), w0 = std::max(0, (listW - gap) / 2);
        place(g_paneHdr[0], listX, top, w0, hh);
        place(g_lists[0], listX, top + hh, w0, std::max(0, bodyH - hh));
        place(g_paneHdr[1], listX + w0 + gap, top, std::max(0, listW - w0 - gap), hh);
        place(g_lists[1], listX + w0 + gap, top + hh, std::max(0, listW - w0 - gap), std::max(0, bodyH - hh));
    } else {
        place(g_list, listX, top, listW, bodyH);
    }
    if (paneW) {
        g_paneSplit = {navW + split + listW, top, navW + split + listW + split, top + bodyH};
        place(g_pane, g_paneSplit.right, top, paneW, bodyH);
    } else {
        g_paneSplit = {};
    }
    if (dwp) EndDeferWindowPos(dwp);
    ApplyTileInfo();  // contenu : cases sur toute la largeur
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
    CreateCommandBar(hwnd);
    g_fullMenu = RegDword(L"Software\\FastExplorer", L"FullMenu", 0) != 0;
    FileOps_SetVerify(RegDword(L"Software\\FastExplorer", L"VerifyCopies", 0) != 0);

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
    crumbs.roots = [] {
        std::vector<Crumb> r;
        auto known = [&](REFKNOWNFOLDERID id, const wchar_t* text) {
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) {
                std::wstring path = p;
                r.push_back({text ? std::wstring(text) : LeafOf(path), path});
            }
            if (p) CoTaskMemFree(p);
        };
        r.push_back({L"Accueil", kHomePath});
        known(FOLDERID_Desktop, L"Bureau");
        known(FOLDERID_Profile, nullptr);
        r.push_back({L"Ce PC", L""});
        r.push_back({L"Réseau", kNetPath});
        r.push_back({L"Corbeille", kBinPath});
        return r;
    };
    g_crumbs = Crumbs_Create(hwnd, ID_CRUMBS, std::move(crumbs));
    g_searchBox = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                                  0, 0, hwnd, (HMENU)ID_SEARCH, g_inst, nullptr);
    Edit_SetCueBannerTextFocused(g_searchBox, L"Filtrer, Entrée : rechercher (Ctrl+F)", TRUE);
    AttachSearchHistory();

    for (int k = 0; k < 2; ++k) {  // la seconde ne sert qu'en double panneau
        HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | (k == 0 ? WS_VISIBLE : 0) | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                      LVS_SHAREIMAGELISTS | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(k == 0 ? ID_LIST : ID_LIST2), g_inst, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                  (ExplorerFlag(L"ShowInfoTip", 1) ? LVS_EX_INFOTIP : 0));
        ListView_SetCallbackMask(lv, LVIS_OVERLAYMASK | LVIS_CUT | LVIS_DROPHILITED);  // fournis par LVN_GETDISPINFO
        SetWindowTheme(lv, L"Explorer", nullptr);
        g_lists[k] = lv;
        g_paneHdr[k] = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_NOTIFY | SS_PATHELLIPSIS | SS_CENTERIMAGE | SS_NOPREFIX,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)(ID_HDR0 + k), g_inst, nullptr);
    }
    g_list = g_lists[0];
    SetCheckboxes(RegDword(L"Software\\FastExplorer", L"Checkboxes", ExplorerFlag(L"AutoCheckSelect", 0)) != 0, false);

    ApplyColumns(g_cols);

    NavHost nav;
    nav.navigate = [](const std::wstring& path) { Navigate(path, Nav::Normal); };
    nav.contextMenu = ShowNavMenu;
    nav.status = [](const std::wstring& text) { SetStatus(text); };
    g_nav = NavPane_Create(hwnd, ID_NAV, WM_APP_NAV, std::move(nav));
    g_pane = Pane_Create(hwnd, ID_PANE, [](const std::wstring& text) { SetStatus(text); });

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                               (HMENU)ID_STATUS, g_inst, nullptr);
    ApplyFont();
    ApplyImageList();
    UpdateSortArrow();
    UpdateButtons();
    UpdateTabStrip();
    LoadViewMode();  // mode d'affichage de la dernière session
    LoadPane();
    g_dualAtStart = RegDword(L"Software\\FastExplorer", L"Dual", 0) != 0;  // double panneau au premier dossier
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
            Thumbs_Start(hwnd, WM_APP_THUMB);
            Props_Start(hwnd, WM_APP_PROPS);
            DirSizes_Start(hwnd, WM_APP_DIRSIZE);
            AddClipboardFormatListener(hwnd);
            RegisterDrop();
            AttachTarget(hwnd);
            if (HMENU sys = GetSystemMenu(hwnd, FALSE)) {
                AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(sys, MF_STRING, ID_SYS_INDEX, L"Recherche instantanée…");
            }
            SetTimer(hwnd, TIMER_INDEXASK, 1500, nullptr);
            return 0;
        case WM_CONTEXTMENU:
            if (HWND hh = ListView_GetHeader(g_list); ((HWND)wp == g_list || (HWND)wp == hh) && lp != -1) {
                RECT hr;
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (IsWindowVisible(hh) && GetWindowRect(hh, &hr) && PtInRect(&hr, pt)) {  // en-tête : choix des colonnes
                    ShowHeaderMenu(pt);
                    return 0;
                }
            }
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
                if (PtInRect(&g_split, p) || PtInRect(&g_paneSplit, p)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN:
            if (POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; PtInRect(&g_split, p)) {
                g_splitDrag = true;
                SetCapture(hwnd);
            } else if (PtInRect(&g_paneSplit, p)) {
                g_paneDrag = true;
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_splitDrag) {
                g_navW96 = MulDiv(std::max(0, (int)GET_X_LPARAM(lp)), 96, (int)g_dpi);
                Layout();
            } else if (g_paneDrag) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_paneW96 = MulDiv(std::max(0, (int)rc.right - (int)GET_X_LPARAM(lp)), 96, (int)g_dpi);
                Layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_splitDrag || g_paneDrag) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            if (g_paneDrag) SavePane();
            g_splitDrag = g_paneDrag = false;
            return 0;
        case WM_APP_ICON: OnIcon((IconResult*)lp); return 0;
        case WM_APP_THUMB: OnThumb((ThumbResult*)lp); return 0;
        case WM_APP_PROPS: OnProps((PropResult*)lp); return 0;
        case WM_APP_SAVEVIEW:
            g_saveViewPosted = false;
            SaveFolderView();
            return 0;
        case WM_APP_DIRSIZE: OnDirSize((DirSizeResult*)lp); return 0;
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
        case WM_INITMENU:
            if ((HMENU)wp == GetSystemMenu(hwnd, FALSE))
                ModifyMenuW((HMENU)wp, ID_SYS_INDEX, MF_BYCOMMAND | MF_STRING, ID_SYS_INDEX,
                            Index_ServiceInstalled() ? L"Désactiver la recherche instantanée (retirer le service)"
                                                     : L"Activer la recherche instantanée…");
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == ID_SYS_INDEX) {
                RunIndexSetup(!Index_ServiceInstalled());
                return 0;
            }
            break;
        case WM_APP_INDEXDONE: OnIndexDone((DWORD)wp, lp != 0); return 0;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ShellState") == 0) ApplyViewPrefs(ReadViewPrefs());
            break;
        case WM_CLIPBOARDUPDATE: OnClipboardUpdate(); return 0;
        case WM_COPYDATA: return ReceiveTab((const COPYDATASTRUCT*)lp) ? TRUE : FALSE;
        case WM_ENDSESSION:
            if (wp) Session_SaveLast(CurrentTabs());
            return 0;
        case WM_CLOSE:
            if (IsWindowVisible(hwnd)) Session_SaveLast(CurrentTabs());  // rouverts au prochain démarrage
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
            g_applyingCols = true;
            for (int i = 0; i < (int)g_cols.size(); ++i)
                ListView_SetColumnWidth(g_list, i, MulDiv(ListView_GetColumnWidth(g_list, i), (int)nd, (int)g_dpi));
            g_applyingCols = false;
            g_dpi = nd;
            ApplyFont();
            ApplyImageList();
            ApplyViewMode(g_viewMode, 0, false);  // icônes et hauteur des lignes à la nouvelle échelle
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
                    SetStatus(L"Chargement de " + PlaceText(g_pending.path) +
                              L"…");
                return 0;
            }
            if (wp == TIMER_WATCH) { OnWatchTimer(); return 0; }
            if (wp == TIMER_AFTEROP) { ApplyAfterOp(); return 0; }
            if (wp == TIMER_PROGRESS) { OnProgressTimer(); return 0; }
            if (wp == TIMER_INDEXASK) {
                KillTimer(hwnd, wp);
                if (IsWindowVisible(hwnd) && !g_editing) AskIndex();
                return 0;
            }
            if (wp == TIMER_PINNED) { KillTimer(hwnd, wp); NavPane_RefreshPinned(true); return 0; }
            if (wp == TIMER_PANE) {
                UpdatePane();
                return 0;
            }
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
            if ((LOWORD(wp) == ID_HDR0 || LOWORD(wp) == ID_HDR1) && HIWORD(wp) == STN_CLICKED) {
                if (LOWORD(wp) - ID_HDR0 != g_side) SwitchSide();
                SetFocus(g_list);
                return 0;
            }
            switch (LOWORD(wp)) {
                case ID_BACK: GoBack(); SetFocus(g_list); return 0;
                case ID_FWD: GoForward(); SetFocus(g_list); return 0;
                case ID_UP: GoUp(); SetFocus(g_list); return 0;
                case ID_REFRESH: g_forceSizes = true; Refresh(); SetFocus(g_list); return 0;
                case CMD_CUT: case CMD_COPY: case CMD_PASTE: case CMD_RENAME: case CMD_DELETE:
                    OnCommandBar(LOWORD(wp));
                    if (LOWORD(wp) != CMD_RENAME) SetFocus(g_list);
                    return 0;
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
            if (hdr->hwndFrom == g_status && hdr->code == NM_CLICK && FileOps_Running() > 0) {  // clic sur la progression
                CopyQueue_Show(hwnd);
                return 0;
            }
            if (g_cmdBar && hdr->hwndFrom == g_cmdBar) {
                if (hdr->code == TBN_DROPDOWN) {
                    const int id = ((NMTOOLBARW*)lp)->iItem;
                    PostMessageW(hwnd, WM_APP_CMDMENU, id, 0);  // après le relâchement du bouton
                    return TBDDRET_DEFAULT;
                }
                if (hdr->code == TBN_GETINFOTIPW) {
                    auto* tip = (NMTBGETINFOTIPW*)lp;
                    if (const wchar_t* s = CommandTip(tip->iItem); s && tip->pszText && tip->cchTextMax > 0)
                        wcsncpy_s(tip->pszText, tip->cchTextMax, s, _TRUNCATE);
                    return 0;
                }
                return 0;
            }
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
            if (hdr->hwndFrom == ListView_GetHeader(g_list)) {  // largeur ou ordre des colonnes changés
                if (hdr->code == HDN_ENDDRAG || hdr->code == HDN_DIVIDERDBLCLICKW ||
                    ((hdr->code == HDN_ITEMCHANGEDW || hdr->code == HDN_ITEMCHANGEDA) && ((NMHEADERW*)lp)->pitem &&
                     (((NMHEADERW*)lp)->pitem->mask & HDI_WIDTH)))
                    PostSaveView();
                break;
            }
            if (g_dual && hdr->hwndFrom == g_lists[g_side ^ 1]) {  // liste inactive
                switch (hdr->code) {
                    case LVN_GETDISPINFOW: FillPassive(((NMLVDISPINFOW*)lp)->item); return 0;
                    case NM_SETFOCUS: SwitchSide(); return 0;  // clic : elle devient active
                    case LVN_ODFINDITEMW: return -1;
                }
                return 0;
            }
            if (hdr->hwndFrom != g_list) break;            switch (hdr->code) {
                case LVN_GETDISPINFOW: FillDispInfo(((NMLVDISPINFOW*)lp)->item); return 0;
                case LVN_ITEMCHANGED:
                    if ((((NMLISTVIEW*)lp)->uChanged & LVIF_STATE) &&
                        ((((NMLISTVIEW*)lp)->uNewState ^ ((NMLISTVIEW*)lp)->uOldState) & LVIS_SELECTED))
                        ScheduleSelectionSummary();
                    return 0;
                case LVN_ODSTATECHANGED: ScheduleSelectionSummary(); return 0;  // plage (Maj+clic, Ctrl+A)
                case NM_CUSTOMDRAW: return OnListCustomDraw((NMLVCUSTOMDRAW*)lp);
                case LVN_GETINFOTIPW: OnInfoTip((NMLVGETINFOTIPW*)lp); return 0;
                case LVN_COLUMNDROPDOWN: {
                    const int c = ((NMLISTVIEW*)lp)->iSubItem;
                    HWND h = ListView_GetHeader(g_list);
                    RECT r{};
                    if (!Header_GetItemDropDownRect(h, c, &r)) Header_GetItemRect(h, c, &r);
                    MapWindowPoints(h, nullptr, (POINT*)&r, 2);
                    ShowColumnFilter(c, {r.left, r.bottom});
                    return 0;
                }
                case LVN_ODFINDITEMW: return FindItem((NMLVFINDITEMW*)lp);
                case LVN_COLUMNCLICK:
                    if (const int c = ((NMLISTVIEW*)lp)->iSubItem; c >= 0 && c < (int)g_cols.size()) Resort(g_cols[c]);
                    return 0;
                case NM_DBLCLK: {
                    const int e = HasView() ? EntryAt(((NMITEMACTIVATE*)lp)->iItem) : -1;
                    if (e >= 0 && GetKeyState(VK_MENU) < 0) {  // Alt+double-clic : propriétés
                        ShowProperties();
                        return 0;
                    }
                    if (e >= 0 && E(e).isDir() && GetKeyState(VK_CONTROL) < 0) {  // Ctrl+double-clic : nouvelle fenêtre
                        NewWindow(g_path.empty() ? std::wstring(NameOf(e)) : FullPath(e));
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
        case WM_APP_CMDMENU: OnCommandDropdown((int)wp); return 0;
        case WM_APP_PASSIVE: OnPassiveData((DirData*)lp); return 0;
        case WM_CTLCOLORSTATIC:  // chemins du double panneau : celui du panneau actif en surbrillance
            for (int s = 0; s < 2; ++s)
                if ((HWND)lp == g_paneHdr[s]) {
                    const bool on = s == g_side;
                    SetBkColor((HDC)wp, GetSysColor(on ? COLOR_HIGHLIGHT : COLOR_3DFACE));
                    SetTextColor((HDC)wp, GetSysColor(on ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
                    return (LRESULT)GetSysColorBrush(on ? COLOR_HIGHLIGHT : COLOR_3DFACE);
                }
            break;
        case WM_DESTROY:
            g_target.store(nullptr);
            Icons_Stop();
            Thumbs_Stop();
            Props_Stop();
            Pane_Shutdown();
            DirSizes_Stop();
            Watch_Stop();
            ShellMenu_Discard(true);
            if (g_taskbar) g_taskbar->Release();
            g_taskbar = nullptr;
            RemoveClipboardFormatListener(hwnd);
            DragDrop_RevokeTarget(g_lists[0]);
            DragDrop_RevokeTarget(g_lists[1]);
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
    // Ctrl+molette sur la liste : taille des icônes / mode d'affichage.
    if (m.message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(m.wParam) & MK_CONTROL) &&
        (m.hwnd == g_list || GetParent(m.hwnd) == g_list)) {
        g_wheelAcc += GET_WHEEL_DELTA_WPARAM(m.wParam);
        while (g_wheelAcc >= WHEEL_DELTA) { g_wheelAcc -= WHEEL_DELTA; Zoom(1); }
        while (g_wheelAcc <= -WHEEL_DELTA) { g_wheelAcc += WHEEL_DELTA; Zoom(-1); }
        return true;
    }
    if (m.hwnd == g_list && (m.message == WM_LBUTTONDOWN || m.message == WM_LBUTTONDBLCLK) && HasView()) {
        LVHITTESTINFO ht{};
        ht.pt = {GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
        const int row = ListView_SubItemHitTest(g_list, &ht);
        if (GroupAt(row) >= 0) {  // en-tête de groupe : replier / déplier
            SetFocus(g_list);
            if (m.message == WM_LBUTTONDOWN) ToggleGroup(row);
            return true;
        }
        if (g_checkboxes && row >= 0 && (ht.flags & LVHT_ONITEMSTATEICON) && EntryAt(row) >= 0) {
            SetFocus(g_list);  // case : ajoute ou retire l'élément de la sélection, sans toucher aux autres
            if (m.message == WM_LBUTTONDOWN) {
                const bool on = ListView_GetItemState(g_list, row, LVIS_SELECTED) != 0;
                ListView_SetItemState(g_list, row, (on ? 0 : LVIS_SELECTED) | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetSelectionMark(g_list, row);
                ListView_RedrawItems(g_list, row, row);
            }
            return true;
        }
    }
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
            const int ne = row >= 0 ? EntryAt(next) : -1;  // en-tête de groupe : on s'arrête
            g_tabNext = ne >= 0 ? NameOf(ne) : L"";
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
                NewTab(g_path.empty() ? std::wstring(NameOf(e)) : FullPath(e), false);
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
            g_tabOut = false;
            if (g_tabDrag >= 0) SetCapture(g_tabBar);  // suivi même hors de la barre (détacher l'onglet)
        } else if (m.message == WM_MOUSEMOVE && g_tabDrag >= 0 && !(m.wParam & MK_LBUTTON)) {
            g_tabDrag = -1;  // bouton relâché ailleurs (capture perdue)
            g_tabOut = false;
        } else if (m.message == WM_MOUSEMOVE && g_tabDrag >= 0 && (m.wParam & MK_LBUTTON)) {
            POINT s{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
            ClientToScreen(g_tabBar, &s);
            RECT br;
            GetWindowRect(g_tabBar, &br);
            InflateRect(&br, S(8), S(24));
            g_tabOut = !PtInRect(&br, s);
            if (g_tabOut) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return true;
            }
            TCHITTESTINFO ht{{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)}, 0};
            const int hit = TabCtrl_HitTest(g_tabBar, &ht);
            if (hit >= 0 && hit < (int)g_tabList.size() && hit != g_tabDrag && (size_t)g_tabDrag == g_tab) {
                MoveTab((size_t)g_tabDrag, (size_t)hit);
                g_tabDrag = hit;
            }
        } else if (m.message == WM_LBUTTONUP) {
            const int drag = g_tabDrag;
            const bool out = g_tabOut;
            g_tabDrag = -1;
            g_tabOut = false;
            if (GetCapture() == g_tabBar) ReleaseCapture();
            if (out && drag >= 0) {
                POINT s{GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
                ClientToScreen(g_tabBar, &s);
                DetachTab((size_t)drag, s);
                return true;
            }
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
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                if (GetKeyState(VK_SHIFT) < 0) SetBookmark((int)(m.wParam - '0'));
                else GoBookmark((int)(m.wParam - '0'));
                return true;
            case 'P': TogglePane(GetKeyState(VK_SHIFT) < 0 ? PaneMode::Details : PaneMode::Preview); return true;
            case VK_RETURN:
                if (focus == g_addr) return false;
                ShowProperties();
                return true;
        }
        return false;
    }
    if (m.message != WM_KEYDOWN) return false;
    if (focus == g_list && HasView() && !g_view.groups.empty()) {
        const int row = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
        if (const int g = GroupAt(row); g >= 0) {
            const bool collapsed = g_view.groups[(size_t)g].collapsed;
            if ((m.wParam == VK_LEFT && !collapsed) || (m.wParam == VK_RIGHT && collapsed) || m.wParam == VK_RETURN ||
                m.wParam == VK_SPACE) {
                ToggleGroup(row);
                return true;
            }
        }
    }
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
                        NewWindow(g_path.empty() ? std::wstring(NameOf(e)) : FullPath(e));
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
            case VK_ADD:  // Ctrl+pavé numérique + : ajuster toutes les colonnes
                if (focus == g_list) {
                    FitColumns();
                    return true;
                }
                break;
        }
        if (sh && m.wParam >= '1' && m.wParam <= '8') {  // Ctrl+Maj+1…8 : mode d'affichage
            ViewByKey((int)(m.wParam - '0'));
            return true;
        }
        if (m.wParam >= '1' && m.wParam <= '9' && !sh) {
            SwitchTab(m.wParam == '9' ? n - 1 : std::min<size_t>(m.wParam - '1', n - 1));
            return true;
        }
    }
    switch (m.wParam) {
        case VK_F5:
            if (g_dual && focus == g_list) { TransferToOther(false); return true; }  // double panneau : copier vers l'autre
            g_forceSizes = true;
            Refresh();
            return true;
        case VK_F6:
            if (g_dual && focus == g_list) { TransferToOther(true); return true; }  // déplacer vers l'autre
            return false;
        case VK_F9: SetDual(!g_dual); return true;
        case VK_F11: ToggleFullscreen(); return true;
        case 'R':
            if (!ctrl) return false;
            g_forceSizes = true;
            Refresh();
            return true;
        case 'L':
            if (!ctrl) return false;
            BeginAddressEdit();
            return true;
        case VK_F4: ShowTypedHistory(); return true;
        case VK_F3: FocusSearch(); return true;
        case 'F':
        case 'E':
            if (!ctrl) return false;
            if (m.wParam == 'E' && GetKeyState(VK_SHIFT) < 0) {  // Ctrl+Maj+E : le dossier courant dans l'arborescence
                if (HasView()) NavPane_ExpandTo(g_path);
                return true;
            }
            FocusSearch();
            return true;
        case VK_TAB:  // barre d'adresse → recherche → liste ; double panneau : d'une liste à l'autre
            if (g_dual && focus == g_list) {
                SwitchSide();
                SetFocus(g_list);
                return true;
            }
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
            if (FileOps_Progress(p) || p.waiting) {  // copie en cours ou en attente : on l'arrête (ce qui est copié reste)
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
        case VK_F2:
            if (ctrl && shift) BatchRename();
            else BeginRename();
            return true;
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

bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start, const RECT* at, bool restore) {
    g_inst = inst;
    g_prefs = ReadViewPrefs();  // avant la première lecture : elle filtre déjà les éléments cachés
    std::wstring first = start;
    if (SessionTabs s; restore && Session_RestoreEnabled() && Session_LoadLast(s) && !s.paths.empty()) {
        g_tabList.clear();
        for (const auto& p : s.paths) {
            Tab t;
            t.path = p;
            g_tabList.push_back(std::move(t));
        }
        // Double panneau : l'onglet du panneau inactif s'affiche d'abord ; SetDual(true) passe ensuite à l'actif.
        const bool dual = s.other >= 0 && RegDword(L"Software\\FastExplorer", L"Dual", 0) != 0;
        g_tab = (size_t)(dual ? s.other : s.active);
        if (dual) g_restoreOther = g_tabList[(size_t)s.active].id;
        first = g_tabList[g_tab].path;
    }
    Navigate(first, Nav::Normal);  // la lecture démarre pendant qu'on construit la fenêtre
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
