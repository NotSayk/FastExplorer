#pragma once
#include "win.h"

#include <winioctl.h>

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

// Recherche instantanée sur tout le disque (à la Everything).
//
// Un service Windows (installé une fois, avec une seule autorisation
// administrateur) lit la table des fichiers de chaque volume NTFS
// (FSCTL_ENUM_USN_DATA : ~1 à 3 s pour des millions de fichiers), la garde en
// mémoire sous une forme compacte et la tient à jour avec le journal USN.
// L'application, lancée normalement, l'interroge par un canal nommé réservé à
// l'utilisateur qui l'a installé. Sans service, la recherche parcourt les
// dossiers comme avant.

struct NameMatcher;

constexpr uint32_t kIndexMaxQuery = 1024, kIndexMaxScope = 32768;

// Éléments d'une recherche (indépendants du transport).
struct IndexQuery {
    std::wstring query;  // mêmes règles que NameMatches
    std::wstring scope;  // dossier où chercher (sous-dossiers compris) ; "" : tout le PC
    bool showHidden = true, showSuperHidden = true;
    uint32_t max = 100000;
};

struct IndexHit {
    std::wstring path;  // relatif à scope (chemin complet si scope est vide)
    uint32_t attr = 0;  // FILE_ATTRIBUTE_DIRECTORY / HIDDEN / SYSTEM / REPARSE_POINT
};

enum class IndexStatus : uint32_t {
    Ok = 0,
    NotIndexed = 1,   // dossier hors des volumes indexés (réseau, FAT, clé ajoutée depuis)
    Building = 2,     // index en construction
    Unavailable = 3,  // pas de service (ou il ne répond pas)
    BadRequest = 4,
};

// Index d'un volume : un nœud par enregistrement de la table des fichiers
// (12 octets + le nom), rangé par numéro d'enregistrement.
class VolumeIndex {
public:
    explicit VolumeIndex(wchar_t letter = L'C') : letter_(letter) {}
    wchar_t letter() const { return letter_; }

    // Construction et mise à jour (sous verrou exclusif : lock()).
    void Clear();
    void Put(uint64_t frn, uint64_t parentFrn, const wchar_t* name, uint32_t n, uint32_t attr);
    void Remove(uint64_t frn);
    void Apply(const USN_RECORD_V2& r);  // une entrée du journal
    void Compact();                      // récupère la place des noms remplacés

    // Recherche (sous verrou partagé). scopeRec : enregistrement du dossier
    // où chercher (kNoScope : tout le volume, chemins complets « C:\… »).
    static constexpr uint64_t kNoScope = ~0ull;
    void Search(const NameMatcher& m, const IndexQuery& q, uint64_t scopeRec, std::vector<IndexHit>& out,
                bool& truncated) const;
    // Chemin complet d'un enregistrement ("" si inconnu).
    std::wstring PathOf(uint64_t rec) const;
    size_t Count() const { return count_; }
    size_t Bytes() const { return nodes_.capacity() * sizeof(Node) + pool_.capacity() * sizeof(wchar_t); }

    std::shared_mutex& lock() const { return mu_; }

    static uint64_t Rec(uint64_t frn) { return frn & 0x0000FFFFFFFFFFFFull; }
    static constexpr uint32_t kRootRec = 5;  // dossier racine d'un volume NTFS

private:
    struct Node {
        uint32_t parent;   // numéro d'enregistrement du dossier parent
        uint32_t nameOff;  // dans pool_
        uint16_t nameLen;  // 0 : enregistrement libre
        uint16_t flags;
    };
    enum : uint16_t { F_DIR = 1, F_HIDDEN = 2, F_SYSTEM = 4, F_REPARSE = 8, F_USED = 0x8000 };
    static uint16_t Flags(uint32_t attr);
    bool Shown(uint16_t f, const IndexQuery& q) const;
    // Chemin de rec jusqu'à stop (exclu) ; false si la chaîne est rompue ou
    // passe par un dossier masqué (selon q). stop = kNoScope : jusqu'à la racine.
    bool BuildPath(uint32_t rec, uint64_t stop, const IndexQuery* q, std::wstring& out) const;

    wchar_t letter_;
    std::vector<Node> nodes_;
    std::vector<wchar_t> pool_;
    size_t garbage_ = 0, count_ = 0;
    mutable std::shared_mutex mu_;
};

// ───────── Service (process FastExplorer.exe --service, en LocalSystem) ─────────

// Point d'entrée du service ; rend la main quand il s'arrête.
int IndexService_Run();
// Installation / désinstallation (process élevé). Code de sortie : 0 = réussi, sinon l'erreur Windows.
int IndexService_Install(const std::wstring& userSid);
int IndexService_Uninstall();

// ───────── Client (application) ─────────

bool Index_ServiceInstalled();
// Interroge le service ; Unavailable s'il n'est pas là. Bloquant (à appeler hors du thread de l'interface).
IndexStatus Index_Search(const IndexQuery& q, std::vector<IndexHit>& hits, bool& truncated);
// SID de l'utilisateur courant (« S-1-5-21-… »).
std::wstring Index_CurrentUserSid();

// Protocole (exposé pour les tests) : demande sérialisée et lecture côté service.
std::vector<BYTE> Index_EncodeQuery(const IndexQuery& q);
bool Index_DecodeQuery(const BYTE* p, size_t n, IndexQuery& q);
