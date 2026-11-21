#pragma once
#include "win.h"
#include "cancel.h"

#include <cstdint>
#include <string>
#include <vector>

enum Col { COL_NAME, COL_SIZE, COL_TYPE, COL_DATE, COL_COUNT };

struct Entry {
    uint32_t nameOff;  // offset dans DirData::pool (nom terminé par \0)
    uint32_t nameLen;  // en caractères
    uint32_t extOff;   // position du '.' de l'extension (== nameLen si aucune)
    uint32_t attr;     // FILE_ATTRIBUTE_*
    int32_t icon;      // icône déjà résolue par le thread de travail, sinon -1
    uint32_t extra;    // racine : type de lecteur (DRIVE_*)
    uint32_t keyOff;   // clé de tri binaire dans DirData::keys
    uint32_t keyLen;
    uint64_t size;
    int64_t mtime;     // FILETIME UTC
    bool isDir() const { return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

// Contenu d'un dossier. Construit par le thread de travail puis immuable :
// l'interface et les tâches de tri le partagent sans verrou.
struct DirData {
    std::wstring path;  // "" = Ce PC
    std::vector<Entry> entries;
    std::vector<wchar_t> pool;
    std::vector<BYTE> keys;
    size_t nDirs = 0, nFiles = 0;
    DWORD error = 0;
    double enumMs = 0, keyMs = 0;
    std::wstring vanished;  // EnumerateNearest : dossier demandé, disparu (sinon vide)

    bool isRoot() const { return path.empty(); }
    const wchar_t* name(const Entry& e) const { return pool.data() + e.nameOff; }
};

// Parallélisme des clés de tri et du tri (Auto = seulement pour les gros dossiers).
enum class Parallel { Auto, Never, Always };
void SetParallel(Parallel p);

// Lit le dossier (ou les lecteurs si d.path est vide) et calcule les clés de tri.
void Enumerate(DirData& d, const CancelToken& ct = {});

// Erreur signifiant « ce dossier n'existe plus ici » (supprimé, renommé,
// lecteur ou média retiré, partage disparu), par opposition à un refus d'accès.
bool IsGoneError(DWORD err);

// Comme Enumerate, mais si le dossier a disparu, remonte au plus proche parent
// existant (jusqu'à Ce PC) ; d.vanished garde alors le chemin demandé.
void EnumerateNearest(DirData& d, const CancelToken& ct = {});

// Permutation d'affichage : dossiers d'abord, puis la colonne demandée,
// puis le nom (ordre naturel et linguistique de l'Explorateur). Ordre total
// et déterministe : les égalités sont départagées par l'index.
std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc);
