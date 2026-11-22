#include "deflate.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <queue>

namespace {

// ───────────────────────────── CRC-32 (tranches de 8 octets) ─────────────────────────────

struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const CrcTables& Tables() {
    static const CrcTables t;
    return t;
}

uint32_t Gf2Times(const uint32_t* mat, uint32_t vec) {
    uint32_t sum = 0;
    for (; vec; vec >>= 1, ++mat)
        if (vec & 1) sum ^= *mat;
    return sum;
}
void Gf2Square(uint32_t* sq, const uint32_t* mat) {
    for (int n = 0; n < 32; ++n) sq[n] = Gf2Times(mat, mat[n]);
}

// ───────────────────────────── Tables communes ─────────────────────────────

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

uint32_t Reverse(uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; ++i, code >>= 1) r = (r << 1) | (code & 1);
    return r;
}

// ───────────────────────────── Décompression ─────────────────────────────

// Table de décodage directe : index = prochains bits (LSB d'abord), valeur = symbole << 4 | longueur.
struct Huff {
    std::vector<uint16_t> t;
    int bits = 0;
    bool Build(const uint8_t* len, int n) {
        int count[16] = {};
        for (int i = 0; i < n; ++i) ++count[len[i]];
        count[0] = 0;
        bits = 0;
        for (int l = 15; l > 0; --l)
            if (count[l]) { bits = l; break; }
        // Code trop rempli : invalide (incomplet : toléré, une entrée vide signale l'erreur au décodage).
        int left = 1;
        for (int l = 1; l <= 15; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false;
        }
        t.assign(bits ? (size_t)1 << bits : 1, 0);
        if (!bits) return true;
        int next[16] = {};
        int code = 0;
        for (int l = 1; l <= 15; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        for (int s = 0; s < n; ++s) {
            const int l = len[s];
            if (!l) continue;
            const uint32_t r = Reverse((uint32_t)next[l]++, l);
            for (uint32_t k = r; k < t.size(); k += 1u << l) t[k] = (uint16_t)((s << 4) | l);
        }
        return true;
    }
};

class Inflater {
public:
    Inflater(const std::function<size_t(BYTE*, size_t)>& rd, const std::function<bool(const BYTE*, size_t)>& wr)
        : read_(rd), write_(wr), win_(kWin) {}

    bool Run() {
        bool final = false;
        do {
            if (!Need(3)) return false;
            final = Bits(1) != 0;
            const uint32_t type = Bits(2);
            if (type == 0) {
                if (!Stored()) return false;
            } else if (type == 1) {
                const FixedHuff& f = Fixed();
                if (!f.ok || !Codes(f.lit, f.dist)) return false;
            } else if (type == 2) {
                if (!Dynamic() || !Codes(lit_, dist_)) return false;
            } else {
                return false;
            }
        } while (!final);
        return Flush();
    }

private:
    static constexpr size_t kWin = 1 << 22;  // 4 Mo : écrits par gros morceaux, 32 Ko gardés pour les références

    bool Need(int n) {
        while (nb_ < n) {
            if (ip_ == il_) {
                il_ = read_(in_, sizeof in_);
                ip_ = 0;
                if (!il_) {  // fin des données : zéros (tolérés seulement s'ils ne sont pas lus pour de vrai)
                    if (++overrun_ > 8) return false;
                    nb_ += 8;
                    continue;
                }
            }
            bitbuf_ |= (uint64_t)in_[ip_++] << nb_;
            nb_ += 8;
        }
        return true;
    }
    uint32_t Bits(int n) {  // après Need(n)
        const uint32_t v = (uint32_t)(bitbuf_ & ((1ull << n) - 1));
        bitbuf_ >>= n;
        nb_ -= n;
        return v;
    }
    int Decode(const Huff& h) {
        if (!h.bits || !Need(h.bits)) return -1;
        const uint16_t e = h.t[bitbuf_ & (h.t.size() - 1)];
        const int l = e & 15;
        if (!l) return -1;
        bitbuf_ >>= l;
        nb_ -= l;
        return e >> 4;
    }

