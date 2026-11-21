#pragma once
#include "win.h"

#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

struct DirData;

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
// Relit l'Accès rapide (au plus une fois toutes les 2 s, sauf `force`).
void NavPane_RefreshPinned(bool force = false);

// ── Lectures (n'importe quel thread ; utilisées par le volet et les tests) ──
struct NavFolder {
    std::wstring text;  // nom affiché
    std::wstring path;
    int icon = -1;      // icône système, -1 = dossier ordinaire
    bool hasChildren = true;
};
// Dossiers épinglés de l'Accès rapide (COM initialisé).
std::vector<NavFolder> NavPane_ReadPinned();
// Sous-dossiers d'un dossier, dans l'ordre de l'Explorateur (sans les dossiers
// protégés du système, cachés ET système). "" = lecteurs de Ce PC.
std::vector<NavFolder> NavPane_ReadChildren(const std::wstring& path);
