#pragma once
#include "win.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

// Regrouper par / filtres de colonne, comme l'Explorateur : chaque élément tombe
// dans une tranche de la colonne (« A – H », « Hier », « Petit (16 Ko – 1 Mo) »,
// le type…). Les groupes sont des lignes d'en-tête insérées dans l'ordre affiché.

struct Bucket {
    int rank = 0;       // ordre des groupes (puis le libellé)
    std::wstring label; // libellé affiché, et identité de la tranche
};

Bucket NameBucket(const wchar_t* name);
// ft, now : FILETIME UTC (0 : date inconnue) ; tranches en heure locale, semaine du lundi.
Bucket DateBucket(int64_t ft, int64_t now);
Bucket SizeBucket(uint64_t size, bool dir);

// Ligne d'en-tête de groupe dans l'ordre affiché : kGroupRow | numéro du groupe.
constexpr uint32_t kGroupRow = 0x80000000u;
inline bool IsGroupRow(uint32_t v) { return (v & kGroupRow) != 0; }

struct GroupInfo {
    std::wstring label;
    size_t count = 0;        // éléments (même repliés)
    bool collapsed = false;
};

using BucketFn = std::function<Bucket(uint32_t idx)>;

struct ColumnFilter {
    BucketFn bucketOf;
    std::set<std::wstring> keep;  // tranches gardées (vide : aucun filtre)
};

// sorted : ordre trié ; filters : éléments gardés ; groupOf vide : pas de groupes.
// reverse : groupes dans l'ordre inverse de leur rang. collapsed : libellés repliés
// (en-tête affiché, éléments omis). groups reçoit les groupes, dans l'ordre affiché ; kept, les éléments
// gardés par les filtres (repliés compris), dans l'ordre du tri.
std::vector<uint32_t> ArrangeOrder(const std::vector<uint32_t>& sorted, const std::vector<ColumnFilter>& filters,
                                   const BucketFn& groupOf, bool reverse, const std::set<std::wstring>& collapsed,
                                   std::vector<GroupInfo>& groups, std::vector<uint32_t>* kept = nullptr);

// Type de dossier (« Optimiser ce dossier pour »), d'après les fichiers qu'il contient : plus de la moitié
// d'images, de musique, de vidéos ou de documents.
enum FolderKind { FK_GENERAL, FK_DOCUMENTS, FK_PICTURES, FK_MUSIC, FK_VIDEOS };
struct DirData;
FolderKind DetectFolderKind(const DirData& d);
// Valeur FolderType d'un desktop.ini (« Music », « Pictures »…) ; -1 : inconnue.
int FolderKindFromName(const std::wstring& folderType);

// Tranches présentes (pour le menu d'un filtre), dans l'ordre des groupes, avec leur nombre.
std::vector<std::pair<Bucket, size_t>> BucketsOf(const std::vector<uint32_t>& sorted, const BucketFn& bucketOf);
