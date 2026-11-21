#pragma once
#include "win.h"

#include <cstdint>
#include <string>

// Surveillance du dossier affiché : un fichier créé, supprimé, renommé ou
// modifié (par nous ou par une autre application) provoque l'envoi de `msg`
// à `target`, avec wParam = id et lParam = 0. Un seul dossier surveillé à la fois.
//
// lParam = kWatchGone : la surveillance s'est arrêtée parce que le dossier n'est
// plus accessible (supprimé, lecteur retiré, réseau perdu).
//
// lParam = kWatchOpened : le dossier est ouvert ; l'interface doit appeler
// Watch_Opened, qui inscrit le handle auprès de Windows (éjection, voir plus bas).
//
// L'ouverture se fait sur le thread de surveillance : un partage réseau muet ne
// bloque jamais l'interface. Avant l'éjection d'un lecteur, `target` reçoit
// WM_DEVICECHANGE / DBT_DEVICEQUERYREMOVE (DBT_DEVTYP_HANDLE) et doit appeler
// Watch_Stop(true) : le lecteur est alors libéré.
constexpr LPARAM kWatchGone = 1;
constexpr LPARAM kWatchOpened = 2;

void Watch_Start(HWND target, UINT msg, const std::wstring& path, uintptr_t id);
void Watch_Opened(HWND target, uintptr_t id);

// wait : attend que le handle du dossier soit réellement fermé (éjection).
void Watch_Stop(bool wait = false);
