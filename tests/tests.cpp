// Tests de FastExplorer (sans dépendance). Lancement : build.bat test
// Compilés sous AddressSanitizer : toute erreur mémoire fait échouer la suite.

#include "win.h"

#include "fileops.h"
#include "icons.h"
#include "jobs.h"
#include "listing.h"
#include "nt.h"
#include "shellmenu.h"
#include "watch.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include "paths.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void Print(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
    fwrite(u.data(), 1, u.size(), stdout);
}

void Check(bool ok, const char* expr, int line) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    printf("ECHEC ligne %d : %s\n", line, expr);
}

void CheckStr(const std::wstring& got, const std::wstring& want, const char* expr, int line, bool icase = false) {
    const bool ok = icase ? _wcsicmp(got.c_str(), want.c_str()) == 0 : got == want;
    if (ok) { ++g_pass; return; }
    ++g_fail;
    printf("ECHEC ligne %d : %s\n", line, expr);
    Print(L"    obtenu  : [" + got + L"]\n    attendu : [" + want + L"]\n");
}

#define CHECK(c) Check((c), #c, __LINE__)
#define CHECK_STR(a, b) CheckStr((a), (b), #a, __LINE__)
#define CHECK_STRI(a, b) CheckStr((a), (b), #a, __LINE__, true)

// ───────────────────────────── Chemins ─────────────────────────────

void TestToNtPath() {
    CHECK_STR(ToNtPath(L"C:\\"), L"\\\\?\\C:\\");
    CHECK_STR(ToNtPath(L"C:"), L"\\\\?\\C:\\");
    CHECK_STR(ToNtPath(L"C:\\Windows"), L"\\\\?\\C:\\Windows");
    CHECK_STR(ToNtPath(L"\\\\srv\\share"), L"\\\\?\\UNC\\srv\\share");
    CHECK_STR(ToNtPath(L"\\\\?\\C:\\x"), L"\\\\?\\C:\\x");
}

void TestJoin() {
    CHECK_STR(Join(L"", L"C:\\"), L"C:\\");
    CHECK_STR(Join(L"C:\\", L"Windows"), L"C:\\Windows");
    CHECK_STR(Join(L"C:\\Windows", L"System32"), L"C:\\Windows\\System32");
}

void TestParentOf() {
    std::wstring p, c;
    CHECK(!ParentOf(L"", p, c));
    CHECK(ParentOf(L"C:\\", p, c)); CHECK_STR(p, L""); CHECK_STR(c, L"C:\\");
    CHECK(ParentOf(L"C:\\a", p, c)); CHECK_STR(p, L"C:\\"); CHECK_STR(c, L"a");
    CHECK(ParentOf(L"C:\\a\\b", p, c)); CHECK_STR(p, L"C:\\a"); CHECK_STR(c, L"b");
    CHECK(ParentOf(L"C:\\a\\", p, c)); CHECK_STR(p, L"C:\\"); CHECK_STR(c, L"a");
    CHECK(ParentOf(L"\\\\srv\\share", p, c)); CHECK_STR(p, L""); CHECK_STR(c, L"share");
    CHECK(ParentOf(L"\\\\srv\\share\\x", p, c)); CHECK_STR(p, L"\\\\srv\\share"); CHECK_STR(c, L"x");
}

void TestNormalizePath() {
    CHECK_STR(NormalizePath(L"", L"C:\\Windows"), L"");
    CHECK_STR(NormalizePath(L"   ", L""), L"");
    CHECK_STR(NormalizePath(L"Ce PC", L"C:\\Windows"), L"");
    CHECK_STR(NormalizePath(L"  \"C:\\Windows\\\"  ", L""), L"C:\\Windows");
    CHECK_STRI(NormalizePath(L"c:/windows/system32/..", L""), L"c:\\windows");
    CHECK_STR(NormalizePath(L"C:", L""), L"C:\\");
    CHECK_STR(NormalizePath(L"C:\\", L""), L"C:\\");
    CHECK_STR(NormalizePath(L"System32", L"C:\\Windows"), L"C:\\Windows\\System32");
    CHECK_STR(NormalizePath(L"..", L"C:\\Windows"), L"C:\\");
    CHECK_STR(NormalizePath(L"foo", L""), L"foo");  // relatif sans base : intouché
    CHECK_STR(NormalizePath(L"\\\\srv\\share\\", L""), L"\\\\srv\\share");
    wchar_t sr[MAX_PATH];
    GetEnvironmentVariableW(L"SystemRoot", sr, MAX_PATH);
    CHECK_STRI(NormalizePath(L"%SystemRoot%", L""), sr);
}

