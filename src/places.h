#pragma once
#include "listing.h"

// Emplacements virtuels : Corbeille (« ::bin ») et Accueil (« ::home »).
// Les entrées portent le nom affiché ; DirData::target donne le chemin réel
// de chacune et DirData::origin son emplacement (d'origine pour la Corbeille).

// Corbeille de l'utilisateur sur tous les lecteurs locaux, lue directement
// dans $Recycle.Bin (fichiers $I) : date de suppression dans mtime.
void Recycle_Enumerate(DirData& d, const CancelToken& ct = {});

// Accueil : dossiers de l'Accès rapide, puis fichiers récents (dossier Recent
// de Windows, raccourcis non résolus : rien ne réveille un réseau). mtime :
// dernière utilisation. maxRecent : nombre de fichiers récents gardés.
// pins : dossiers de l'Accès rapide (déjà lus par le volet de navigation : rien à redemander au Shell).
struct NavFolder;
void Home_Enumerate(DirData& d, const std::vector<NavFolder>& pins, const CancelToken& ct = {}, size_t maxRecent = 40);

// Même chose avec un autre dossier de raccourcis récents (tests).
void Home_EnumerateFrom(DirData& d, const std::wstring& recentDir, const std::vector<NavFolder>& pins,
                        const CancelToken& ct, size_t maxRecent);

// Nom affiché d'un emplacement virtuel (« Corbeille », « Accueil »), "" sinon.
const wchar_t* Places_Title(const std::wstring& path);
// Texte tapé dans la barre d'adresse → emplacement virtuel (« corbeille », « shell:RecycleBinFolder »…), "" sinon.
std::wstring Places_FromAlias(const std::wstring& text);
