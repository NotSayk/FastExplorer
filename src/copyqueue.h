#pragma once
#include "win.h"

// Fenêtre « File d'attente des copies » : les copies et déplacements en cours
// ou en attente, avec pause / reprise, « démarrer maintenant » et annulation.
// Se met à jour seule tant qu'elle est ouverte (lecture de FileOps_Jobs).

// Ouvre la fenêtre (ou la ramène devant), sur l'écran de `owner`. activate = false : sans prendre le focus.
void CopyQueue_Show(HWND owner, bool activate = true);
bool CopyQueue_Visible();
