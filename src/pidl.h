#pragma once
#include "win.h"

#include <shlobj.h>

#include <memory>
#include <string>

// Identifiant Shell absolu (PIDL), libéré automatiquement.
struct PidlFree {
    void operator()(ITEMIDLIST* p) const { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlFree>;

// Chemin → PIDL ("" = Ce PC). nullptr si l'élément n'existe pas.
Pidl ParsePath(const std::wstring& path);
