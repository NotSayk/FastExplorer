#pragma once
#include "win.h"

#include <string>

// FastExplorer comme explorateur par défaut, pour l'utilisateur seulement (HKCU), réversible :
//  - dossiers et lecteurs : verbe « FastExplorer » par défaut (Directory\shell, Drive\shell) ;
//  - Win+E : CLSID {52205fd8-…}\shell\opennewwindow\command.
// L'ancien verbe par défaut est gardé (HKCU\Software\FastExplorer\DefaultPrev) et remis à l'arrêt.

bool DefExp_IsOn();
// exe : chemin de FastExplorer.exe. 0 : fait ; sinon code d'erreur du registre.
LSTATUS DefExp_Set(bool on, const std::wstring& exe);
// Commande enregistrée pour ouvrir un dossier (« "C:\…\FastExplorer.exe" "%1" »).
std::wstring DefExp_Command(const std::wstring& exe);
