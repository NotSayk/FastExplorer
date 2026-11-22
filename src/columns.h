#pragma once
#include "win.h"

#include <propsys.h>

#include <cstdint>
#include <string>
#include <vector>

// Colonnes de la vue Détails : celles que la lecture du dossier fournit déjà
// (taille, dates, attributs…), et toute propriété du Shell (auteur, dimensions,
// durée…), lue en arrière-plan par le Property System.

struct ColSpec {
    int key = 0;           // COL_* (listing.h) ; COL_PROP : propriété du Shell
    PROPERTYKEY pk{};      // si COL_PROP
    std::wstring title;
    int width96 = 120;     // largeur à 96 ppp
    bool right = false;    // aligné à droite (tailles)
};

// Même colonne (même donnée affichée) ?
bool SameCol(const ColSpec& a, const ColSpec& b);
ColSpec Columns_Builtin(int key);
// Nom, Modifié le, Type, Taille (comme l'Explorateur).
std::vector<ColSpec> Columns_Default();
// Proposées dans le menu de l'en-tête (hors Nom).
std::vector<ColSpec> Columns_Common();
ColSpec Columns_Prop(const PROPERTYKEY& pk);

// Identifiant texte stable : « size », « ctime »… ou « {GUID} 4 » pour une propriété.
std::wstring Columns_Id(const ColSpec& c);
bool Columns_FromId(const std::wstring& id, ColSpec& c);

// « Plus… » : toutes les propriétés affichables, à cocher. Nom reste en tête.
// Renvoie true si l'utilisateur a validé (cols est alors la nouvelle liste).
bool Columns_ChooseDialog(HWND owner, std::vector<ColSpec>& cols);

// ───────── Lecture des propriétés (threads dédiés) ─────────

struct PropResult {
    uint64_t serial;
    uint32_t idx;
    std::vector<PROPERTYKEY> keys;
    std::vector<std::wstring> text;  // par clé ("" : aucune valeur)
    std::vector<double> num;         // par clé ; NaN : pas un nombre ni une date
};

// Les résultats arrivent à `target` par `msg` (lParam = PropResult*, à libérer).
void Props_Start(HWND target, UINT msg);
void Props_Stop();
// Nouvelle vue : les demandes précédentes sont abandonnées.
void Props_SetSerial(uint64_t serial);
// urgent : ligne visible (servie en premier) ; sinon lecture de fond (tri sur la colonne).
// local = false : fichier cloud absent du disque, seules les propriétés en cache sont lues.
void Props_Request(uint64_t serial, uint32_t idx, std::wstring path, bool local, std::vector<PROPERTYKEY> keys,
                   bool urgent);
