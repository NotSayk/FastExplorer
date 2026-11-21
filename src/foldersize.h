#pragma once
#include "win.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Taille des dossiers (colonne Taille), calculée en arrière-plan : plusieurs
// threads, E/S en priorité basse (la navigation et les copies passent avant).

struct FolderSize {
    uint64_t bytes = 0;            // taille des fichiers, sous-dossiers compris
    uint64_t files = 0, dirs = 0;  // nombre de fichiers et de sous-dossiers
    bool complete = true;          // false : un sous-dossier n'a pas pu être lu (accès refusé…)
};

// Parcourt `dir` et ses sous-dossiers, sans suivre les jonctions ni les liens
// symboliques (ni boucle, ni double compte). `stop` est consulté régulièrement :
// true abandonne le calcul (retour false).
bool FolderSize_Compute(const std::wstring& dir, FolderSize& out, const std::function<bool()>& stop = {});

struct DirSizeResult {
    uint64_t ticket;    // demande d'origine
    std::wstring path;  // dossier mesuré
    FolderSize size;
};

// Les résultats arrivent à `target` par `msg` (lParam = DirSizeResult*, à libérer).
void DirSizes_Start(HWND target, UINT msg);
void DirSizes_Stop();

// Remplace la demande en cours : ces dossiers, dans cet ordre. Un calcul déjà
// lancé pour l'un d'eux continue (sans repartir de zéro) ; les autres s'arrêtent.
void DirSizes_Request(uint64_t ticket, std::vector<std::wstring> dirs);
