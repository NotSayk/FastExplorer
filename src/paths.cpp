#include "paths.h"

#include <shlwapi.h>

#include <vector>

std::wstring Join(const std::wstring& base, const wchar_t* name) {
    if (base.empty()) return name;
    std::wstring r = base;
    if (r.back() != L'\\') r += L'\\';
    r += name;
    return r;
}

std::wstring ToNtPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    std::wstring r = L"\\\\?\\" + p;
    if (p.size() == 2 && p[1] == L':') r += L'\\';
    return r;
}

bool ParentOf(const std::wstring& p, std::wstring& parent, std::wstring& child) {
    if (p.empty()) return false;
    std::wstring s = p;
    if (s.size() == 3 && s[1] == L':') { parent.clear(); child = s; return true; }
    while (s.size() > 1 && s.back() == L'\\') s.pop_back();
    // « \\serveur » : son parent est Réseau (« ::net », net.h).
    if (s.size() > 2 && s[0] == L'\\' && s[1] == L'\\' && s[2] != L'?' && s[2] != L'.' &&
        s.find(L'\\', 2) == std::wstring::npos) {
        parent = L"::net";
        child = s.substr(2);
        return true;
    }
    const size_t k = s.find_last_of(L'\\');
    if (k == std::wstring::npos) { parent.clear(); child = s; return true; }
    child = s.substr(k + 1);
    parent = s.substr(0, k);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    if (parent == L"\\" || parent == L"\\\\") parent.clear();
    return true;
}

std::wstring NormalizePath(std::wstring s, const std::wstring& base) {
    auto isTrim = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'"'; };
    while (!s.empty() && isTrim(s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && isTrim(s[b])) ++b;
    s.erase(0, b);
    if (s.empty() || _wcsicmp(s.c_str(), L"Ce PC") == 0) return L"";

    std::vector<wchar_t> buf(32768);
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf.data(), (DWORD)buf.size());
    if (n && n <= buf.size()) s = buf.data();
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    if (s.size() == 2 && s[1] == L':') s += L'\\';

    if (PathIsRelativeW(s.c_str())) {
        if (base.empty()) return s;  // pas de dossier courant : on ne devine pas
        s = Join(base, s.c_str());
    }

    const DWORD need = GetFullPathNameW(s.c_str(), 0, nullptr, nullptr);
    if (need) {
        std::wstring full(need, L'\0');
        const DWORD got = GetFullPathNameW(s.c_str(), need, full.data(), nullptr);
        if (got && got < need) { full.resize(got); s = full; }
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring CleanFileName(std::wstring s) {
    size_t b = 0;
    while (b < s.size() && s[b] == L' ') ++b;
    s.erase(0, b);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

const wchar_t* FileNameError(const std::wstring& name) {
    if (name.empty()) return L"Vous devez entrer un nom.";
    for (wchar_t c : name)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c))
            return L"Un nom de fichier ne peut pas contenir les caractères suivants :\n\\ / : * ? \" < > |";
    if (name.size() > 255) return L"Ce nom est trop long (255 caractères au maximum).";
    // Noms de périphériques : refusés par Windows dans tous les dossiers.
    static const wchar_t* const kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL"};
    for (const wchar_t* r : kReserved)
        if (_wcsicmp(name.c_str(), r) == 0) return L"Ce nom est réservé par Windows.";
    if (name.size() == 4 && (_wcsnicmp(name.c_str(), L"COM", 3) == 0 || _wcsnicmp(name.c_str(), L"LPT", 3) == 0) &&
        name[3] >= L'1' && name[3] <= L'9')
        return L"Ce nom est réservé par Windows.";
    return nullptr;
}