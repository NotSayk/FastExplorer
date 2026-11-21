#pragma once
#include "win.h"

#include <string>

// Convention : "" désigne la racine virtuelle « Ce PC » (liste des lecteurs).

// base + "\" + name (sans doubler le séparateur). base vide → name.
std::wstring Join(const std::wstring& base, const wchar_t* name);

// Chemin Win32 → chemin \\?\ (lève MAX_PATH). Gère les lecteurs et l'UNC.
std::wstring ToNtPath(const std::wstring& p);

// Dossier parent et nom de l'enfant. false si p est déjà la racine.
bool ParentOf(const std::wstring& p, std::wstring& parent, std::wstring& child);

// Nettoie une saisie utilisateur : guillemets, espaces, %VARIABLES%, '/',
// "..", chemin relatif à `base`. Une saisie relative sans base est renvoyée
// telle quelle (elle échouera proprement à l'ouverture).
std::wstring NormalizePath(std::wstring s, const std::wstring& base);
