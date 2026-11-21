#pragma once
#include "win.h"

#include <string>

// Crée la fenêtre principale et lance la navigation vers `start`.
// at : position de la fenêtre (nouvelle fenêtre ouverte depuis une autre), sinon celle par défaut.
bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start, const RECT* at = nullptr);

// Boucle de messages. Renvoie le code de sortie.
int Ui_Run();
