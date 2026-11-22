#pragma once
#include "win.h"

#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

struct DirData;
struct ViewPrefs;

// Volet de navigation (arborescence à gauche) :
//   ★ Accès rapide : les dossiers épinglés de l'Explorateur, lus dans son propre
//     « Accès rapide » (épingler / désépingler par le menu du Shell marche donc
//     tel quel, et reste commun avec l'Explorateur) ;
//   🖥 Ce PC : les lecteurs, puis leurs dossiers, chargés à l'ouverture d'un nœud.
//
// Aucun accès disque sur le thread de l'interface : chaque lecture part sur un
// thread, son résultat revient par `msg` (à relayer à NavPane_OnResult).

struct NavHost {
    std::function<void(const std::wstring& path)> navigate;  // "" = Ce PC
    // Menu contextuel d'un dossier du volet (path : "" = Ce PC).
    std::function<void(const std::wstring& path, POINT screen)> contextMenu;
    std::function<void(const std::wstring& text)> status;  // message pour la barre d'état
};

HWND NavPane_Create(HWND parent, UINT id, UINT msg, NavHost host);
void NavPane_SetImageList(HIMAGELIST il);
// WM_NOTIFY venant du volet ; true si traité (*result = valeur de retour).
bool NavPane_Notify(const NMHDR* hdr, LRESULT* result);
void NavPane_OnResult(LPARAM lp);
// WM_CONTEXTMENU du volet (lp : point écran, ou -1 au clavier).
void NavPane_ContextMenu(LPARAM lp);

// Dossier affiché : sélectionné dans le volet s'il y figure.
void NavPane_ShowPath(const std::wstring& path);
// Contenu d'un dossier tout juste relu : ses sous-dossiers déjà affichés dans le
// volet sont mis à jour sans nouvelle lecture du disque.
void NavPane_FolderListed(const DirData& d);
void NavPane_RefreshDrives();
// Éléments cachés affichés ou non (comme la liste) ; relit les dossiers ouverts.
void NavPane_SetViewPrefs(const ViewPrefs& p);
// Nom affiché d'un lecteur de Ce PC (« Disque local (C:) ») s'il est déjà lu, sinon "".
std::wstring NavPane_DriveText(const std::wstring& root);
// Dossier sous ce point de l'écran (clic milieu : nouvel onglet). false : aucun.
bool NavPane_PathAt(POINT screen, std::wstring& path);
// Relit l'Accès rapide (au plus une fois toutes les 2 s, sauf `force`).
void NavPane_RefreshPinned(bool force = false);

// Épingle propre à FastExplorer (pins.*), indépendante de Windows.
enum class NavPin {
    None,     // pas dans l'Accès rapide (ou seulement parmi les dossiers par défaut)
    Windows,  // épinglé dans l'Accès rapide de Windows
    Own,      // épinglé dans FastExplorer
};
NavPin NavPane_PinOf(const std::wstring& path);
// Épingle / retire dans FastExplorer puis rafraîchit le volet ; false si échec.
bool NavPane_Pin(const std::wstring& path, bool pin);
// Désépingle de l'Accès rapide de Windows (verbe du Shell, sur un thread) puis
// rafraîchit le volet. Le menu d'un dossier ouvert depuis son parent ne propose
// que « Épingler » : seul l'élément de l'Accès rapide sait se désépingler.
void NavPane_UnpinWindows(const std::wstring& path);

// ── Lectures (n'importe quel thread ; utilisées par le volet et les tests) ──
struct NavFolder {
    std::wstring text;  // nom affiché
    std::wstring path;
    int icon = -1;      // icône système, -1 = dossier ordinaire
    bool hasChildren = true;
    NavPin pin = NavPin::None;
};
// Contenu de l'Accès rapide (COM initialisé), dans cet ordre :
//   1. les épingles de Windows ; si son Accès rapide ne répond pas (Explorateur
//      désactivé, Windows allégé, stratégie…), leur dernière copie connue ;
//   2. les épingles de FastExplorer qui n'y sont pas déjà ;
//   3. si rien de tout ça : Bureau, Téléchargements, Documents, Images, Musique, Vidéos.
// useShell = false : comme si l'Accès rapide de Windows ne répondait pas (tests).
std::vector<NavFolder> NavPane_ReadPinned(bool useShell = true);
// Contenu actuel de l'Accès rapide du volet (thread de l'interface ; pas encore lu : dernière copie connue).
std::vector<NavFolder> NavPane_ShownPins();
// Sous-dossiers d'un dossier, dans l'ordre de l'Explorateur (sans les dossiers
// protégés du système, cachés ET système). "" = lecteurs de Ce PC.
std::vector<NavFolder> NavPane_ReadChildren(const std::wstring& path);
