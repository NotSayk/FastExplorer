#pragma once
#include "win.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Volet de droite, comme l'Explorateur : aperçu du fichier (Alt+P) ou détails
// (Alt+Maj+P) avec propriétés modifiables. Tout le travail lent (gestionnaires
// d'aperçu de Windows, hébergés hors processus par prevhost.exe ; lecture et
// écriture des propriétés ; miniatures) se fait sur un thread dédié.

enum class PaneMode { None = 0, Preview = 1, Details = 2 };

struct PaneItem {
    std::wstring path;  // chemin complet
    uint32_t attr = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
};

// status : message court pour la barre d'état (propriété enregistrée, erreur).
HWND Pane_Create(HWND parent, int id, std::function<void(const std::wstring&)> status);
void Pane_SetMode(PaneMode m);
PaneMode Pane_Mode();
// À montrer : la sélection ; vide : le dossier affiché (folderTitle, folderText : « 12 éléments »).
// Ne relit rien si c'est déjà ce qui est affiché (mêmes chemins, mêmes dates).
void Pane_Show(const std::wstring& folderTitle, const std::wstring& folderText, std::vector<PaneItem> items);
void Pane_Shutdown();

// Le fichier vient-il d'Internet (flux Zone.Identifier, zone 3 ou 4) ? Exposé pour les tests.
bool Pane_FromInternet(const std::wstring& path);
// Texte d'un fichier pour l'aperçu (UTF-8, UTF-16 ou page de code locale ; fins de ligne CRLF).
std::wstring Pane_ReadText(const std::wstring& path, size_t maxBytes, bool& truncated);