    bool Flush() {
        if (op_ > flushed_ && !write_(win_.data() + flushed_, op_ - flushed_)) return false;
        flushed_ = op_;
        return true;
    }
    // Place pour au moins `n` octets (n ≤ 258) : écrit le tampon et garde les 32 derniers Ko.
    bool Room(size_t n) {
        if (op_ + n <= kWin) return true;
        if (!Flush()) return false;
        memmove(win_.data(), win_.data() + op_ - 32768, 32768);
        op_ = flushed_ = 32768;
        return true;
    }

    bool Stored() {
        const int drop = nb_ & 7;
        bitbuf_ >>= drop;
        nb_ -= drop;
        if (!Need(32)) return false;
        const uint32_t len = Bits(16), nlen = Bits(16);
        if ((len ^ 0xFFFF) != nlen || overrun_) return false;
        uint32_t left = len;
        while (left && nb_ >= 8) {  // octets déjà dans le tampon de bits
            if (!Room(1)) return false;
            win_[op_++] = (BYTE)Bits(8);
            --left;
        }
        while (left) {
            if (ip_ == il_) {
                il_ = read_(in_, sizeof in_);
                ip_ = 0;
                if (!il_) return false;
            }
            if (!Room(1)) return false;
            const size_t n = std::min<size_t>({left, il_ - ip_, kWin - op_});
            memcpy(win_.data() + op_, in_ + ip_, n);
            op_ += n;
            ip_ += n;
            left -= (uint32_t)n;
        }
        return true;
    }

    struct FixedHuff {
        Huff lit, dist;
        bool ok;
        FixedHuff() {
            uint8_t l[288];
            for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            uint8_t d[30];
            std::fill(d, d + 30, (uint8_t)5);
            ok = lit.Build(l, 288) && dist.Build(d, 30);
        }
    };
    static const FixedHuff& Fixed() {  // construites une fois (thread-safe)
        static const FixedHuff f;
        return f;
    }

    bool Dynamic() {
        if (!Need(14)) return false;
        const int hlit = (int)Bits(5) + 257, hdist = (int)Bits(5) + 1, hclen = (int)Bits(4) + 4;
        if (hlit > 286 || hdist > 30) return false;
        uint8_t cl[19] = {};
        for (int i = 0; i < hclen; ++i) {
            if (!Need(3)) return false;
            cl[kClenOrder[i]] = (uint8_t)Bits(3);
        }
        Huff ch;
        if (!ch.Build(cl, 19)) return false;
        uint8_t lens[286 + 30] = {};
        for (int i = 0; i < hlit + hdist;) {
            const int s = Decode(ch);
            if (s < 0) return false;
            if (s < 16) {
                lens[i++] = (uint8_t)s;
                continue;
            }
            int rep = 0;
            uint8_t val = 0;
            if (s == 16) {
                if (!i || !Need(2)) return false;
                val = lens[i - 1];
                rep = 3 + (int)Bits(2);
            } else if (s == 17) {
                if (!Need(3)) return false;
                rep = 3 + (int)Bits(3);
            } else {
                if (!Need(7)) return false;
                rep = 11 + (int)Bits(7);
            }
            if (i + rep > hlit + hdist) return false;
            while (rep--) lens[i++] = val;
        }
        if (!lens[256]) return false;  // pas de fin de bloc
        return lit_.Build(lens, hlit) && dist_.Build(lens + hlit, hdist);
    }

    bool Codes(const Huff& lit, const Huff& dist) {
        for (;;) {
            if (!Room(258)) return false;
            const int s = Decode(lit);
            if (s < 0) return false;
            if (s < 256) {
                win_[op_++] = (BYTE)s;
                continue;
            }
            if (s == 256) return overrun_ == 0 || nb_ >= overrun_ * 8;
            const int li = s - 257;
            if (li >= 29) return false;
            if (!Need(kLenExtra[li])) return false;
            const size_t len = kLenBase[li] + Bits(kLenExtra[li]);
            const int ds = Decode(dist);
            if (ds < 0 || ds >= 30 || !Need(kDistExtra[ds])) return false;
            const size_t d = kDistBase[ds] + Bits(kDistExtra[ds]);
            if (d > op_) return false;
            BYTE* o = win_.data() + op_;
            const BYTE* from = o - d;
            if (d >= len) memcpy(o, from, len);
            else
                for (size_t k = 0; k < len; ++k) o[k] = from[k];
            op_ += len;
        }
    }