// ───────────────────────────── Lecture et tri ─────────────────────────────

std::wstring g_root;

void MakeFile(const std::wstring& path, DWORD bytes) {
    HANDLE h = CreateFileW(ToNtPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { printf("impossible de créer un fichier de test\n"); return; }
    std::vector<char> buf(bytes, 'x');
    DWORD w;
    if (bytes) WriteFile(h, buf.data(), bytes, &w, nullptr);
    CloseHandle(h);
}

void RemoveTree(const std::wstring& path) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(ToNtPath(Join(path, L"*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring child = Join(path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(child);
            else DeleteFileW(ToNtPath(child).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(ToNtPath(path).c_str());
}

std::vector<std::wstring> Names(const DirData& d, const std::vector<uint32_t>& order) {
    std::vector<std::wstring> v;
    for (uint32_t i : order) v.emplace_back(d.name(d.entries[i]));
    return v;
}

int IndexOf(const std::vector<std::wstring>& v, const wchar_t* s) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == s) return (int)i;
    return -1;
}

void SetupFixture() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = NormalizePath(std::wstring(tmp) + L"fe_tests_" + std::to_wstring(GetCurrentProcessId()), L"");
    RemoveTree(g_root);
    CreateDirectoryW(ToNtPath(g_root).c_str(), nullptr);
    MakeFile(Join(g_root, L"a2.txt"), 10);
    MakeFile(Join(g_root, L"a10.txt"), 300);
    MakeFile(Join(g_root, L"B.txt"), 5);
    MakeFile(Join(g_root, L"été.md"), 0);
    MakeFile(Join(g_root, L"noext"), 1);
    MakeFile(Join(g_root, L".hidden"), 2);
    CreateDirectoryW(ToNtPath(Join(g_root, L"zdir")).c_str(), nullptr);
    CreateDirectoryW(ToNtPath(Join(g_root, L"Adir")).c_str(), nullptr);
}

void TestEnumerate() {
    DirData d;
    d.path = g_root;
    Enumerate(d);
    CHECK(d.error == 0);
    CHECK(d.entries.size() == 8);
    CHECK(d.nDirs == 2);
    CHECK(d.nFiles == 6);
    CHECK(d.keys.size() > 0);
    for (const Entry& e : d.entries) {
        const std::wstring n = d.name(e);
        CHECK(e.nameLen == n.size());
        CHECK(e.keyLen > 0);
        if (n == L"a10.txt") { CHECK(e.size == 300); CHECK(e.extOff == 3); CHECK(e.mtime > 0); }
        if (n == L"noext") CHECK(e.extOff == e.nameLen);
        if (n == L"zdir") { CHECK(e.isDir()); CHECK(e.size == 0); }
    }
}

void TestSortName() {
    DirData d;
    d.path = g_root;
    Enumerate(d);
    auto v = Names(d, SortOrder(d, COL_NAME, true));
    CHECK(v.size() == 8);
    if (v.size() != 8) return;
    CHECK_STR(v[0], L"Adir");  // dossiers d'abord, insensible à la casse
    CHECK_STR(v[1], L"zdir");
    // ordre naturel (2 < 10), insensible à la casse, linguistique (é ~ e)
    CHECK(IndexOf(v, L"a2.txt") < IndexOf(v, L"a10.txt"));
    CHECK(IndexOf(v, L"a10.txt") < IndexOf(v, L"B.txt"));
    CHECK(IndexOf(v, L"B.txt") < IndexOf(v, L"été.md"));
    CHECK(IndexOf(v, L"été.md") < IndexOf(v, L"noext"));

    auto r = Names(d, SortOrder(d, COL_NAME, false));
    CHECK_STR(r[0], L"zdir");  // décroissant : dossiers toujours en premier
    CHECK_STR(r[1], L"Adir");
    CHECK(IndexOf(r, L"noext") < IndexOf(r, L"a2.txt"));
}

void TestSortOther() {
    DirData d;
    d.path = g_root;
    Enumerate(d);
    auto s = Names(d, SortOrder(d, COL_SIZE, false));
    CHECK_STR(s[2], L"a10.txt");  // plus gros fichier juste après les 2 dossiers
    CHECK_STR(s.back(), L"été.md");
    auto t = Names(d, SortOrder(d, COL_TYPE, true));
    CHECK_STR(t[2], L"noext");  // extension vide en premier
}

void TestEdgeCases() {
    DirData empty;
    empty.path = Join(g_root, L"zdir");
    Enumerate(empty);
    CHECK(empty.error == 0);
    CHECK(empty.entries.empty());
    CHECK(SortOrder(empty, COL_NAME, true).empty());

    DirData missing;
    missing.path = Join(g_root, L"nexiste_pas");
    Enumerate(missing);
    CHECK(missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND);

    std::atomic<uint64_t> gen{5};
    DirData cancelled;
    cancelled.path = g_root;
    Enumerate(cancelled, CancelToken{&gen, 4});
    CHECK(cancelled.error == ERROR_CANCELLED);

    DirData drives;  // Ce PC
    Enumerate(drives);
    CHECK(drives.error == 0);
    bool hasC = false;
    for (const Entry& e : drives.entries) hasC |= _wcsicmp(drives.name(e), L"C:\\") == 0;
    CHECK(hasC);
}

void TestLongPath() {
    std::wstring p = g_root;
    for (int i = 0; i < 6; ++i) {
        p = Join(p, std::wstring(50, (wchar_t)(L'a' + i)).c_str());
        CreateDirectoryW(ToNtPath(p).c_str(), nullptr);
    }
    CHECK(p.size() > MAX_PATH);
    MakeFile(Join(p, L"profond.txt"), 3);
    DirData d;
    d.path = p;
    Enumerate(d);
    CHECK(d.error == 0);
    CHECK(d.entries.size() == 1);
    std::wstring parent, child;
    CHECK(ParentOf(p, parent, child));
    CHECK(child == std::wstring(50, L'f'));
}

// Le tri parallèle doit produire exactement le même ordre que le séquentiel.
void TestParallelMatchesSequential() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    for (int col = 0; col < COL_COUNT; ++col) {
        for (bool asc : {true, false}) {
            SetParallel(Parallel::Never);
            DirData a;
            a.path = sys;
            Enumerate(a);
            auto oa = SortOrder(a, col, asc);
            SetParallel(Parallel::Always);
            DirData b;
            b.path = sys;
            Enumerate(b);
            auto ob = SortOrder(b, col, asc);
            CHECK(a.keys == b.keys);
            CHECK(Names(a, oa) == Names(b, ob));
        }
    }
    SetParallel(Parallel::Auto);
}

