#pragma once
#include "win.h"

#include <string>
#include <vector>

// Épingles enregistrées par FastExplorer lui-même, indépendantes de Windows :
//   pins.txt       dossiers épinglés « dans FastExplorer » (menu contextuel) ;
//   shellpins.txt  copie de la dernière lecture réussie de l'Accès rapide de
//                  Windows, affichée quand celui-ci ne répond plus.
// Fichiers texte UTF-8 dans %APPDATA%\FastExplorer, une entrée par ligne
// (« chemin » ou « chemin<TAB>libellé »). Utilisable depuis n'importe quel thread.

struct SavedPin {
    std::wstring path;
    std::wstring text;  // vide : nom du dossier
};

// Dossier des fichiers (créé au besoin) ; "" si introuvable.
std::wstring Pins_Dir();
// Tests : autre dossier ("" = revenir à %APPDATA%\FastExplorer).
void Pins_SetDir(const std::wstring& dir);

std::vector<SavedPin> Pins_LoadOwn();
bool Pins_IsOwn(const std::wstring& path);
// Ajoute à la fin / retire ; false si l'écriture du fichier échoue.
bool Pins_AddOwn(const std::wstring& path);
bool Pins_RemoveOwn(const std::wstring& path);

std::vector<SavedPin> Pins_LoadSnapshot();
// N'écrit que si le contenu change.
bool Pins_SaveSnapshot(const std::vector<SavedPin>& pins);
