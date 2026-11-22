#pragma once
#include "win.h"

#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

// Glisser-déposer OLE : la liste est à la fois source et cible, interopérable
// avec l'Explorateur et les autres applications.
//
// Cible : on ne réinvente rien. Le dépôt est confié à la cible du Shell — celle
// du dossier affiché, ou celle de l'élément survolé (dossier, .zip, raccourci,
// exécutable…). On obtient exactement le comportement de l'Explorateur : même
// volume = déplacer, autre volume = copier, Ctrl / Maj / Alt, menu « Copier ici /
// Déplacer ici / Créer un raccourci » au glisser du bouton droit, fichiers
// virtuels, déplacement optimisé, conflits.
//
// Exception, pour la vitesse : un glisser du bouton gauche de fichiers ordinaires
// sur un dossier ordinaire (copier ou déplacer, tel que choisi par la cible du
// Shell) est confié à `transfer`, donc à notre moteur de copie. La source est
// prévenue d'un déplacement optimisé : elle ne supprime rien elle-même.

struct DropHost {
    std::function<std::wstring()> folder;                    // dossier affiché ("" = Ce PC)
    std::function<int(POINT client, std::wstring& name)> hit;  // ligne survolée (-1 = vide) et son nom
    std::function<bool(const std::wstring& name)> dragged;   // élément en cours de glissement (depuis nous)
    std::function<void(int row)> highlight;                  // ligne cible en surbrillance (-1 = aucune)
    std::function<void(bool intoFolder)> dropped;            // dépôt fait ; intoFolder : dans le dossier affiché
    // Copier (move = false) ou déplacer `paths` dans `dest` ; vide = toujours le Shell.
    std::function<void(const std::wstring& dest, std::vector<std::wstring> paths, bool move)> transfer;
    // hit() renvoie des chemins complets (fil d'Ariane) : pas de dossier affiché, ni de défilement.
    bool fullPaths = false;
};

bool DragDrop_RegisterTarget(HWND list, DropHost host);
void DragDrop_RevokeTarget(HWND list);

// Commence un glissement des éléments (LVN_BEGINDRAG / LVN_BEGINRDRAG).
// Bloquant jusqu'au dépôt ; renvoie l'effet obtenu.
DWORD DragDrop_Start(HWND list, const std::wstring& folder, const std::vector<std::wstring>& names, POINT client);

// Cible Shell d'un élément du dossier, ou du dossier lui-même si name est vide.
bool DragDrop_ShellTarget(HWND owner, const std::wstring& folder, const std::wstring& name, IDropTarget** out);
