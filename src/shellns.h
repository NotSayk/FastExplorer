#pragma once
#include "win.h"
#include "cancel.h"
#include "listing.h"

#include <string>
#include <vector>

// Emplacements du Shell sans chemin Win32 : téléphones et appareils photo (MTP),
// et tout dossier désigné par son nom d'analyse (« ::{GUID}\… »). Lus par
// IShellItem / IEnumShellItems ; ouverture, menu, copie et suppression passent
// par le Shell (comme dans l'Explorateur, c'est le pilote de l'appareil qui
// travaille). Le chemin d'un tel dossier est son nom d'analyse complet
// (SIGDN_DESKTOPABSOLUTEPARSING) ; DirData::target garde celui de chaque élément.

// « ::{…} » (pas ::bin, ::home, ::net).
inline bool ShellNs_IsPath(const std::wstring& p) { return p.size() > 3 && p[0] == L':' && p[1] == L':' && p[2] == L'{'; }

// Contenu du dossier (attributs, taille, date, icône du Shell). d.error : échec (appareil débranché…).
void ShellNs_Enumerate(DirData& d, const CancelToken& ct = {});

// Dossier parent ("" : Ce PC ou le Bureau). false : pas un emplacement du Shell.
bool ShellNs_Parent(const std::wstring& path, std::wstring& parent);

// Nom affiché (« Galaxy S24 », « Stockage interne partagé »).
std::wstring ShellNs_Title(const std::wstring& path);

struct ShellNsCrumb {
    std::wstring text, path;
};
// Du plus haut (Ce PC) au dossier lui-même ; vide si illisible.
std::vector<ShellNsCrumb> ShellNs_Chain(const std::wstring& path);

// Appareils de Ce PC sans lettre de lecteur (téléphones, appareils photo, MTP).
struct ShellNsDevice {
    std::wstring path, name;
    int icon = -1;  // index de la liste d'images système
};
std::vector<ShellNsDevice> ShellNs_Devices();

// Ouvre un élément par le Shell (un fichier de l'appareil est copié puis ouvert, comme dans l'Explorateur).
bool ShellNs_Open(HWND owner, const std::wstring& path);

// Opérations du Shell (sa fenêtre de progression, ses confirmations). Sur un thread à part ;
// done(ok) est appelé sur ce thread à la fin (peut être vide).
// Colle le presse-papiers dans le dossier.
void ShellNs_Paste(HWND owner, const std::wstring& folder, std::function<void(bool)> done = {});
// Copie des fichiers (chemins Win32 ou du Shell) dans le dossier.
void ShellNs_CopyInto(HWND owner, const std::vector<std::wstring>& items, const std::wstring& folder,
                      std::function<void(bool)> done = {});
void ShellNs_Delete(HWND owner, const std::vector<std::wstring>& items, std::function<void(bool)> done = {});
void ShellNs_Rename(HWND owner, const std::wstring& item, const std::wstring& newName, std::function<void(bool)> done = {});
