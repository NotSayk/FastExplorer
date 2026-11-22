#pragma once
#include "win.h"

#include <cstdint>
#include <string>

// Miniatures (photos, vidéos, PDF…) pour les modes icônes, lues sur un thread
// dédié par IShellItemImageFactory : le cache de miniatures de Windows est
// partagé avec l'Explorateur (une image déjà vue là-bas arrive aussitôt).

struct ThumbResult {
    uint64_t serial;  // vue d'origine
    uint32_t idx;     // index d'entrée
    int px;           // taille demandée
    HBITMAP bmp;      // 32 bits, au plus px × px ; nullptr : pas de miniature pour ce fichier
};

// Les résultats arrivent à `target` par `msg` (lParam = ThumbResult*, à libérer avec son bitmap).
void Thumbs_Start(HWND target, UINT msg);
void Thumbs_Stop();
// Nouvelle vue ou nouvelle taille : les demandes précédentes sont abandonnées.
void Thumbs_SetSerial(uint64_t serial);
// Les plus récentes d'abord (lignes visibles après un défilement).
void Thumbs_Request(uint64_t serial, uint32_t idx, std::wstring path, int px);

// Le type de ce fichier peut-il avoir une miniature ? (Jamais pour un fichier cloud absent du disque.)
// Dossiers : oui (aperçu de leurs premiers fichiers, composé par le Shell).
bool Thumbs_Wanted(uint32_t attr, uint64_t size);
