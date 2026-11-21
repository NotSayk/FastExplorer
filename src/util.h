#pragma once
#include "win.h"

#include <cstdint>
#include <string>

// Horloge haute résolution, en millisecondes.
inline double NowMs() {
    static const double kScale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * kScale;
}

// Temps écoulé depuis la création du processus (inclut le chargement par l'OS).
inline double MsSinceProcessStart() {
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    auto q = [](FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    return (double)(q(now) - q(c)) / 10000.0;
}

inline std::wstring ErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring r = msg ? msg : L"Erreur inconnue";
    if (msg) LocalFree(msg);
    while (!r.empty() && (r.back() == L'\n' || r.back() == L'\r' || r.back() == L' ')) r.pop_back();
    return r;
}

template <class T>
inline int Cmp3(T a, T b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}
