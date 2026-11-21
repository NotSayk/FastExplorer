#pragma once
#include "win.h"

#include <cstdint>
#include <string>

// Surveillance du dossier affiché : un fichier créé, supprimé, renommé ou
// modifié (par nous ou par une autre application) provoque l'envoi de `msg`
// à `target`, avec wParam = id. Un seul dossier surveillé à la fois.
// L'ouverture de la surveillance se fait sur le thread de surveillance : un
// partage réseau muet ne bloque jamais l'interface.
void Watch_Start(HWND target, UINT msg, const std::wstring& path, uintptr_t id);
void Watch_Stop();
