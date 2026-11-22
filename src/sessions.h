#pragma once
#include "win.h"

#include <string>
#include <vector>

// Ensembles d'onglets : ceux de la dernière fenêtre fermée (rouverts au
// démarrage sans dossier en argument) et les sessions nommées. Signets :
// dossiers rangés sous Alt+1…9. Tout est dans HKCU\Software\FastExplorer.

struct SessionTabs {
    std::vector<std::wstring> paths;  // un par onglet ; "" = Ce PC
    int active = 0;                   // onglet affiché
    int other = -1;                   // double panneau : onglet du panneau inactif (-1 : aucun)
};

// Forme REG_MULTI_SZ, chaque chaîne suivie de son zéro (sans le zéro final de la
// liste) : « actif,autre », puis un chemin par onglet précédé de « > » (un
// chemin vide, Ce PC, terminerait sinon la liste).
std::wstring Session_Encode(const SessionTabs& s);
bool Session_Decode(const std::wstring& multi, SessionTabs& s);  // false : vide ou illisible

bool Session_SaveLast(const SessionTabs& s);
bool Session_LoadLast(SessionTabs& s);
bool Session_RestoreEnabled();  // rouvrir les onglets au démarrage (oui par défaut)
void Session_SetRestore(bool on);

std::vector<std::wstring> Session_Names();  // sessions nommées, dans l'ordre alphabétique
bool Session_Save(const std::wstring& name, const SessionTabs& s);
bool Session_Load(const std::wstring& name, SessionTabs& s);
bool Session_Delete(const std::wstring& name);

// Recherches récentes (HKCU SearchHistory, REG_MULTI_SZ) : la plus récente d'abord, sans doublon (casse ignorée).
constexpr size_t kSearchHistoryMax = 25;
std::vector<std::wstring> HistoryPush(std::vector<std::wstring> list, const std::wstring& query, size_t max = kSearchHistoryMax);
std::vector<std::wstring> SearchHistory_Get();
void SearchHistory_Add(const std::wstring& query);
void SearchHistory_Remove(const std::wstring& query);
void SearchHistory_Clear();

constexpr int kBookmarks = 9;
bool Bookmark_Get(int n, std::wstring& path);  // n = 1…9 ; false : aucun signet
bool Bookmark_Set(int n, const std::wstring& path);
bool Bookmark_Clear(int n);

// Petite boîte de saisie (nom d'une session…). false : annulée ou texte vide.
bool AskText(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& text);
