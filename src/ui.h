#pragma once
#include "win.h"

#include <string>

// Crée la fenêtre principale et lance la navigation vers `start`.
bool Ui_Create(HINSTANCE inst, int show, const std::wstring& start);

// Boucle de messages. Renvoie le code de sortie.
int Ui_Run();
