#pragma once
#include "win.h"
#include "cancel.h"

#include <functional>

// Tâches de fond. Une seule tâche « courante » à la fois : en démarrer une
// nouvelle rend la précédente périmée ET interrompt ses E/S bloquantes
// (CancelSynchronousIo), pour qu'un partage réseau muet ne garde pas son
// thread bloqué indéfiniment.

// Nouvelle génération : périme la tâche courante et annule ses E/S.
CancelToken Jobs_NewGeneration();

// Génération courante (un résultat dont la génération diffère est périmé).
uint64_t Jobs_Current();

// Lance fn sur un nouveau thread (COM initialisé) qui devient la tâche courante.
void Jobs_Spawn(std::function<void()> fn);
