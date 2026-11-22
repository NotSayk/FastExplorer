#pragma once
#include "listing.h"

#include <string>

// Réseau. « ::net » (Réseau) : les serveurs connus — ceux déjà visités
// (mémorisés) et ceux des lecteurs réseau connectés. « \\serveur » : ses
// partages de fichiers (NetShareEnum), comme des dossiers. Les identifiants
// sont demandés par Windows (WNetAddConnection3).

inline constexpr const wchar_t* kNetPath = L"::net";

// « \\serveur » seul (pas de partage).
bool Net_IsServerPath(const std::wstring& p);
// « \\serveur\partage\x » → « \\serveur » ; "" si ce n'est pas un chemin réseau.
std::wstring Net_ServerOf(const std::wstring& p);

// Partages de d.path (« \\serveur »). Partages d'administration (« C$ ») : attribut caché.
// origin = commentaire de chaque partage ; d.server = true.
void Net_EnumShares(DirData& d, const CancelToken& ct = {});
// Serveurs connus (d.path = kNetPath) : target = « \\serveur », origin = lecteurs connectés, mtime = dernière visite.
void Net_EnumServers(DirData& d, const CancelToken& ct = {});

// Mémorise le serveur d'un chemin réseau ouvert avec succès (dernière visite).
void Net_Remember(const std::wstring& path);
// Retire un serveur de la liste mémorisée.
void Net_Forget(const std::wstring& server);

// Erreur d'accès qu'une connexion avec d'autres identifiants peut lever.
bool Net_IsAuthError(DWORD e);
// Demande des identifiants (fenêtre de Windows) pour `path`, puis connecte.
// NO_ERROR : connecté ; ERROR_CANCELLED : annulé par l'utilisateur.
DWORD Net_Connect(HWND owner, const std::wstring& path);
// Texte d'une erreur réseau (codes NERR_* compris).
std::wstring Net_ErrorText(DWORD e);
