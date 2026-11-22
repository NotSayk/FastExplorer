#pragma once
#include "win.h"

#include <cstdint>
#include <functional>
#include <vector>

// DEFLATE (RFC 1951) et CRC-32, sans bibliothèque externe : de quoi lire et
// écrire les ZIP nous-mêmes (archive.h).

// CRC-32 (ZIP, gzip). crc : valeur précédente (0 au départ).
uint32_t Crc32(uint32_t crc, const void* data, size_t n);
// CRC de A puis B, connaissant crc(A), crc(B) et la longueur de B (calcul en parallèle).
uint32_t Crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB);

// Décompression en flux : `read` remplit un tampon (0 : fin des données),
// `write` reçoit la sortie par morceaux (false : arrêter). false si les
// données sont invalides, tronquées ou si `write` a demandé l'arrêt.
bool InflateStream(const std::function<size_t(BYTE*, size_t)>& read,
                   const std::function<bool(const BYTE*, size_t)>& write);
// Même chose en mémoire (tests, petits éléments).
bool Inflate(const BYTE* in, size_t n, std::vector<BYTE>& out);

// Compression d'un morceau : data[0, dictLen) est ce qui précède (dictionnaire,
// 32 Ko au plus utiles), data[dictLen, dictLen + len) le morceau. Les morceaux
// compressés séparément (en parallèle) se mettent bout à bout : chacun finit
// aligné sur un octet ; seul le dernier porte `final`.
void DeflateChunk(const BYTE* data, size_t dictLen, size_t len, bool final, std::vector<BYTE>& out);
// Tout un tampon d'un coup.
std::vector<BYTE> Deflate(const BYTE* data, size_t n);
