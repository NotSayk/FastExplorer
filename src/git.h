#pragma once
#include "win.h"

#include <map>
#include <string>
#include <vector>

// Intégration Git : dans un dépôt, la branche courante, l'état des fichiers (git status)
// et le changement de branche. git.exe est lancé sans console ; rien n'est fait s'il manque.

struct GitStatus {
    std::wstring root;    // dossier du dépôt (contient .git)
    std::wstring branch;  // « main » ; « (détachée) » si HEAD détachée
    // Chemin relatif à root, en minuscules, séparateur « \ » ; un dossier entier finit par « \ ».
    // Code : M A D R C T (modifié…), ? non suivi, ! ignoré, U conflit.
    std::map<std::wstring, wchar_t> files;
    bool ok = false;
    DWORD error = 0;
    std::wstring message;  // sortie d'erreur de git
};

// Dépôt contenant `dir` (dossier qui a un .git, dossier ou fichier) ; "" sinon. Chemins locaux et réseau.
std::wstring Git_FindRoot(const std::wstring& dir);
bool Git_Available();
GitStatus Git_Status(const std::wstring& root, DWORD timeoutMs = 10000);
// Analyse de « git status --porcelain=v1 -z -b » (pour les tests).
void Git_ParseStatus(const std::string& out, GitStatus& st);
// États des éléments du dossier `dir` (dans le dépôt) : nom en minuscules → code.
// Dossier : son propre code (non suivi / ignoré en entier), sinon « m » si quelque chose a changé dedans.
std::map<std::wstring, wchar_t> Git_FolderStates(const GitStatus& st, const std::wstring& dir);
const wchar_t* Git_StateText(wchar_t code);

std::vector<std::wstring> Git_Branches(const std::wstring& root);  // branches locales
// git switch <branch>. 0 : fait ; sinon code de sortie, message = ce que git a écrit.
DWORD Git_Switch(const std::wstring& root, const std::wstring& branch, std::wstring& message);

// Lance git avec ces arguments (dans root) ; sortie standard et d'erreur, code de sortie. false : pas lancé.
bool Git_Run(const std::wstring& root, const std::wstring& args, std::string& out, std::string& err, DWORD& exitCode,
             DWORD timeoutMs = 10000);
