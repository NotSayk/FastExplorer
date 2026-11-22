#pragma once
#include "win.h"
#include "cancel.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Clés de tri. COL_PROP : propriété du Shell, valeurs fournies à SortOrder (PropValues).
enum Col { COL_NAME, COL_SIZE, COL_TYPE, COL_DATE, COL_CTIME, COL_ATIME, COL_ATTR, COL_COUNT, COL_PROP = 100 };

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
    int64_t mtime;     // FILETIME UTC : modification
    int64_t ctime;     // création (0 : inconnue)
    int64_t atime;     // dernier accès (0 : inconnu)
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
    bool fromIndex = false;  // résultats de l'index (service de recherche instantanée)

    bool isRoot() const { return path.empty() && query.empty(); }  // Ce PC (pas une recherche dans tout le PC)
    const wchar_t* name(const Entry& e) const { return pool.data() + e.nameOff; }
};

// Colonne Attributs : une lettre par attribut, comme l'Explorateur (« RHSA », « DA »…).
constexpr uint32_t kShownAttrs = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                 FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED |
                                 FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
                                 FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_PINNED | FILE_ATTRIBUTE_UNPINNED |
                                 FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
std::wstring AttrText(uint32_t attr);

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

// Même règle, préparée une fois pour comparer beaucoup de noms (thread-safe en lecture).
struct NameMatcher {
    std::vector<std::wstring> terms;
    std::vector<bool> wild;
    std::vector<std::string> ascii;  // mot en ASCII minuscule ("" : pas d'ASCII pur)
    explicit NameMatcher(const std::wstring& q);
    bool operator()(const wchar_t* name, uint32_t n) const;
    static int AsciiFind(const wchar_t* name, uint32_t n, const std::string& term);
};

// Filtres d'une recherche, mêlés aux mots du nom (anglais accepté aussi) :
//   ext:pdf  ext:jpg;png        extension(s)
//   type:dossier  type:fichier  (folder / file)
//   taille:>10Mo  taille:<1Go  taille:1Mo..50Mo   (size:, unités o Ko Mo Go To / B KB MB GB TB, base 1024)
//   date:aujourd'hui  date:hier  date:semaine  date:mois  date:année   (7, 30, 365 derniers jours)
//   date:>2026-01-01  date:<2026-01-01  date:2026-01-01  date:2026-01-01..2026-03-31   (modifié:, modified:)
// Un filtre illisible reste un mot du nom (aucun résultat : l'erreur se voit).
struct SearchSpec {
    std::wstring names;              // le reste : mots du nom (règles de NameMatches)
    std::vector<std::wstring> exts;  // en minuscules, sans le point
    int kind = 0;                    // 0 : tout, 1 : dossiers, 2 : fichiers
    uint64_t minSize = 0, maxSize = UINT64_MAX;
    int64_t minTime = INT64_MIN, maxTime = INT64_MAX;  // FILETIME UTC, [min, max)
    bool sized() const { return minSize || maxSize != UINT64_MAX; }
    bool dated() const { return minTime != INT64_MIN || maxTime != INT64_MAX; }
    bool filtered() const { return !exts.empty() || kind || sized() || dated(); }
    // Filtres seulement (pas les mots du nom). name : nom ou chemin relatif (l'extension est prise sur la fin).
    bool Accept(const wchar_t* name, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime) const;
};
// now : FILETIME UTC de référence (0 : maintenant).
SearchSpec ParseSearch(const std::wstring& q, int64_t now = 0);

// Construction de résultats hors Enumerate / SearchTree (index de recherche).
void DirData_Add(DirData& d, const wchar_t* name, uint32_t n, uint32_t attr, uint64_t size, int64_t mtime,
                 int64_t ctime = 0, int64_t atime = 0);
void DirData_BuildKeys(DirData& d);  // à appeler une fois toutes les entrées ajoutées

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
// prop : valeurs de la colonne COL_PROP (par index d'entrée).
struct PropValues;
std::vector<uint32_t> SortOrder(const DirData& d, int col, bool asc, const ViewPrefs& prefs = {},
                                const std::wstring& nameFilter = {}, const std::vector<uint64_t>* dirSizes = nullptr,
                                const PropValues* prop = nullptr);

// Valeurs d'une colonne de propriété du Shell (auteur, dimensions, durée…), par index d'entrée.
struct PropValues {
    std::vector<std::wstring> text;  // texte affiché ("" : aucune valeur)
    std::vector<double> num;         // clé de tri numérique ; NaN : trier sur le texte
    std::vector<uint8_t> state;      // 0 : pas demandée, 1 : en cours de lecture, 2 : lue
    void Reset(size_t n) {
        text.assign(n, {});
        num.assign(n, std::numeric_limits<double>::quiet_NaN());
        state.assign(n, 0);
    }
};
