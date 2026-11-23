#pragma once
#include "win.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Empreintes de fichiers (MD5, SHA-1, SHA-256) par BCrypt, en une seule lecture.

struct FileHashes {
    std::wstring md5, sha1, sha256;  // hexadécimal minuscule
    DWORD error = 0;                 // 0 : calculées ; ERROR_CANCELLED : interrompu
};

// Bloquant. progress : octets lus jusqu'ici ; cancel : demande d'arrêt.
FileHashes Hash_File(const std::wstring& path, const std::atomic<bool>* cancel = nullptr,
                     const std::function<void(uint64_t)>& progress = {});
FileHashes Hash_Bytes(const void* data, size_t len);

// « A1 b2 c3 » → « a1b2c3 » (espaces, tirets, deux-points retirés) : pour comparer une empreinte collée.
std::wstring Hash_Normalize(const std::wstring& s);

// Fenêtre « Empreintes » : calcule celles des fichiers (sur un thread), les affiche, compare avec une empreinte collée.
void Hash_Show(HWND owner, HFONT font, std::vector<std::wstring> paths);
