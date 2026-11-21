#pragma once
#include "win.h"
#include "cancel.h"

#include <cstdint>
#include <functional>
#include <memory>
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

// Ce PC : nom et occupation d'un lecteur (lus avec un délai maximal : un
// lecteur de DVD qui démarre ou un réseau muet ne retardent pas l'affichage).
struct DriveInfo {
    std::wstring label;      // nom de volume (ou partage, pour un lecteur réseau)
    uint64_t total = 0, free = 0;  // 0 : inconnu (pas de média, délai dépassé)
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
    std::vector<DriveInfo> drives;  // Ce PC : une par entrée
    // Résultats de recherche (SearchTree) : les noms sont des chemins relatifs à `path`.
    std::wstring query;      // "" : contenu ordinaire du dossier
    bool searchDone = true;  // false : résultats partiels, la recherche continue
    bool truncated = false;  // trop de résultats : recherche arrêtée
    size_t scanned = 0;      // dossiers parcourus

    bool isRoot() const { return path.empty(); }
    const wchar_t* name(const Entry& e) const { return pool.data() + e.nameOff; }
};

// Parallélisme des clés de tri et du tri (Auto = seulement pour les gros dossiers).
enum class Parallel { Auto, Never, Always };
void SetParallel(Parallel p);

// Texte du type de lecteur (DRIVE_*).
const wchar_t* DriveTypeText(UINT type);
// Nom affiché d'un lecteur, comme l'Explorateur : « Données (D:) », « Disque local (C:) ».
std::wstring DriveDisplayName(const std::wstring& root, UINT type, const DriveInfo& info);
// Nom et occupation de lecteurs (« C:\ »…), en parallèle, au plus `timeoutMs`.
std::vector<DriveInfo> QueryDrives(const std::vector<std::wstring>& roots, DWORD timeoutMs);

// Lit le dossier (ou les lecteurs si d.path est vide) et calcule les clés de tri.
void Enumerate(DirData& d, const CancelToken& ct = {});

// Erreur signifiant « ce dossier n'existe plus ici » (supprimé, renommé,
// lecteur ou média retiré, partage disparu), par opposition à un refus d'accès.
bool IsGoneError(DWORD err);

// Comme Enumerate, mais si le dossier a disparu, remonte au plus proche parent
// existant (jusqu'à Ce PC) ; d.vanished garde alors le chemin demandé.
void EnumerateNearest(DirData& d, const CancelToken& ct = {});

// Résumé d'une sélection pour la barre d'état, comme l'Explorateur :
// « 3 éléments sélectionnés  4,52 Mo » ; taille seulement s'il n'y a que des
// fichiers (withSize = false : jamais, p. ex. lecteurs de Ce PC). "" si rien.
std::wstring SelectionText(size_t count, bool anyDir, uint64_t bytes, bool withSize = true);

struct ViewPrefs;

// Le nom correspond-il à la recherche ? Mots séparés par des espaces, tous
// présents (sans tenir compte de la casse ni des accents) ; un mot avec * ou ?
// est un motif sur le nom entier (« *.txt »).
bool NameMatches(const std::wstring& query, const wchar_t* name, uint32_t n);

// Recherche récursive dans d.path (sous-dossiers compris, sans suivre les
// jonctions ni entrer dans les dossiers masqués par `prefs`), sur plusieurs
// threads. `partial` reçoit, depuis le thread appelant, des copies triables
// des résultats au fil de l'eau (~4 par seconde) ; d contient tout à la fin.
constexpr size_t kSearchMaxResults = 100000;
void SearchTree(DirData& d, const std::wstring& query, const ViewPrefs& prefs, const CancelToken& ct,
                const std::function<void(std::shared_ptr<DirData>)>& partial = {});

// Préférences d'affichage de l'Explorateur (communes avec lui).
struct ViewPrefs {
    bool showHidden = true;        // éléments cachés
    bool showSuperHidden = true;   // fichiers protégés du système (cachés ET système)
    bool showExtensions = true;    // extensions des types connus
    bool operator==(const ViewPrefs&) const = default;
};
// Réglages actuels de Windows (SHGetSetSettings).
ViewPrefs ReadViewPrefs();
// Change les réglages de Windows, comme Afficher ▸ Afficher de l'Explorateur
// (prévient les autres fenêtres sur un thread).
void WriteViewPrefs(const ViewPrefs& p);
// L'élément est-il affiché ? Un protégé exige les deux réglages, comme l'Explorateur.
bool IsShown(DWORD attr, const ViewPrefs& p);

// Nombre de caractères d'extension à masquer à l'affichage d'un fichier (0 :
// nom complet). Masquée : type connu et extensions masquées, ou type marqué
// NeverShowExt (.lnk, .url…), sauf AlwaysShowExt. Résultat mis en cache par
// extension ; utilisable depuis n'importe quel thread.
uint32_t HiddenExtLen(const wchar_t* name, uint32_t nameLen, uint32_t extOff, bool showExtensions);

// Permutation d'affichage : dossiers d'abord, puis la colonne demandée,
// puis le nom (ordre naturel et linguistique de l'Explorateur). Ordre total
// et déterministe : les égalités sont départagées par l'index.
// Les éléments que `prefs` masque n'y figurent pas (lecteurs de Ce PC : jamais),
// ni ceux dont le nom ne correspond pas à `nameFilter` (filtre instantané,
// mêmes règles que NameMatches ; vide : aucun filtre).
// dirSizes : taille calculée de chaque dossier (par index d'entrée), pour le tri par taille.
std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc, const ViewPrefs& prefs = {},
                                const std::wstring& nameFilter = {}, const std::vector<uint64_t>* dirSizes = nullptr);
