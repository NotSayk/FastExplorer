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
// dossier copié dans lui-même…). Reprise après erreur : une erreur passagère
// (réseau coupé un instant, fichier ouvert un court moment) est réessayée ; les
// gros fichiers vers ou depuis le réseau reprennent là où la copie s'est
// arrêtée. Un fichier qui échoue encore (accès refusé, fichier verrouillé) est
// rendu dans `shellLeft`, seul : ce qui est copié reste, et l'appelant confie le
// reste au Shell, avec ses fenêtres d'erreur et d'élévation.

struct FastProgress {
    std::atomic<uint64_t> bytesDone{0}, bytesTotal{0};
    std::atomic<uint32_t> filesDone{0}, filesTotal{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};  // en pause : les threads de copie attendent (entre deux blocs)
    bool move = false;
    const wchar_t* label = nullptr;  // barre d'état : « Extraction », « Compression » (nullptr : copie / déplacement)
    ULONGLONG start = 0;  // GetTickCount64
    bool verify = false;          // relire chaque copie et la comparer à l'original (compté dans bytesTotal)
    bool allowDecrypted = false;  // fichiers chiffrés (EFS) copiés en clair là où le chiffrement est impossible
};

struct FastOutcome {
    std::vector<std::wstring> created;    // noms créés directement dans la destination
    // Ce qui reste à confier au Shell : (source, dossier de destination). Un élément entier quand notre moteur
    // n'a rien pu en faire, sinon les seuls fichiers qui ont échoué, chacun vers son dossier.
    std::vector<std::pair<std::wstring, std::wstring>> shellLeft;
    bool cancelled = false;
    DWORD error = 0;                      // déplacement : source impossible à supprimer après copie
    uint32_t retried = 0;                 // fichiers réussis après une ou plusieurs reprises
    uint32_t verifyFailed = 0;            // copies différentes de l'original (refaites par le Shell)
};

// sources : chemins complets (fichiers ou dossiers) ; dest : dossier existant.
bool FastTransfer(const std::vector<std::wstring>& sources, const std::wstring& dest, bool move, FastProgress& prog,
                  FastOutcome& out);

// Relit la copie (sans le cache, pour lire le disque) et la compare à l'original. prog : octets comptés.
bool FastVerify(const std::wstring& src, const std::wstring& dst, FastProgress* prog = nullptr);

// Erreur passagère qui mérite une nouvelle tentative (réseau, partage).
bool FastTransient(DWORD error);

// Nombre de threads de copie selon les disques (1 sur disque dur, 4 en réseau, 8 sinon).
int FastCopyThreads(const std::wstring& src, const std::wstring& dest);
