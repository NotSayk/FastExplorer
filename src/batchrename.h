#pragma once
#include "win.h"

#include <cstdint>
#include <string>
#include <vector>

// Renommage par lot avancé (ce que l'Explorateur ne sait pas faire) :
// rechercher / remplacer (texte ou expression régulière), modèle de nom avec
// compteur et date, changement de casse, aperçu avant / après.
//
// Pour chaque élément : base = nom sans l'extension (avec, si `withExtension`) ;
// 1. rechercher / remplacer dans la base ;
// 2. modèle : « {nom} » = la base obtenue, « {ext} » = l'extension sans le point,
//    « {n} » = compteur (« {n:3} » : sur 3 chiffres), « {date} » = date de
//    modification (AAAA-MM-JJ), « {dossier} » = nom du dossier parent ;
// 3. casse ;
// 4. extension d'origine remise (sauf si `withExtension`).

enum class NameCase { Keep, Lower, Upper, Title, Sentence };

struct RenameRule {
    std::wstring find, replace;
    bool regex = false;
    bool matchCase = false;
    std::wstring pattern = L"{nom}";
    NameCase nameCase = NameCase::Keep;
    bool withExtension = false;  // rechercher / remplacer et casse portent aussi sur l'extension
    int start = 1, step = 1;
};

struct RenameItem {
    std::wstring name;
    bool dir = false;
    int64_t modified = 0;  // FILETIME UTC (0 : inconnue)
};

// Nouveaux noms, dans l'ordre des éléments. error : expression régulière invalide (noms inchangés).
std::vector<std::wstring> BatchNames(const RenameRule& rule, const std::vector<RenameItem>& items,
                                     const std::wstring& folderName, std::wstring* error = nullptr);

// Pourquoi ce nom est refusé (caractère interdit, nom réservé, vide…) ; "" s'il est valable.
std::wstring NameProblem(const std::wstring& name);

// État de chaque nouveau nom : "" (valable), sinon la raison (doublon dans le lot compris, sans la casse).
// `others` : noms du dossier qui ne sont pas renommés (pris).
std::vector<std::wstring> BatchProblems(const std::vector<std::wstring>& names, const std::vector<std::wstring>& others);
