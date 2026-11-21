#pragma once
#include "pidl.h"
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

// Construit le menu à l'avance (≈100 ms de gestionnaires Shell), au moment où
// le bouton droit s'enfonce : ShellMenu_Show le réutilise s'il vise les mêmes
// éléments, et l'affiche donc dès le relâchement.
bool ShellMenu_Prepare(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, bool extended);

// Libère un menu préparé et non affiché (avant OleUninitialize).
void ShellMenu_Discard();

// Une fois par processus, sur un thread de fond : charge les gestionnaires du
// Shell (DLL d'extensions, associations, sous-menu Nouveau ▸) en construisant
// les menus du fond du dossier et des éléments donnés. Le premier clic droit
// passe ainsi de ~1,5 s à ~0,1 s.
void ShellMenu_WarmUp(const std::wstring& folder, const std::vector<std::wstring>& names);

// Exécute directement un verbe ("properties", "delete"…) sur des éléments.
bool ShellMenu_Invoke(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names,
                      const char* verb);

// À appeler en tête de WndProc : relaie aux extensions les messages des
// sous-menus dessinés par elles (Ouvrir avec ▸, Envoyer vers ▸, Nouveau ▸).
bool ShellMenu_HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Objet Shell (IContextMenu, IDataObject…) pour des éléments d'un même dossier.
bool ShellObjectOf(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, REFIID riid,
                   void** out);

// Cible d'un raccourci .lnk, sans le « résoudre » (ni recherche, ni réseau).
// false si la cible n'est pas un chemin du système de fichiers.
bool ResolveShortcut(const std::wstring& lnk, std::wstring& target);
