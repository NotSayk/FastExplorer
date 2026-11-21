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
    const size_t k = s.find_last_of(L'\\');
    if (k == std::wstring::npos) { parent.clear(); child = s; return true; }
    child = s.substr(k + 1);
    parent = s.substr(0, k);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    // "\\serveur" seul n'est pas énumérable : on remonte à Ce PC.
    if (parent.rfind(L"\\\\", 0) == 0 && parent.find(L'\\', 2) == std::wstring::npos) parent.clear();
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