// Un partage réseau muet (adresse non routable) doit pouvoir être abandonné
// rapidement : c'est ce que fait Jobs_NewGeneration via CancelSynchronousIo.
void TestCancelHungNetworkPath() {
    std::atomic<bool> done{false};
    double finishedAt = 0;
    DWORD err = 0;
    CancelToken ct = Jobs_NewGeneration();
    const double t0 = NowMs();
    Jobs_Spawn([&] {
        DirData d;
        d.path = L"\\\\10.255.255.1\\partage";
        Enumerate(d, ct);
        err = d.error;
        finishedAt = NowMs();
        done = true;
    });
    Sleep(300);  // le thread est bloqué dans CreateFileW
    const bool stillBlocked = !done;
    const double tCancel = NowMs();
    Jobs_NewGeneration();  // périme + CancelSynchronousIo
    while (!done && NowMs() - tCancel < 15000) Sleep(5);
    CHECK(done.load());
    if (!stillBlocked) {
        printf("  (note : le chemin réseau a échoué tout seul en %.0f ms, annulation non exercée)\n", finishedAt - t0);
        return;
    }
    const double latency = finishedAt - tCancel;
    printf("  annulation d'un partage reseau muet : %.1f ms apres la demande\n", latency);
    CHECK(latency < 1000);
    CHECK(err == ERROR_CANCELLED);
}

// ───────────────────────────── Bloc Shell ─────────────────────────────

bool Own(uint32_t attr, const wchar_t* ext) { return NeedsOwnIcon(attr, ext, (uint32_t)wcslen(ext)); }

