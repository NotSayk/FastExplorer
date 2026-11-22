#pragma once
#include "fastcopy.h"
#include "listing.h"

#include <memory>
#include <string>
#include <vector>

// Archives. Les ZIP sont lus et écrits par notre propre moteur (deflate.h) :
// parcourus comme des dossiers (« C:\x\a.zip\sous-dossier »), extraits et
// créés sur plusieurs threads. Les autres formats (7z, rar, tar, gz, xz…) sont
// extraits par tar.exe de Windows (libarchive).

struct ZipEntry {
    std::wstring name;  // chemin interne, séparateur '\', sans '\' final
    uint64_t csize = 0, usize = 0, offset = 0;  // offset : en-tête local
    uint32_t crc = 0, attr = 0;                  // attr : FILE_ATTRIBUTE_* (0 si inconnus)
    uint16_t method = 0, flags = 0;
    int64_t mtime = 0, ctime = 0, atime = 0;     // FILETIME UTC (0 : inconnu)
    bool dir = false;
    bool encrypted() const { return (flags & 1) != 0; }
};

struct ZipIndex {
    std::wstring path;
    int64_t writeTime = 0;
    uint64_t fileSize = 0;
    std::vector<ZipEntry> entries;  // dans l'ordre du répertoire central (noms dangereux exclus)
    DWORD error = 0;
};

// Lit le répertoire central (ZIP64 compris). error = ERROR_BAD_FORMAT si ce n'est pas un ZIP.
std::shared_ptr<const ZipIndex> Zip_ReadIndex(const std::wstring& zip);
// Idem, gardé en cache tant que le fichier ne change pas.
std::shared_ptr<const ZipIndex> Zip_Index(const std::wstring& zip);

// « C:\x\a.zip\sous\dossier » → archive « C:\x\a.zip », intérieur « sous\dossier ».
// false si aucun élément du chemin n'est un fichier .zip.
bool Archive_Split(const std::wstring& path, std::wstring& archive, std::wstring& inner);
bool Archive_IsZip(const std::wstring& name);
// Format que l'on sait extraire (ZIP par nous, les autres par tar.exe).
bool Archive_IsArchive(const std::wstring& name);
// Nom du dossier d'extraction : « photos.tar.gz » → « photos ».
std::wstring Archive_Stem(const std::wstring& name);

// Contenu d'un dossier d'archive (d.path = chemin complet, archive comprise).
void Archive_Enumerate(DirData& d, const CancelToken& ct = {});

struct ExtractOptions {
    std::wstring base;                 // dossier interne de départ ("" : racine)
    std::vector<std::wstring> items;   // éléments de `base` à extraire (noms) ; vide : tout
    bool overwrite = false;            // sinon un élément de premier niveau qui existe déjà devient « nom (2) »
};
// Extrait dans `dest` (créé au besoin). created : noms de premier niveau écrits dans dest.
// Autres formats que ZIP : tout est extrait par tar.exe (items ignorés).
DWORD Archive_Extract(const std::wstring& archive, const std::wstring& dest, const ExtractOptions& opt, FastProgress& prog,
                      std::vector<std::wstring>* created, std::wstring* detail);

// Crée `zip` (ne doit pas exister) avec les fichiers et dossiers `sources`
// (chemins complets ; chacun à la racine de l'archive, sous son nom).
DWORD Zip_Create(const std::wstring& zip, const std::vector<std::wstring>& sources, FastProgress& prog,
                 std::wstring* detail);

// Taille totale décompressée des éléments choisis (avant une extraction vers un dossier temporaire).
uint64_t Archive_SizeOf(const std::wstring& archive, const ExtractOptions& opt);
