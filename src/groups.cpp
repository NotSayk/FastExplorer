#include "groups.h"

#include "listing.h"

#include <shlwapi.h>

#include <algorithm>
#include <map>

namespace {

constexpr int64_t kDay = 864000000000LL;  // en unités de 100 ns

int64_t ToLocal(int64_t ft) {
    FILETIME u{(DWORD)ft, (DWORD)(ft >> 32)}, l{};
    SYSTEMTIME su{}, sl{};
    if (!FileTimeToSystemTime(&u, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl) ||
        !SystemTimeToFileTime(&sl, &l))
        return ft;
    return (int64_t)(((uint64_t)l.dwHighDateTime << 32) | l.dwLowDateTime);
}

void YearMonth(int64_t local, int& y, int& m) {
    FILETIME f{(DWORD)local, (DWORD)(local >> 32)};
    SYSTEMTIME s{};
    FileTimeToSystemTime(&f, &s);
    y = s.wYear;
    m = s.wMonth;
}

bool Less(const Bucket& a, const Bucket& b) {
    if (a.rank != b.rank) return a.rank < b.rank;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.label.c_str(), -1,
                           b.label.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
}

}  // namespace

Bucket NameBucket(const wchar_t* name) {
    wchar_t c = name ? name[0] : 0;
    if (c >= L'0' && c <= L'9') return {0, L"0 – 9"};
    // Lettre de base (é → E, ç → C).
    wchar_t folded[8] = {};
    if (c && FoldStringW(MAP_COMPOSITE, &c, 1, folded, 8) > 0) c = folded[0];
    c = (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c);
    if (c >= L'A' && c <= L'H') return {1, L"A – H"};
    if (c >= L'I' && c <= L'P') return {2, L"I – P"};
    if (c >= L'Q' && c <= L'Z') return {3, L"Q – Z"};
    return {4, L"Autres"};
}

Bucket DateBucket(int64_t ft, int64_t now) {
    if (ft <= 0) return {9, L"Date inconnue"};
    const int64_t t = ToLocal(ft), n = ToLocal(now);
    const int64_t day = t / kDay, today = n / kDay;
    if (day > today) return {0, L"Dans le futur"};
    if (day == today) return {1, L"Aujourd'hui"};
    if (day == today - 1) return {2, L"Hier"};
    // Jours depuis le 1er janvier 1601 (un lundi) : 0 = lundi.
    const int64_t weekStart = today - today % 7;
    if (day >= weekStart) return {3, L"Plus tôt cette semaine"};
    if (day >= weekStart - 7) return {4, L"La semaine dernière"};
    int ty, tm, ny, nm;
    YearMonth(t, ty, tm);
    YearMonth(n, ny, nm);
    if (ty == ny && tm == nm) return {5, L"Plus tôt ce mois-ci"};
    if ((ty == ny && tm == nm - 1) || (ty == ny - 1 && tm == 12 && nm == 1)) return {6, L"Le mois dernier"};
    if (ty == ny) return {7, L"Plus tôt cette année"};
    return {8, L"Il y a longtemps"};
}

Bucket SizeBucket(uint64_t size, bool dir) {
    constexpr uint64_t K = 1024, M = K * K, G = M * K;
    if (dir) return {0, L"Dossiers"};
    if (size == 0) return {1, L"Vide (0 Ko)"};
    if (size < 16 * K) return {2, L"Minuscule (0 – 16 Ko)"};
    if (size < M) return {3, L"Petit (16 Ko – 1 Mo)"};
    if (size < 128 * M) return {4, L"Moyen (1 – 128 Mo)"};
    if (size < G) return {5, L"Grand (128 Mo – 1 Go)"};
    if (size < 4 * G) return {6, L"Très grand (1 – 4 Go)"};
    return {7, L"Gigantesque (> 4 Go)"};
}

std::vector<std::pair<Bucket, size_t>> BucketsOf(const std::vector<uint32_t>& sorted, const BucketFn& bucketOf) {
    std::map<std::wstring, std::pair<Bucket, size_t>> m;
    for (uint32_t i : sorted) {
        if (IsGroupRow(i)) continue;
        Bucket b = bucketOf(i);
        auto it = m.find(b.label);
        if (it == m.end()) {
            std::wstring key = b.label;
            m.emplace(std::move(key), std::make_pair(std::move(b), (size_t)1));
        }
        else ++it->second.second;
    }
    std::vector<std::pair<Bucket, size_t>> v;
    for (auto& [k, p] : m) v.push_back(std::move(p));
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return Less(a.first, b.first); });
    return v;
}