    const std::function<size_t(BYTE*, size_t)>& read_;
    const std::function<bool(const BYTE*, size_t)>& write_;
    BYTE in_[1 << 16];
    size_t ip_ = 0, il_ = 0;
    uint64_t bitbuf_ = 0;
    int nb_ = 0, overrun_ = 0;
    std::vector<BYTE> win_;
    size_t op_ = 0, flushed_ = 0;
    Huff lit_, dist_;
};

// ───────────────────────────── Compression ─────────────────────────────

struct BitOut {
    std::vector<BYTE>& out;
    uint64_t buf = 0;
    int n = 0;
    void Put(uint32_t v, int bits) {
        buf |= (uint64_t)v << n;
        n += bits;
        while (n >= 8) {
            out.push_back((BYTE)buf);
            buf >>= 8;
            n -= 8;
        }
    }
    void Align() {
        if (n) Put(0, 8 - n);
    }
};

// Longueurs de code limitées à `limit` bits (Huffman puis ajustement de Kraft, comme miniz).
void BuildLengths(const uint32_t* freq, int n, int limit, uint8_t* len) {
    std::fill(len, len + n, (uint8_t)0);
    std::vector<int> syms;
    for (int i = 0; i < n; ++i)
        if (freq[i]) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1) {
        len[syms[0]] = 1;
        return;
    }
    // Arbre de Huffman (n ≤ 288 : une file de priorité suffit).
    struct Node {
        uint64_t f;
        int id;
        bool operator>(const Node& o) const { return f != o.f ? f > o.f : id > o.id; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> q;
    std::vector<int> parent(2 * syms.size(), -1);
    for (size_t i = 0; i < syms.size(); ++i) q.push({freq[syms[i]], (int)i});
    int next = (int)syms.size();
    while (q.size() > 1) {
        const Node a = q.top();
        q.pop();
        const Node b = q.top();
        q.pop();
        parent[a.id] = parent[b.id] = next;
        q.push({a.f + b.f, next++});
    }
    int count[64] = {};
    std::vector<int> depth(syms.size());
    for (size_t i = 0; i < syms.size(); ++i) {
        int d = 0;
        for (int p = (int)i; parent[p] >= 0; p = parent[p]) ++d;
        depth[i] = std::min(d, 63);
        ++count[depth[i]];
    }
    for (int i = limit + 1; i < 64; ++i) {
        count[limit] += count[i];
        count[i] = 0;
    }
    uint64_t total = 0;
    for (int i = limit; i > 0; --i) total += (uint64_t)count[i] << (limit - i);
    while (total != (1ull << limit)) {
        --count[limit];
        for (int i = limit - 1; i > 0; --i)
            if (count[i]) {
                --count[i];
                count[i + 1] += 2;
                break;
            }
        --total;
    }
    // Les plus fréquents reçoivent les codes les plus courts.
    std::vector<int> order(syms.begin(), syms.end());
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return freq[a] > freq[b]; });
    size_t k = 0;
    for (int l = 1; l <= limit; ++l)
        for (int c = 0; c < count[l]; ++c) len[order[k++]] = (uint8_t)l;
}

void Codes(const uint8_t* len, int n, uint16_t* code) {
    int count[16] = {}, next[16] = {};
    for (int i = 0; i < n; ++i) ++count[len[i]];
    count[0] = 0;
    int c = 0;
    for (int l = 1; l <= 15; ++l) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (int i = 0; i < n; ++i) code[i] = len[i] ? (uint16_t)Reverse((uint32_t)next[len[i]]++, len[i]) : 0;
}

struct Tabs {
    uint8_t lenCode[259];     // longueur 3..258 → index 0..28
    uint8_t distCode[32769];  // distance 1..32768 → 0..29
    Tabs() {
        for (int i = 0; i < 29; ++i)
            for (int l = kLenBase[i]; l < (i == 28 ? 259 : kLenBase[i + 1]); ++l) lenCode[l] = (uint8_t)i;
        lenCode[258] = 28;
        for (int i = 0; i < 30; ++i)
            for (int d = kDistBase[i]; d < (i == 29 ? 32769 : kDistBase[i + 1]); ++d) distCode[d] = (uint8_t)i;
    }
};
const Tabs& T() {
    static const Tabs t;
    return t;
}

struct Sym {
    uint16_t lit;   // littéral, ou longueur (3..258) si dist
    uint16_t dist;  // 0 : littéral
};

