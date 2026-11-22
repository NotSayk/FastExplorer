#pragma once
#include "win.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Copie et déplacement rapides par l'API fichiers : CopyFileExW sur plusieurs
// threads (~0,6 ms par petit fichier, contre ~10 ms par élément pour le moteur
// du Shell), MoveFileExW sur un même volume (instantané).
//
// Seulement quand rien ne réclame le Shell : sinon FastTransfer renvoie false
// sans rien toucher (conflit de nom, lien ou jonction, chemin non fichier,
// dossier copié dans lui-même…). Un élément dont un fichier échoue (accès
// refusé, fichier verrouillé, disque plein) est retiré de la destination et
// rendu dans `shellLeft` : l'appelant le confie au Shell, avec ses fenêtres
// d'erreur et d'élévation.

struct FastProgress {
    std::atomic<uint64_t> bytesDone{0}, bytesTotal{0};
    std::atomic<uint32_t> filesDone{0}, filesTotal{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};  // en pause : les threads de copie attendent (entre deux blocs)
    bool move = false;
    ULONGLONG start = 0;  // GetTickCount64
};

struct FastOutcome {
    std::vector<std::wstring> created;    // noms créés directement dans la destination
    std::vector<std::wstring> shellLeft;  // sources (de premier niveau) à confier au Shell
    bool cancelled = false;
    DWORD error = 0;                      // déplacement : source impossible à supprimer après copie
};

// sources : chemins complets (fichiers ou dossiers) ; dest : dossier existant.
bool FastTransfer(const std::vector<std::wstring>& sources, const std::wstring& dest, bool move, FastProgress& prog,
                  FastOutcome& out);

// Nombre de threads de copie selon les disques (1 sur disque dur, 4 en réseau, 8 sinon).
int FastCopyThreads(const std::wstring& src, const std::wstring& dest);
