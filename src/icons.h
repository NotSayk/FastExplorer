#pragma once
#include "win.h"

#include <cstdint>
#include <string>

// Icônes « propres » à un fichier (exe, raccourcis, dossiers personnalisés…),
// résolues sur un thread dédié. Les autres fichiers prennent l'icône de leur
// extension, instantanément (cache dans ui.cpp).

// Faut-il demander l'icône propre de cet élément plutôt que celle de son type ?
// Jamais pour un fichier cloud absent du disque : lire son icône le téléchargerait.
bool NeedsOwnIcon(uint32_t attr, const wchar_t* ext, uint32_t extLen);

struct IconResult {
    uint64_t serial;  // vue d'origine
    uint32_t idx;     // index d'entrée
    int icon;         // index dans la liste d'images système
    int overlay;      // surimpression (flèche de raccourci, état cloud…), 0 = aucune
};

// Démarre le thread ; les résultats arrivent à `target` par `msg` (lParam = IconResult*, à libérer).
void Icons_Start(HWND target, UINT msg);
void Icons_Stop();

// Nouvelle vue affichée : les demandes des vues précédentes sont abandonnées.
void Icons_SetSerial(uint64_t serial);

// Demande l'icône de `path`. Traitement du plus récent au plus ancien : les
// lignes visibles en dernier (après un défilement) passent en premier.
void Icons_Request(uint64_t serial, uint32_t idx, std::wstring path);