// Au moins deux codes : certains décodeurs refusent un code à un seul symbole.
void EnsureTwo(uint8_t* len, int n) {
    int nz = 0;
    for (int i = 0; i < n; ++i) nz += len[i] != 0;
    for (int i = 0; i < n && nz < 2; ++i)
        if (!len[i]) {
            len[i] = 1;
            ++nz;
        }
    // deux codes de longueur 1 au plus haut : si un seul existait avec une autre longueur, on normalise
    if (nz == 2) {
        for (int i = 0; i < n; ++i)
            if (len[i]) len[i] = 1;
    }
}

// Écrit un bloc (dynamique, fixe ou stocké : le plus court) pour syms[a, b) couvrant raw[0, rawLen).
void EmitBlock(BitOut& bo, const Sym* s, size_t ns, const BYTE* raw, size_t rawLen, bool final) {
    const Tabs& tb = T();
    uint32_t lf[286] = {}, df[30] = {};
    for (size_t i = 0; i < ns; ++i) {
        if (!s[i].dist) ++lf[s[i].lit];
        else {
            ++lf[257 + tb.lenCode[s[i].lit]];
            ++df[tb.distCode[s[i].dist]];
        }
    }
    lf[256] = 1;
    uint8_t ll[286], dl[30];
    BuildLengths(lf, 286, 15, ll);
    BuildLengths(df, 30, 15, dl);
    EnsureTwo(ll, 286);
    EnsureTwo(dl, 30);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) --hlit;
    while (hdist > 1 && !dl[hdist - 1]) --hdist;
    // Longueurs de code, compressées (16 : répéter, 17 / 18 : zéros).
    uint8_t all[316];
    memcpy(all, ll, hlit);
    memcpy(all + hlit, dl, hdist);
    const int total = hlit + hdist;
    struct Rle {
        uint8_t sym, extra;
    };
    std::vector<Rle> rle;
    for (int i = 0; i < total;) {
        const uint8_t v = all[i];
        int run = 1;
        while (i + run < total && all[i + run] == v) ++run;
        if (v == 0 && run >= 3) {
            int r = run;
            while (r >= 11) {
                const int k = std::min(r, 138);
                rle.push_back({18, (uint8_t)(k - 11)});
                r -= k;
            }
            if (r >= 3) {
                rle.push_back({17, (uint8_t)(r - 3)});
                r = 0;
            }
            while (r--) rle.push_back({0, 0});
        } else if (v != 0 && run >= 4) {
            rle.push_back({v, 0});
            int r = run - 1;
            while (r >= 3) {
                const int k = std::min(r, 6);
                rle.push_back({16, (uint8_t)(k - 3)});
                r -= k;
            }
            while (r--) rle.push_back({v, 0});
        } else {
            for (int k = 0; k < run; ++k) rle.push_back({v, 0});
        }
        i += run;
    }
    uint32_t cf[19] = {};
    for (const Rle& r : rle) ++cf[r.sym];
    uint8_t cl[19];
    BuildLengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && !cl[kClenOrder[hclen - 1]]) --hclen;

    // Coûts en bits.
    uint64_t dyn = 3 + 14 + 3ull * hclen, fix = 3;
    for (const Rle& r : rle) dyn += cl[r.sym] + (r.sym == 16 ? 2 : r.sym == 17 ? 3 : r.sym == 18 ? 7 : 0);
    for (int i = 0; i < 286; ++i) {
        if (!lf[i]) continue;
        const int fl = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        const int extra = i > 256 ? kLenExtra[i - 257] : 0;
        dyn += (uint64_t)lf[i] * (ll[i] + extra);
        fix += (uint64_t)lf[i] * (fl + extra);
    }
    for (int i = 0; i < 30; ++i) {
        dyn += (uint64_t)df[i] * (dl[i] + kDistExtra[i]);
        fix += (uint64_t)df[i] * (5 + kDistExtra[i]);
    }
    const uint64_t stored = (rawLen / 65535 + 1) * (5 * 8 + 3) + rawLen * 8 + 7;

    if (stored < dyn && stored < fix) {
        size_t off = 0;
        do {
            const size_t n = std::min<size_t>(rawLen - off, 65535);
            const bool last = off + n >= rawLen;
            bo.Put(final && last ? 1 : 0, 1);
            bo.Put(0, 2);
            bo.Align();
            bo.Put((uint32_t)n, 16);
            bo.Put((uint32_t)(n ^ 0xFFFF), 16);
            bo.out.insert(bo.out.end(), raw + off, raw + off + n);
            off += n;
        } while (off < rawLen);
        return;
    }
    uint16_t lc[286], dc[30];
    uint8_t fl[288], fd[30];
    const bool useFix = fix <= dyn;
    if (useFix) {
        for (int i = 0; i < 288; ++i) fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        std::fill(fd, fd + 30, (uint8_t)5);
        uint16_t lc2[288];
        Codes(fl, 288, lc2);
        memcpy(lc, lc2, sizeof lc);
        Codes(fd, 30, dc);
        memcpy(ll, fl, 286);
        memcpy(dl, fd, 30);
        bo.Put(final ? 1 : 0, 1);
        bo.Put(1, 2);
    } else {
        Codes(ll, 286, lc);
        Codes(dl, 30, dc);
        uint16_t cc[19];
        Codes(cl, 19, cc);
        bo.Put(final ? 1 : 0, 1);
        bo.Put(2, 2);
        bo.Put((uint32_t)(hlit - 257), 5);
        bo.Put((uint32_t)(hdist - 1), 5);
        bo.Put((uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; ++i) bo.Put(cl[kClenOrder[i]], 3);
        for (const Rle& r : rle) {
            bo.Put(cc[r.sym], cl[r.sym]);
            if (r.sym == 16) bo.Put(r.extra, 2);
            else if (r.sym == 17) bo.Put(r.extra, 3);
            else if (r.sym == 18) bo.Put(r.extra, 7);
        }
    }
    for (size_t i = 0; i < ns; ++i) {
        if (!s[i].dist) {
            bo.Put(lc[s[i].lit], ll[s[i].lit]);
            continue;
        }
        const int li = tb.lenCode[s[i].lit];
        bo.Put(lc[257 + li], ll[257 + li]);
        if (kLenExtra[li]) bo.Put(s[i].lit - kLenBase[li], kLenExtra[li]);
        const int di = tb.distCode[s[i].dist];
        bo.Put(dc[di], dl[di]);
        if (kDistExtra[di]) bo.Put(s[i].dist - kDistBase[di], kDistExtra[di]);
    }
    bo.Put(lc[256], ll[256]);
}

