#pragma once
#include "win.h"

#include <functional>
#include <string>
#include <vector>

// Fil d'Ariane de la barre d'adresse : « Ce PC › Disque local (C:) › Dev › … ».
//   - clic sur un segment : ouvre ce dossier ;
//   - clic sur un chevron › : menu de ses sous-dossiers (lus sur un thread) ;
//   - chemin trop long : les premiers segments passent dans un menu « « » ;
//   - clic dans le vide : saisie du chemin (host.edit).

struct Crumb {
    std::wstring text;  // affiché
    std::wstring path;  // dossier ouvert au clic ("" = Ce PC)
};

struct CrumbHost {
    std::function<void(const std::wstring& path)> navigate;
    std::function<void()> edit;
    // Clic du milieu sur un segment : ouvrir ce dossier dans un nouvel onglet.
    std::function<void(const std::wstring& path)> openNewTab;
    // Nom d'un lecteur (« Disque local (C:) ») s'il est connu, sinon "".
    std::function<std::wstring(const std::wstring& root)> driveText;
};

HWND Crumbs_Create(HWND parent, UINT id, CrumbHost host);
void Crumbs_SetPath(const std::wstring& path);
// Noms des lecteurs devenus connus : le fil est recomposé (même chemin).
void Crumbs_Update();

// Découpage d'un chemin en segments (sans UI ; testé).
std::vector<Crumb> Crumbs_Split(const std::wstring& path,
                                const std::function<std::wstring(const std::wstring&)>& driveText = {});
