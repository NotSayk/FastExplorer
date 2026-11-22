#include "batchrename.h"

#include <algorithm>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace {

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

wchar_t UpperChar(wchar_t c) { return Upper(std::wstring(1, c))[0]; }

std::wstring ApplyCase(const std::wstring& s, NameCase c) {
    switch (c) {
        case NameCase::Lower: return Lower(s);
        case NameCase::Upper: return Upper(s);
        case NameCase::Sentence: {
            std::wstring r = Lower(s);
            for (auto& ch : r)
                if (IsCharAlphaW(ch)) {
                    ch = UpperChar(ch);
                    break;
                }
            return r;
        }
        case NameCase::Title: {
            std::wstring r = Lower(s);
            bool start = true;
            for (auto& ch : r) {
                if (IsCharAlphaNumericW(ch)) {
                    if (start) ch = UpperChar(ch);
                    start = false;
                } else {
                    start = ch == L' ' || ch == L'_' || ch == L'-' || ch == L'.' || ch == L'(' || ch == L'[';
                }
            }
            return r;
        }
        default: return s;
    }
}

// Remplacement de texte simple, toutes les occurrences.
std::wstring ReplaceText(const std::wstring& s, const std::wstring& find, const std::wstring& by, bool matchCase) {
    if (find.empty()) return s;
    const std::wstring hay = matchCase ? s : Lower(s), needle = matchCase ? find : Lower(find);
    std::wstring out;
    size_t at = 0;
    for (size_t k; (k = hay.find(needle, at)) != std::wstring::npos; at = k + needle.size()) {
        out.append(s, at, k - at);
        out += by;
    }
    out.append(s, at, std::wstring::npos);
    return out;
}

std::wstring DateOf(int64_t ft) {
    if (ft <= 0) return L"";
    FILETIME u{(DWORD)ft, (DWORD)(ft >> 32)};
    SYSTEMTIME su{}, sl{};
    if (!FileTimeToSystemTime(&u, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl)) return L"";
    wchar_t b[16];
    swprintf_s(b, L"%04u-%02u-%02u", sl.wYear, sl.wMonth, sl.wDay);
    return b;
}

std::wstring Counter(long long n, int digits) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n);
    if ((int)s.size() < digits) s.insert(0, digits - s.size(), L'0');
    return n < 0 ? L"-" + s : s;
}

// Modèle : {nom} {ext} {n} {n:3} {date} {dossier} ; « {{ » et « }} » pour des accolades.
std::wstring Expand(const std::wstring& pat, const std::wstring& base, const std::wstring& ext, long long n,
                    const std::wstring& date, const std::wstring& folder) {
    std::wstring out;
    for (size_t i = 0; i < pat.size(); ++i) {
        const wchar_t c = pat[i];
        if ((c == L'{' || c == L'}') && i + 1 < pat.size() && pat[i + 1] == c) {
            out += c;
            ++i;
            continue;
        }
        if (c != L'{') {
            out += c;
            continue;
        }
        const size_t end = pat.find(L'}', i);
        if (end == std::wstring::npos) {
            out.append(pat, i, std::wstring::npos);
            break;
        }
        std::wstring tok = Lower(pat.substr(i + 1, end - i - 1)), arg;
        if (const size_t colon = tok.find(L':'); colon != std::wstring::npos) {
            arg = tok.substr(colon + 1);
            tok.resize(colon);
        }
        if (tok == L"nom" || tok == L"name") out += base;
        else if (tok == L"ext") out += ext;
        else if (tok == L"n") out += Counter(n, arg.empty() ? 0 : std::clamp(_wtoi(arg.c_str()), 0, 12));
        else if (tok == L"date") out += date;
        else if (tok == L"dossier" || tok == L"folder") out += folder;
        else out.append(pat, i, end - i + 1);  // inconnu : laissé tel quel
        i = end;
    }
    return out;
}

}  // namespace

std::vector<std::wstring> BatchNames(const RenameRule& rule, const std::vector<RenameItem>& items,
                                     const std::wstring& folderName, std::wstring* error) {
    std::vector<std::wstring> out;
    out.reserve(items.size());
    if (error) error->clear();
    std::wregex re;
    if (rule.regex && !rule.find.empty()) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!rule.matchCase) flags |= std::regex_constants::icase;
            re = std::wregex(rule.find, flags);
        } catch (const std::regex_error&) {
            if (error) *error = L"Expression régulière invalide";
            for (const auto& it : items) out.push_back(it.name);
            return out;
        }
    }
    long long n = rule.start;
    for (const auto& it : items) {
        std::wstring base = it.name, ext;  // ext : avec le point
        if (!it.dir) {
            const size_t dot = it.name.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0) {
                base = it.name.substr(0, dot);
                ext = it.name.substr(dot);
            }
        }
        std::wstring work = rule.withExtension ? it.name : base;
        if (!rule.find.empty()) {
            if (rule.regex) {
                try {
                    work = std::regex_replace(work, re, rule.replace);
                } catch (const std::regex_error&) {
                    if (error) *error = L"Expression régulière trop complexe";
                }
            } else {
                work = ReplaceText(work, rule.find, rule.replace, rule.matchCase);
            }
        }
        const std::wstring pattern = rule.pattern.empty() ? L"{nom}" : rule.pattern;
        std::wstring name = Expand(pattern, work, ext.empty() ? L"" : ext.substr(1), n, DateOf(it.modified), folderName);
        name = ApplyCase(name, rule.nameCase);
        if (!rule.withExtension) name += ext;
        out.push_back(std::move(name));
        n += rule.step;
    }
    return out;
}

std::wstring NameProblem(const std::wstring& name) {
    if (name.empty()) return L"Nom vide";
    for (wchar_t c : name)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c))
            return std::wstring(L"Caractère interdit : ") + (c < 32 ? std::wstring(L"contrôle") : std::wstring(1, c));
    if (name.back() == L' ' || name.back() == L'.') return L"Se termine par un espace ou un point";
    if (name.size() > 255) return L"Nom trop long";
    std::wstring stem = Lower(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    static const wchar_t* kReserved[] = {L"con",  L"prn",  L"aux",  L"nul",  L"com1", L"com2", L"com3", L"com4",
                                         L"com5", L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3",
                                         L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
    for (const wchar_t* r : kReserved)
        if (stem == r) return L"Nom réservé par Windows";
    return L"";
}

std::vector<std::wstring> BatchProblems(const std::vector<std::wstring>& names, const std::vector<std::wstring>& others) {
    std::vector<std::wstring> out(names.size());
    std::unordered_set<std::wstring> taken;
    for (const auto& o : others) taken.insert(Lower(o));
    std::unordered_map<std::wstring, size_t> seen;
    for (size_t i = 0; i < names.size(); ++i) {
        out[i] = NameProblem(names[i]);
        if (!out[i].empty()) continue;
        const std::wstring k = Lower(names[i]);
        if (taken.count(k)) {
            out[i] = L"Nom déjà pris dans le dossier";
            continue;
        }
        auto [it, fresh] = seen.emplace(k, i);
        if (!fresh) {
            out[i] = L"Même nom qu'un autre élément du lot";
            if (out[it->second].empty()) out[it->second] = out[i];
        }
    }
    return out;
}
