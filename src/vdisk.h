#pragma once
#include "win.h"

#include <string>

// Images disque (.iso, .img, .vhd, .vhdx) : montées par virtdisk (AttachVirtualDisk), comme le
// « Monter » de l'Explorateur mais sans ouvrir sa fenêtre. Un ISO se monte sans être
// administrateur ; un VHD le demande souvent (ERROR_ACCESS_DENIED : passer par le Shell).
// On démonte par « Éjecter », dans le menu du lecteur.

bool VDisk_IsImage(const std::wstring& name);

// Monte l'image (déjà montée : retrouve son lecteur) et attend sa lettre (au plus timeoutMs).
// root : « E:\ » si une lettre est apparue. Renvoie 0 ou un code d'erreur Win32. Bloquant.
DWORD VDisk_Mount(const std::wstring& image, std::wstring& root, DWORD timeoutMs = 8000);