void TestNeedsOwnIcon() {
    CHECK(Own(FILE_ATTRIBUTE_ARCHIVE, L".exe"));
    CHECK(Own(FILE_ATTRIBUTE_ARCHIVE, L".LNK"));  // insensible à la casse
    CHECK(Own(FILE_ATTRIBUTE_ARCHIVE, L".ico"));
    CHECK(!Own(FILE_ATTRIBUTE_ARCHIVE, L".txt"));
    CHECK(!Own(FILE_ATTRIBUTE_ARCHIVE, L""));
    CHECK(!Own(FILE_ATTRIBUTE_ARCHIVE, L".exe2"));
    CHECK(!Own(FILE_ATTRIBUTE_DIRECTORY, L""));                              // dossier ordinaire
    CHECK(Own(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY, L""));     // desktop.ini possible
    CHECK(Own(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT, L""));  // jonction, lien
    // Fichier cloud absent du disque : jamais (le lire le téléchargerait).
    CHECK(!Own(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_REPARSE_POINT, L".exe"));
    CHECK(!Own(FILE_ATTRIBUTE_OFFLINE, L".lnk"));
    CHECK(!Own(FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY, L""));
}

void TestShortcut() {
    const std::wstring dir = Join(g_root, L"Adir");
    const std::wstring lnk = Join(dir, L"vers zdir.lnk");
    const std::wstring target = Join(g_root, L"zdir");
    IShellLinkW* sl = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))));
    if (!sl) return;
    sl->SetPath(target.c_str());
    IPersistFile* pf = nullptr;
    sl->QueryInterface(IID_PPV_ARGS(&pf));
    CHECK(pf && SUCCEEDED(pf->Save(lnk.c_str(), TRUE)));
    if (pf) pf->Release();
    sl->Release();

    std::wstring got;
    CHECK(ResolveShortcut(lnk, got));
    CHECK_STRI(got, target);
    std::wstring none;
    CHECK(!ResolveShortcut(Join(dir, L"absent.lnk"), none));
    CHECK(!ResolveShortcut(Join(g_root, L"a2.txt"), none));  // pas un raccourci
}

void TestPrepareMenu() {
    // Menu préparé au bouton enfoncé : construit pour un fichier, un dossier et
    // le fond ; refusé pour un élément absent ; libéré sans fuite (ASan).
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"a2.txt"}, false));
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"zdir"}, true));
    CHECK(ShellMenu_Prepare(nullptr, g_root, {}, false));
    CHECK(!ShellMenu_Prepare(nullptr, g_root, {L"absent.txt"}, false));
    ShellMenu_Discard();
    ShellMenu_Discard();  // deux fois : sans effet
}

void TestEnumerateFileIsNotDirectory() {
    DirData d;
    d.path = Join(g_root, L"a2.txt");
    Enumerate(d);
    CHECK(d.error == ERROR_DIRECTORY);  // permet d'ouvrir le fichier au lieu d'afficher une erreur
    CHECK(d.entries.empty());
}

void TestWatch() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FeTestSink";
    RegisterClassW(&wc);
    HWND sink = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    CHECK(sink != nullptr);
    if (!sink) return;
    constexpr UINT kMsg = WM_APP + 9;
    const std::wstring dir = Join(g_root, L"Adir");

    auto waitFor = [&](uintptr_t id, double ms) {
        const double t0 = NowMs();
        MSG m;
        while (NowMs() - t0 < ms) {
            while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE))
                if (m.wParam == id) return NowMs() - t0;
            Sleep(2);
        }
        return -1.0;
    };

    Watch_Start(sink, kMsg, dir, 41);
    Sleep(150);  // le thread doit avoir ouvert la surveillance
    MakeFile(Join(dir, L"nouveau.txt"), 1);
    const double latency = waitFor(41, 3000);
    CHECK(latency >= 0);
    printf("  surveillance : changement signale en %.1f ms\n", latency);

    Watch_Stop();
    Sleep(100);
    MSG m;
    while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) {}
    MakeFile(Join(dir, L"apres_arret.txt"), 1);
    CHECK(waitFor(41, 500) < 0);  // arrêtée : plus aucun message

    Watch_Start(sink, kMsg, Join(g_root, L"nexiste_pas"), 42);  // dossier absent : pas de plantage
    Sleep(100);
    Watch_Stop();
    DestroyWindow(sink);
}

