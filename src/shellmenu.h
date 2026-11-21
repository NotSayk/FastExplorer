#pragma once
#include "win.h"

#include <functional>
#include <string>
#include <vector>

// Menus contextuels et verbes du Shell (IContextMenu) : exactement le menu de
// l'Explorateur, extensions tierces comprises (7-Zip, Git, antivirus…).
// Thread de l'interface uniquement (COM STA, OLE initialisé).
//
// folder : dossier courant ("" = Ce PC). names : éléments de ce dossier ;
// une liste vide désigne le fond du dossier (Nouveau ▸, Coller, Propriétés…).

// Appelé avec le verbe choisi ("open", "delete", "properties"…, parfois vide).
// Renvoie true si l'application le gère elle-même : le Shell ne l'exécute pas.
using VerbFilter = std::function<bool(const std::wstring& verb)>;

// Affiche le menu à `pt` (coordonnées écran) et exécute la commande choisie.
// extended : Maj enfoncée (verbes étendus : « Copier en tant que chemin d'accès »…).
bool ShellMenu_Show(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, POINT pt,
                    bool extended, const VerbFilter& filter);

// Exécute directement un verbe ("properties", "delete"…) sur des éléments.
bool ShellMenu_Invoke(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names,
                      const char* verb);

// À appeler en tête de WndProc : relaie aux extensions les messages des
// sous-menus dessinés par elles (Ouvrir avec ▸, Envoyer vers ▸, Nouveau ▸).
bool ShellMenu_HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Cible d'un raccourci .lnk, sans le « résoudre » (ni recherche, ni réseau).
// false si la cible n'est pas un chemin du système de fichiers.
bool ResolveShortcut(const std::wstring& lnk, std::wstring& target);
