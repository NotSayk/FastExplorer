// FastExplorer — point d'entrée.
//
//   FastExplorer.exe [dossier]
//   FastExplorer.exe --bench <dossier> [--seq|--par]   (mesures, sortie console)
//
// Architecture :
//   nt.*       accès à ntdll (NtQueryDirectoryFile)
//   paths.*    manipulation de chemins (pure, testée)
//   listing.*  lecture d'un dossier, clés de tri, tri (sans UI, testé)
//   jobs.*     tâches de fond avec annulation et interruption d'E/S
//   shellmenu.* menus contextuels et verbes du Shell, raccourcis .lnk
//   icons.*    icônes propres aux fichiers (exe, lnk…), thread dédié
//   watch.*    surveillance du dossier affiché
//   fileops.*  opérations sur les fichiers, presse-papiers
//   fastcopy.* copie rapide (CopyFileExW en parallèle), le Shell en secours
//   dragdrop.* glisser-déposer (source et cible OLE)
//   navpane.*  volet de navigation : Accès rapide, arborescence de Ce PC
//   ui.*       fenêtre, ListView virtuelle, raccourcis

#include "win.h"

#include "listing.h"
#include "nt.h"
#include "paths.h"
#include "ui.h"
#include "util.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <string>

namespace {

void Out(const std::wstring& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!h || h == INVALID_HANDLE_VALUE) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
}

int RunBench(const std::wstring& path) {
    constexpr int kRuns = 7;
    double bEnum = 1e30, bKeys = 1e30, bSort = 1e30, bTotal = 1e30;
    size_t count = 0, dirs = 0, files = 0;
    for (int r = 0; r < kRuns; ++r) {
        DirData d;
        d.path = path;
        const double t0 = NowMs();
        Enumerate(d);
        if (d.error) { Out(L"Erreur : " + ErrorText(d.error) + L"\n"); return 1; }
        const double t1 = NowMs();
        auto order = SortOrder(d, COL_NAME, true);
        const double t2 = NowMs();
        bEnum = std::min(bEnum, d.enumMs);
        bKeys = std::min(bKeys, d.keyMs);
        bSort = std::min(bSort, t2 - t1);
        bTotal = std::min(bTotal, t2 - t0);
        count = d.entries.size();
        dirs = d.nDirs;
        files = d.nFiles;
    }
    wchar_t s[512];
    swprintf_s(s, L"%s\n  %zu entrées (%zu dossiers, %zu fichiers) — meilleur de %d\n"
                  L"  lecture %.3f ms | clés %.3f ms | tri %.3f ms | total %.3f ms\n",
               path.empty() ? L"Ce PC" : path.c_str(), count, dirs, files, kRuns, bEnum, bKeys, bSort, bTotal);
    Out(s);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!LoadNt()) {
        MessageBoxW(nullptr, L"ntdll introuvable.", L"FastExplorer", MB_ICONERROR);
        return 1;
    }

    // __wargv (CRT) plutôt que CommandLineToArgvW : évite de charger shell32 dès le lancement.
    const int argc = __argc;
    wchar_t** argv = __wargv;
    std::wstring start;
    if (argc >= 3 && _wcsicmp(argv[1], L"--bench") == 0) {
        if (argc >= 4 && _wcsicmp(argv[3], L"--seq") == 0) SetParallel(Parallel::Never);
        if (argc >= 4 && _wcsicmp(argv[3], L"--par") == 0) SetParallel(Parallel::Always);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return RunBench(NormalizePath(argv[2], L""));
    }
    if (argc >= 2) {
        wchar_t cwd[MAX_PATH];
        const DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
        start = NormalizePath(argv[1], (n && n < MAX_PATH) ? std::wstring(cwd) : L"");
    } else {
        wchar_t prof[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
        start = (n && n < MAX_PATH) ? std::wstring(prof) : L"";
    }

    // OLE (et pas seulement COM) : presse-papiers des verbes Copier/Couper du
    // menu Shell, et plus tard glisser-déposer.
    OleInitialize(nullptr);
    int rc = 1;
    if (Ui_Create(inst, show, start)) rc = Ui_Run();
    OleUninitialize();
    return rc;
}