constexpr int kHashBits = 15;
constexpr int kMaxChain = 32;
constexpr size_t kNice = 128;
constexpr size_t kLazy = 32;

}  // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t n) {
    const CrcTables& T = Tables();
    const BYTE* p = (const BYTE*)data;
    uint32_t c = ~crc;
    while (n >= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= c;
        c = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
            T.t[3][b & 0xFF] ^ T.t[2][(b >> 8) & 0xFF] ^ T.t[1][(b >> 16) & 0xFF] ^ T.t[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = T.t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint32_t Crc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    if (!len2) return crc1;
    uint32_t even[32], odd[32];
    odd[0] = 0xEDB88320u;
    uint32_t row = 1;
    for (int n = 1; n < 32; ++n) {
        odd[n] = row;
        row <<= 1;
    }
    Gf2Square(even, odd);
    Gf2Square(odd, even);
    do {
        Gf2Square(even, odd);
        if (len2 & 1) crc1 = Gf2Times(even, crc1);
        len2 >>= 1;
        if (!len2) break;
        Gf2Square(odd, even);
        if (len2 & 1) crc1 = Gf2Times(odd, crc1);
        len2 >>= 1;
    } while (len2);
    return crc1 ^ crc2;
}

bool InflateStream(const std::function<size_t(BYTE*, size_t)>& read,
                   const std::function<bool(const BYTE*, size_t)>& write) {
    auto inf = std::make_unique<Inflater>(read, write);
    return inf->Run();
}

bool Inflate(const BYTE* in, size_t n, std::vector<BYTE>& out) {
    size_t pos = 0;
    out.clear();
    return InflateStream(
        [&](BYTE* b, size_t cap) {
            const size_t k = std::min(cap, n - pos);
            memcpy(b, in + pos, k);
            pos += k;
            return k;
        },
        [&](const BYTE* b, size_t k) {
            out.insert(out.end(), b, b + k);
            return true;
        });
}

void DeflateChunk(const BYTE* data, size_t dictLen, size_t len, bool final, std::vector<BYTE>& out) {
    BitOut bo{out};
    if (dictLen > 32768) {
        data += dictLen - 32768;
        dictLen = 32768;
    }
    if (!len) {
        if (final) {  // bloc fixe vide : fin de bloc seule
            bo.Put(1, 1);
            bo.Put(1, 2);
            bo.Put(0, 7);
        } else {  // bloc stocké vide : alignement
            bo.Put(0, 3);
            bo.Align();
            bo.Put(0, 16);
            bo.Put(0xFFFF, 16);
        }
        bo.Align();
        return;
    }
    const size_t end = dictLen + len;
    std::vector<int32_t> head((size_t)1 << kHashBits, -1);
    std::vector<int32_t> prev(end);
    auto hash = [&](size_t i) {
        const uint32_t v = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= end) return;
        const uint32_t h = hash(i);
        prev[i] = head[h];
        head[h] = (int32_t)i;
    };
    auto match = [&](size_t i, size_t& bestDist) {
        size_t best = 0;
        if (i + 2 >= end) return best;
        const size_t maxLen = std::min<size_t>(258, end - i);
        int chain = head[hash(i)];
        for (int tries = kMaxChain; chain >= 0 && tries--; chain = prev[chain]) {
            const size_t d = i - (size_t)chain;
            if (d > 32768) break;
            const BYTE* a = data + i;
            const BYTE* b = data + chain;
            if (a[best] != b[best] || a[0] != b[0]) continue;
            size_t l = 0;
            while (l + 8 <= maxLen) {
                uint64_t x, y;
                memcpy(&x, a + l, 8);
                memcpy(&y, b + l, 8);
                if (x != y) {
                    unsigned long bit;
                    _BitScanForward64(&bit, x ^ y);
                    l += bit >> 3;
                    goto done;
                }
                l += 8;
            }
            while (l < maxLen && a[l] == b[l]) ++l;
        done:
            if (l > best) {
                best = l;
                bestDist = d;
                if (l >= kNice || l >= maxLen) break;
            }
        }
        return best >= 3 ? best : 0;
    };
    for (size_t i = dictLen > 32768 ? dictLen - 32768 : 0; i < dictLen; ++i) insert(i);

    std::vector<Sym> syms;
    syms.reserve(len / 2 + 16);
    std::vector<size_t> rawAt;  // position brute de chaque symbole (découpe en blocs)
    rawAt.reserve(len / 2 + 16);
    for (size_t i = dictLen; i < end;) {
        size_t d = 0;
        const size_t l = match(i, d);
        insert(i);
        if (l && l < kLazy && i + 1 < end) {  // évaluation paresseuse : le suivant fait-il mieux ?
            size_t d2 = 0;
            if (match(i + 1, d2) > l) {
                rawAt.push_back(i);
                syms.push_back({data[i], 0});
                ++i;
                continue;
            }
        }
        rawAt.push_back(i);
        if (l) {
            syms.push_back({(uint16_t)l, (uint16_t)d});
            for (size_t k = 1; k < l; ++k) insert(i + k);
            i += l;
        } else {
            syms.push_back({data[i], 0});
            ++i;
        }
    }
    constexpr size_t kBlock = 1 << 15;  // symboles par bloc
    for (size_t a = 0; a < syms.size(); a += kBlock) {
        const size_t b = std::min(syms.size(), a + kBlock);
        const size_t r0 = rawAt[a], r1 = b < syms.size() ? rawAt[b] : end;
        const bool last = b == syms.size();
        EmitBlock(bo, syms.data() + a, b - a, data + r0, r1 - r0, final && last);
    }
    if (!final) {  // bloc stocké vide : le morceau finit aligné, le suivant peut s'y coller
        bo.Put(0, 3);
        bo.Align();
        bo.Put(0, 16);
        bo.Put(0xFFFF, 16);
    }
    bo.Align();
}

std::vector<BYTE> Deflate(const BYTE* data, size_t n) {
    std::vector<BYTE> out;
    constexpr size_t kChunk = 1 << 20;
    if (!n) DeflateChunk(data, 0, 0, true, out);
    for (size_t off = 0; off < n; off += kChunk) {
        const size_t len = std::min(kChunk, n - off);
        DeflateChunk(data + off - std::min<size_t>(off, 32768), std::min<size_t>(off, 32768), len, off + len >= n, out);
    }
    return out;
}