std::vector<uint32_t> ArrangeOrder(const std::vector<uint32_t>& sorted, const std::vector<ColumnFilter>& filters,
                                   const BucketFn& groupOf, bool reverse, const std::set<std::wstring>& collapsed,
                                   std::vector<GroupInfo>& groups, std::vector<uint32_t>* keptOut) {
    groups.clear();
    std::vector<uint32_t> kept;
    kept.reserve(sorted.size());
    for (uint32_t i : sorted) {
        if (IsGroupRow(i)) continue;
        bool ok = true;
        for (const auto& f : filters)
            if (!f.keep.empty() && !f.keep.count(f.bucketOf(i).label)) {
                ok = false;
                break;
            }
        if (ok) kept.push_back(i);
    }
    if (keptOut) *keptOut = kept;
    if (!groupOf) return kept;

    // Groupes : éléments répartis en gardant l'ordre du tri à l'intérieur de chacun.
    std::vector<Bucket> defs;
    std::map<std::wstring, size_t> at;
    std::vector<std::vector<uint32_t>> members;
    for (uint32_t i : kept) {
        Bucket b = groupOf(i);
        auto it = at.find(b.label);
        if (it == at.end()) {
            it = at.emplace(b.label, defs.size()).first;
            defs.push_back(std::move(b));
            members.emplace_back();
        }
        members[it->second].push_back(i);
    }
    std::vector<size_t> g(defs.size());
    for (size_t k = 0; k < g.size(); ++k) g[k] = k;
    std::sort(g.begin(), g.end(), [&](size_t a, size_t b) { return Less(defs[a], defs[b]); });
    if (reverse) std::reverse(g.begin(), g.end());
    std::vector<uint32_t> out;
    out.reserve(kept.size() + g.size());
    for (size_t k : g) {
        GroupInfo gi;
        gi.label = defs[k].label;
        gi.count = members[k].size();
        gi.collapsed = collapsed.count(gi.label) > 0;
        out.push_back(kGroupRow | (uint32_t)groups.size());
        if (!gi.collapsed) out.insert(out.end(), members[k].begin(), members[k].end());
        groups.push_back(std::move(gi));
    }
    return out;
}

FolderKind DetectFolderKind(const DirData& d) {
    static const wchar_t* const kPics[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"heif", L"tif", L"tiff",
                                           L"raw", L"cr2", L"nef", L"arw", L"dng", L"avif", L"jxl", L"ico", nullptr};
    static const wchar_t* const kMusic[] = {L"mp3", L"flac", L"wav", L"m4a", L"aac", L"ogg", L"oga", L"wma", L"opus", L"aiff", L"alac", L"ape", nullptr};
    static const wchar_t* const kVideos[] = {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"m4v", L"mpg", L"mpeg", L"flv", L"ts", L"m2ts", L"3gp", nullptr};
    static const wchar_t* const kDocs[] = {L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"pdf", L"txt", L"odt", L"ods", L"odp", L"rtf", L"md", L"csv", L"epub", nullptr};
    static const wchar_t* const* const kSets[] = {kDocs, kPics, kMusic, kVideos};
    size_t files = 0, count[4] = {};
    for (const Entry& e : d.entries) {
        if (e.isDir()) continue;
        if (e.attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;  // desktop.ini, Thumbs.db…
        ++files;
        if (e.extOff + 1 >= e.nameLen || e.nameLen - e.extOff > 6) continue;
        const wchar_t* ext = d.name(e) + e.extOff + 1;
        for (int k = 0; k < 4; ++k)
            for (const wchar_t* const* s = kSets[k]; *s; ++s)
                if (_wcsicmp(ext, *s) == 0) {
                    ++count[k];
                    k = 4;
                    break;
                }
    }
    if (!files) return FK_GENERAL;
    int best = 0;
    for (int k = 1; k < 4; ++k)
        if (count[k] > count[best]) best = k;
    if (count[best] * 2 <= files) return FK_GENERAL;
    static const FolderKind kKinds[] = {FK_DOCUMENTS, FK_PICTURES, FK_MUSIC, FK_VIDEOS};
    return kKinds[best];
}

int FolderKindFromName(const std::wstring& t) {
    auto has = [&](const wchar_t* s) { return StrStrIW(t.c_str(), s) != nullptr; };
    if (t.empty()) return -1;
    if (has(L"Music")) return FK_MUSIC;
    if (has(L"Picture") || has(L"Photo")) return FK_PICTURES;
    if (has(L"Video")) return FK_VIDEOS;
    if (has(L"Document")) return FK_DOCUMENTS;
    if (has(L"Generic")) return FK_GENERAL;
    return -1;
}