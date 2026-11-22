#pragma once
#include "win.h"

#include <string>
#include <unordered_set>

// Index de Windows Search (SystemIndex), pour « contenu: » : le texte des
// documents (PDF, Office, texte… par les filtres IFilter installés) que Windows
// a déjà indexé. Hors des emplacements indexés, la recherche lit les fichiers
// texte elle-même (FileContainsText).

// 1 : dossier indexé ; 0 : hors des emplacements indexés ; -1 : Windows Search indisponible.
int WSearch_Indexed(const std::wstring& folder);

// Fichiers sous `folder` ("" : tout l'index) dont le contenu contient `word` (mots commençant par lui ;
// plusieurs mots : la phrase). Chemins complets en minuscules. false : requête impossible.
bool WSearch_Contains(const std::wstring& folder, const std::wstring& word, std::unordered_set<std::wstring>& out,
                      size_t max = 200000);

// « file:C:/a/b.txt » → « C:\a\b.txt » ; « file://srv/x » → « \\srv\x » ; vide : pas un fichier.
std::wstring WSearch_PathOfUrl(const std::wstring& url);

// Texte SQL de la requête (exposé pour les tests).
std::wstring WSearch_Sql(const std::wstring& folder, const std::wstring& word, size_t max);