void TestFileNames() {
    CHECK_STR(CleanFileName(L"  rapport.txt  "), L"rapport.txt");
    CHECK_STR(CleanFileName(L"note..."), L"note");
    CHECK_STR(CleanFileName(L"a. . "), L"a");
    CHECK_STR(CleanFileName(L"   "), L"");
    CHECK(FileNameError(L"rapport final.docx") == nullptr);
    CHECK(FileNameError(L".gitignore") == nullptr);
    CHECK(FileNameError(L"CON.txt") == nullptr);  // accepté par Windows 11 ; seul le nom nu est refusé
    CHECK(FileNameError(L"") != nullptr);
    CHECK(FileNameError(L"a/b") != nullptr);
    CHECK(FileNameError(L"quoi?") != nullptr);
    CHECK(FileNameError(L"x\ty") != nullptr);
    CHECK(FileNameError(L"con") != nullptr);
    CHECK(FileNameError(L"LPT1") != nullptr);
    CHECK(FileNameError(L"COM0") == nullptr);
    CHECK(FileNameError(std::wstring(256, L'a')) != nullptr);
    CHECK(FileNameError(std::wstring(255, L'a')) == nullptr);
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void TestFileOps() {
    HWND sink = CreateWindowW(L"FeTestSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(sink != nullptr);
    if (!sink) return;
    constexpr UINT kMsg = WM_APP + 10;
    const std::wstring ops = Join(g_root, L"ops");
    CreateDirectoryW(ops.c_str(), nullptr);

    auto run = [&](FileOp op) -> std::unique_ptr<FileOpResult> {
        op.silent = true;  // aucune fenêtre pendant les tests
        FileOps_Start(nullptr, sink, kMsg, std::move(op));
        const double t0 = NowMs();
        MSG m;
        while (NowMs() - t0 < 15000) {
            if (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) return std::unique_ptr<FileOpResult>((FileOpResult*)m.lParam);
            Sleep(2);
        }
        return nullptr;
    };
    auto created = [](const std::unique_ptr<FileOpResult>& r) {
        return r && r->created.size() == 1 ? r->created[0] : std::wstring(L"<aucun>");
    };

    FileOp nf;
    nf.kind = FileOpKind::NewFolder;
    nf.folder = ops;
    nf.newName = L"Nouveau dossier";
    auto r = run(FileOp{nf.kind, nf.folder, {}, {}, nf.newName});
    CHECK(r && SUCCEEDED(r->hr));
    CHECK_STR(created(r), L"Nouveau dossier");
    r = run(FileOp{nf.kind, nf.folder, {}, {}, nf.newName});
    CHECK_STR(created(r), L"Nouveau dossier (2)");  // numérotation comme l'Explorateur
    CHECK(Exists(Join(ops, L"Nouveau dossier (2)")));

    FileOp rn;
    rn.kind = FileOpKind::Rename;
    rn.folder = ops;
    rn.paths = {Join(ops, L"Nouveau dossier (2)")};
    rn.newName = L"renommé";
    r = run(std::move(rn));
    CHECK(r && SUCCEEDED(r->hr));
    CHECK_STR(created(r), L"renommé");
    CHECK(Exists(Join(ops, L"renommé")) && !Exists(Join(ops, L"Nouveau dossier (2)")));

    MakeFile(Join(ops, L"f.txt"), 10);
    FileOp cp;
    cp.kind = FileOpKind::Copy;
    cp.folder = Join(ops, L"renommé");
    cp.paths = {Join(ops, L"f.txt")};
    r = run(std::move(cp));
    CHECK(r && SUCCEEDED(r->hr));
    CHECK_STR(created(r), L"f.txt");
    CHECK(Exists(Join(ops, L"renommé\\f.txt")) && Exists(Join(ops, L"f.txt")));

    FileOp same;  // copie dans son propre dossier : un double, pas un écrasement
    same.kind = FileOpKind::Copy;
    same.folder = ops;
    same.paths = {Join(ops, L"f.txt")};
    r = run(std::move(same));
    CHECK(r && SUCCEEDED(r->hr));
    const std::wstring dup = created(r);
    printf("  copie dans le meme dossier : [%ls]\n", dup.c_str());
    CHECK(dup != L"f.txt" && dup != L"<aucun>" && Exists(Join(ops, dup.c_str())));

    FileOp mv;
    mv.kind = FileOpKind::Move;
    mv.folder = Join(ops, L"Nouveau dossier");
    mv.paths = {Join(ops, L"renommé\\f.txt")};
    r = run(std::move(mv));
    CHECK(r && SUCCEEDED(r->hr));
    CHECK_STR(created(r), L"f.txt");
    CHECK(Exists(Join(ops, L"Nouveau dossier\\f.txt")) && !Exists(Join(ops, L"renommé\\f.txt")));

    FileOp stay;  // déplacer là où il est déjà : rien à faire, pas d'erreur
    stay.kind = FileOpKind::Move;
    stay.folder = ops;
    stay.paths = {Join(ops, L"f.txt")};
    r = run(std::move(stay));
    CHECK(r && SUCCEEDED(r->hr) && r->created.empty());
    CHECK(Exists(Join(ops, L"f.txt")));

    FileOp del;
    del.kind = FileOpKind::Delete;
    del.folder = ops;
    del.permanent = true;
    del.paths = {Join(ops, L"Nouveau dossier"), Join(ops, dup.c_str())};
    del.focusAfter = L"f.txt";
    r = run(std::move(del));
    CHECK(r && SUCCEEDED(r->hr));
    CHECK(r && r->focusAfter == L"f.txt");
    CHECK(!Exists(Join(ops, L"Nouveau dossier")) && !Exists(Join(ops, dup.c_str())));

    FileOp gone;
    gone.kind = FileOpKind::Delete;
    gone.folder = ops;
    gone.permanent = true;
    gone.paths = {Join(ops, L"absent.txt")};
    r = run(std::move(gone));
    CHECK(r && FileOps_IsFailure(*r));
    CHECK(FileOps_Running() == 0);

    // Collage depuis une application qui ne fournit que des chemins (CF_HDROP),
    // et depuis un « Couper » de l'Explorateur (format Shell + effet DÉPLACER).
    {
        const std::wstring src = Join(ops, L"f.txt");
        const size_t bytes = sizeof(DROPFILES) + (src.size() + 2) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        auto* df = (DROPFILES*)GlobalLock(g);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy((char*)df + sizeof(DROPFILES), src.c_str(), src.size() * sizeof(wchar_t));
        GlobalUnlock(g);
        IDataObject* dobj = nullptr;
        CHECK(SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&dobj))));
        if (dobj) {
            FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM sm{};
            sm.tymed = TYMED_HGLOBAL;
            sm.hGlobal = g;
            CHECK(SUCCEEDED(dobj->SetData(&fe, &sm, TRUE)));
            std::vector<Pidl> items;
            bool cut = true;
            CHECK(ItemsFromDataObject(dobj, items, cut));
            CHECK(items.size() == 1 && !cut);
            wchar_t back[MAX_PATH] = {};
            CHECK(!items.empty() && SHGetPathFromIDListW(items[0].get(), back) && _wcsicmp(back, src.c_str()) == 0);
            dobj->Release();
        }
        IDataObject* shellObj = nullptr;
        CHECK(ShellObjectOf(nullptr, ops, {L"f.txt"}, IID_IDataObject, (void**)&shellObj));
        if (shellObj) {
            HGLOBAL e = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            *(DWORD*)GlobalLock(e) = DROPEFFECT_MOVE;
            GlobalUnlock(e);
            FORMATETC fe{(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL};
            STGMEDIUM sm{};
            sm.tymed = TYMED_HGLOBAL;
            sm.hGlobal = e;
            shellObj->SetData(&fe, &sm, TRUE);
            std::vector<Pidl> items;
            bool cut = false;
            CHECK(ItemsFromDataObject(shellObj, items, cut));
            CHECK(items.size() == 1 && cut);
            shellObj->Release();
        }
    }
    DestroyWindow(sink);
}

}  // namespace

int wmain() {
    if (!LoadNt()) { printf("ntdll introuvable\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    TestNeedsOwnIcon();
    TestToNtPath();
    TestJoin();
    TestParentOf();
    TestNormalizePath();
    SetupFixture();
    TestEnumerate();
    TestSortName();
    TestSortOther();
    TestEdgeCases();
    TestLongPath();
    TestParallelMatchesSequential();
    TestCancelHungNetworkPath();
    TestShortcut();
    TestEnumerateFileIsNotDirectory();
    TestPrepareMenu();
    TestFileNames();
    TestWatch();
    TestFileOps();
    RemoveTree(g_root);
    printf("\n%d verifications OK, %d echec(s)\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
