// Tests de FastExplorer (sans dépendance). Lancement : build.bat test
// Compilés sous AddressSanitizer : toute erreur mémoire fait échouer la suite.

#include "win.h"

#include "columns.h"
#include "archive.h"
#include "deflate.h"
#include "dragdrop.h"
#include "fastcopy.h"
#include "fileops.h"
#include "foldersize.h"
#include "icons.h"
#include "index.h"
#include "jobs.h"
#include "listing.h"
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include "crumbs.h"
#include "navpane.h"
#include "panes.h"
#include "pins.h"
#include "places.h"
#include "sessions.h"
#include "groups.h"
#include "batchrename.h"
#include "wsearch.h"
#include <unordered_set>
#include <thread>
#include "net.h"
#include <lm.h>
#include "nt.h"
#include "shellmenu.h"
#include "watch.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include "paths.h"
#include "util.h"

#include <cstdio>
#include <limits>
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
    CHECK(ParentOf(L"\\\\srv\\share", p, c)); CHECK_STR(p, L"\\\\srv"); CHECK_STR(c, L"share");
    CHECK(ParentOf(L"\\\\srv", p, c)); CHECK_STR(p, L"::net"); CHECK_STR(c, L"srv");
    CHECK(ParentOf(L"\\\\srv\\", p, c)); CHECK_STR(p, L"::net"); CHECK_STR(c, L"srv");
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

    // Filtre instantané : sans casse ni accents, motifs, plusieurs mots, ordre gardé.
    auto f = Names(d, SortOrder(d, COL_NAME, true, {}, L"A1"));
    CHECK(f.size() == 1 && f[0] == L"a10.txt");
    f = Names(d, SortOrder(d, COL_NAME, true, {}, L"ETE"));
    CHECK(f.size() == 1 && f[0] == L"été.md");
    f = Names(d, SortOrder(d, COL_NAME, true, {}, L"*.txt"));
    CHECK(f.size() == 3 && f[0] == L"a2.txt" && f[2] == L"B.txt");
    f = Names(d, SortOrder(d, COL_NAME, true, {}, L"dir"));
    CHECK(f.size() == 2 && f[0] == L"Adir");
    CHECK(Names(d, SortOrder(d, COL_NAME, true, {}, L"a txt")).size() == 2);
    CHECK(SortOrder(d, COL_NAME, true, {}, L"   ").size() == 8);  // espaces seuls : aucun filtre
    CHECK(SortOrder(d, COL_NAME, true, {}, L"introuvable").empty());
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

void TestFolderSize() {
    const std::wstring base = Join(g_root, L"tailles");
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(Join(base, L"sous").c_str(), nullptr);
    CreateDirectoryW(Join(base, L"sous\\profond").c_str(), nullptr);
    CreateDirectoryW(Join(base, L"vide").c_str(), nullptr);
    MakeFile(Join(base, L"un.bin"), 100);
    MakeFile(Join(base, L"sous\\deux.bin"), 1000);
    MakeFile(Join(base, L"sous\\profond\\trois.bin"), 5);
    // Jonction vers le dossier lui-même : ni boucle, ni double compte.
    _wsystem((L"cmd /c mklink /J \"" + Join(base, L"sous\\boucle") + L"\" \"" + base + L"\" >nul").c_str());
    FolderSize fs;
    CHECK(FolderSize_Compute(base, fs));
    CHECK(fs.bytes == 1105);
    CHECK(fs.files == 3);
    CHECK(fs.dirs == 3);
    CHECK(fs.complete);
    FolderSize empty;
    CHECK(FolderSize_Compute(Join(base, L"vide"), empty) && empty.bytes == 0 && empty.files == 0);
    // Arrêt demandé : abandon.
    CHECK(!FolderSize_Compute(base, fs, [] { return true; }));
    // Dossier introuvable : incomplet, taille nulle.
    FolderSize none;
    CHECK(FolderSize_Compute(Join(base, L"absent"), none) && !none.complete && none.bytes == 0);

    // Tri par taille avec les tailles calculées des dossiers.
    DirData d;
    d.path = base;
    Enumerate(d);
    std::vector<uint64_t> sizes(d.entries.size(), 0);
    for (size_t i = 0; i < d.entries.size(); ++i)
        if (wcscmp(d.name(d.entries[i]), L"sous") == 0) sizes[i] = 1005;
    auto s = Names(d, SortOrder(d, COL_SIZE, false, {}, {}, &sizes));
    CHECK_STR(s[0], L"sous");
    auto a = Names(d, SortOrder(d, COL_SIZE, true, {}, {}, &sizes));
    CHECK_STR(a[0], L"vide");
    RemoveDirectoryW(Join(base, L"sous\\boucle").c_str());
}

// Entrée de journal USN construite à la main.
std::vector<BYTE> UsnRecord(uint64_t frn, uint64_t parent, const std::wstring& name, uint32_t reason, uint32_t attr) {
    std::vector<BYTE> b(sizeof(USN_RECORD_V2) + name.size() * 2, 0);
    auto* r = (USN_RECORD_V2*)b.data();
    r->RecordLength = (DWORD)b.size();
    r->MajorVersion = 2;
    r->FileReferenceNumber = frn;
    r->ParentFileReferenceNumber = parent;
    r->Reason = reason;
    r->FileAttributes = attr;
    r->FileNameLength = (WORD)(name.size() * 2);
    r->FileNameOffset = (WORD)offsetof(USN_RECORD_V2, FileName);
    memcpy(r->FileName, name.data(), name.size() * 2);
    return b;
}

void TestVolumeIndex() {
    VolumeIndex v(L'C');
    const uint64_t seq = 3ull << 48;  // numéro de séquence (ignoré pour les parents)
    v.Put(5, 5, L"", 0, FILE_ATTRIBUTE_DIRECTORY);
    v.Put(seq | 100, 5, L"Users", 5, FILE_ATTRIBUTE_DIRECTORY);
    v.Put(seq | 101, 100, L"sam", 3, FILE_ATTRIBUTE_DIRECTORY);
    v.Put(102, seq | 101, L"Docs", 4, FILE_ATTRIBUTE_DIRECTORY);
    v.Put(103, 101, L".cache", 6, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN);
    v.Put(200, 102, L"Rapport.txt", 11, FILE_ATTRIBUTE_ARCHIVE);
    v.Put(201, 103, L"rapport_cache.txt", 17, 0);
    v.Put(202, 999, L"rapport_orphelin.txt", 20, 0);  // parent inconnu : jamais affiché
    v.Put(203, 102, L"Été rapport.md", 14, 0);
    CHECK(v.Count() == 9);
    CHECK_STR(v.PathOf(200), L"C:\\Users\\sam\\Docs\\Rapport.txt");
    CHECK_STR(v.PathOf(5), L"C:\\");

    auto search = [&](const std::wstring& q, uint64_t scope, bool hidden, uint32_t max = 1000) {
        IndexQuery iq;
        iq.query = q;
        iq.showHidden = hidden;
        iq.max = max;
        std::vector<IndexHit> hits;
        bool trunc = false;
        v.Search(NameMatcher(q), iq, scope, hits, trunc);
        std::vector<std::wstring> s;
        for (auto& h : hits) s.push_back(h.path);
        std::sort(s.begin(), s.end());
        if (trunc) s.push_back(L"<tronqué>");
        return s;
    };
    auto all = search(L"rapport", VolumeIndex::kNoScope, true);
    CHECK(all.size() == 3);
    CHECK(IndexOf(all, L"C:\\Users\\sam\\.cache\\rapport_cache.txt") >= 0);
    CHECK(IndexOf(all, L"C:\\Users\\sam\\Docs\\Été rapport.md") >= 0);
    CHECK(search(L"rapport", VolumeIndex::kNoScope, false).size() == 2);  // pas dans le dossier masqué
    auto scoped = search(L"RAPPORT txt", 101, true);
    CHECK(scoped.size() == 2);
    CHECK(IndexOf(scoped, L"Docs\\Rapport.txt") >= 0);  // relatif au dossier de recherche
    CHECK(search(L"ete", VolumeIndex::kNoScope, true).size() == 1);  // sans accents
    CHECK(search(L"*.md", VolumeIndex::kNoScope, true).size() == 1);
    CHECK(search(L"docs", VolumeIndex::kNoScope, true).size() == 1);  // les dossiers aussi
    CHECK(search(L"rapport", 102, true).size() == 2);
    auto one = search(L"rapport", VolumeIndex::kNoScope, true, 1);
    CHECK(one.size() == 2 && one.back() == L"<tronqué>");

    // Journal : renommage + déplacement, ancien nom ignoré, suppression.
    auto old = UsnRecord(200, 102, L"Rapport.txt", USN_REASON_RENAME_OLD_NAME, 0);
    v.Apply(*(USN_RECORD_V2*)old.data());
    CHECK_STR(v.PathOf(200), L"C:\\Users\\sam\\Docs\\Rapport.txt");
    auto ren = UsnRecord(200, 101, L"Bilan.txt", USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, 0);
    v.Apply(*(USN_RECORD_V2*)ren.data());
    CHECK_STR(v.PathOf(200), L"C:\\Users\\sam\\Bilan.txt");
    auto add = UsnRecord(seq | 204, 102, L"nouveau rapport.pdf", USN_REASON_FILE_CREATE, 0);
    v.Apply(*(USN_RECORD_V2*)add.data());
    CHECK(search(L"nouveau", VolumeIndex::kNoScope, true).size() == 1);
    auto del = UsnRecord(seq | 204, 102, L"nouveau rapport.pdf", USN_REASON_FILE_DELETE | USN_REASON_CLOSE, 0);
    v.Apply(*(USN_RECORD_V2*)del.data());
    CHECK(search(L"nouveau", VolumeIndex::kNoScope, true).empty());
    CHECK(v.Count() == 9);
    v.Compact();
    CHECK_STR(v.PathOf(200), L"C:\\Users\\sam\\Bilan.txt");

    // Protocole : aller-retour et demandes malformées.
    IndexQuery q;
    q.query = L"rapport *.txt";
    q.scope = L"C:\\Users";
    q.showHidden = false;
    q.max = 42;
    auto enc = Index_EncodeQuery(q);
    IndexQuery d;
    CHECK(Index_DecodeQuery(enc.data(), enc.size(), d));
    CHECK_STR(d.query, q.query);
    CHECK_STR(d.scope, q.scope);
    CHECK(!d.showHidden && d.showSuperHidden && d.max == 42);
    CHECK(!Index_DecodeQuery(enc.data(), enc.size() - 2, d));  // tronquée
    enc[0] ^= 1;
    CHECK(!Index_DecodeQuery(enc.data(), enc.size(), d));  // mauvaise signature
    IndexQuery empty;
    auto e2 = Index_EncodeQuery(empty);
    CHECK(!Index_DecodeQuery(e2.data(), e2.size(), d));  // recherche vide
    CHECK(!Index_CurrentUserSid().empty());
    if (!Index_ServiceInstalled()) {  // sans service : la recherche passe par le parcours des dossiers
        std::vector<IndexHit> hits;
        bool trunc = false;
        CHECK(Index_Search(q, hits, trunc) == IndexStatus::Unavailable);
    }
}

void TestColumns() {
    // Attributs, comme l'Explorateur.
    CHECK(AttrText(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) == L"RHA");
    CHECK(AttrText(FILE_ATTRIBUTE_DIRECTORY) == L"D");
    CHECK(AttrText(FILE_ATTRIBUTE_NORMAL).empty());

    // Dates de création et d'accès lues avec le dossier.
    DirData d;
    d.path = g_root;
    Enumerate(d);
    bool dates = !d.entries.empty();
    for (const Entry& e : d.entries) dates &= e.ctime > 0 && e.atime > 0;
    CHECK(dates);
    CHECK(SortOrder(d, COL_CTIME, true).size() == SortOrder(d, COL_NAME, true).size());

    // Tri sur une propriété : sans valeur d'abord, nombres par valeur, texte en ordre naturel.
    DirData p;
    for (const wchar_t* n : {L"a", L"b", L"c", L"d"}) DirData_Add(p, n, 1, 0, 1, 0);
    DirData_BuildKeys(p);
    PropValues pv;
    pv.Reset(4);
    pv.text = {L"x10", L"", L"x9", L"x2"};
    auto byText = Names(p, SortOrder(p, COL_PROP, true, {}, {}, nullptr, &pv));
    CHECK((byText == std::vector<std::wstring>{L"b", L"d", L"c", L"a"}));
    pv.text = {L"1 Mo", L"3 Ko", L"", L"2 Mo"};
    pv.num = {1e6, 3e3, std::numeric_limits<double>::quiet_NaN(), 2e6};
    auto byNum = Names(p, SortOrder(p, COL_PROP, false, {}, {}, nullptr, &pv));
    CHECK((byNum == std::vector<std::wstring>{L"d", L"a", L"b", L"c"}));

    // Identifiants de colonnes (mémoire des affichages).
    ColSpec c;
    CHECK(Columns_FromId(L"ctime", c) && c.key == COL_CTIME && Columns_Id(c) == L"ctime");
    const ColSpec author = Columns_Prop(PKEY_Author);
    CHECK(author.key == COL_PROP && !author.title.empty());
    CHECK(Columns_FromId(Columns_Id(author), c) && SameCol(c, author));
    CHECK(Columns_Prop(PKEY_Size).key == COL_SIZE);  // propriété déjà fournie par la lecture du dossier
    CHECK(!Columns_FromId(L"inconnue", c));
}

void TestSearchFilters() {
    // 15 mars 2026, 12 h UTC.
    SYSTEMTIME st{2026, 3, 0, 15, 12, 0, 0, 0};
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    const int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    constexpr int64_t day = 864000000000ll;

    auto s = ParseSearch(L"rapport ext:PDF;.docx type:fichier taille:>10Mo", now);
    CHECK_STR(s.names, L"rapport");
    CHECK(s.exts.size() == 2 && s.exts[0] == L"pdf" && s.exts[1] == L"docx");
    CHECK(s.kind == 2);
    CHECK(s.minSize == 10ull * 1024 * 1024 + 1 && s.maxSize == UINT64_MAX);
    CHECK(s.Accept(L"a\\Rapport.Pdf", 13, 0, 20ull << 20, 0));
    CHECK(!s.Accept(L"a\\Rapport.Pdf", 13, 0, 1ull << 20, 0));    // trop petit
    CHECK(!s.Accept(L"rapport.txt", 11, 0, 20ull << 20, 0));        // autre extension
    CHECK(!s.Accept(L"x.pdf", 5, FILE_ATTRIBUTE_DIRECTORY, 0, 0));  // dossier
    CHECK(!s.Accept(L"dossier.pdf\\fichier", 19, 0, 20ull << 20, 0));  // l'extension est celle du nom

    auto r = ParseSearch(L"size:1,5Go..2Go type:folder", now);
    CHECK(r.names.empty() && r.kind == 1 && r.minSize == 1610612736ull && r.maxSize == 2147483648ull);
    CHECK(ParseSearch(L"taille:<1Ko", now).maxSize == 1023);
    CHECK(ParseSearch(L"taille:10", now).minSize == 10);
    CHECK_STR(ParseSearch(L"taille:beaucoup ext: type:truc", now).names, L"taille:beaucoup ext: type:truc");  // illisibles

    auto w = ParseSearch(L"date:semaine", now);
    CHECK(w.minTime == now - 7 * day && w.maxTime == INT64_MAX);
    auto y = ParseSearch(L"modifié:hier", now);
    CHECK(y.maxTime - y.minTime == day && y.maxTime <= now && now - y.maxTime < day);
    auto t = ParseSearch(L"date:aujourd'hui", now);
    CHECK(t.minTime <= now && now - t.minTime < day);
    auto d1 = ParseSearch(L"date:2026-01-01", now);
    CHECK(d1.maxTime - d1.minTime == day);
    auto rg = ParseSearch(L"date:2026-01-01..2026-01-31", now);
    CHECK(rg.maxTime - rg.minTime == 31 * day);
    auto after = ParseSearch(L"date:>2026-01-01", now), from = ParseSearch(L"date:>=2026-01-01", now);
    CHECK(after.minTime == from.minTime + day);
    CHECK(ParseSearch(L"date:<2026-01-01", now).maxTime == from.minTime);
    CHECK(!ParseSearch(L"date:2026-13-01", now).dated());

    // Filtre instantané et recherche dans les sous-dossiers.
    DirData d;
    d.path = g_root;
    Enumerate(d);
    auto txt = Names(d, SortOrder(d, COL_NAME, true, {}, L"ext:txt"));
    CHECK(txt.size() == 3);
    auto big = Names(d, SortOrder(d, COL_NAME, true, {}, L"a taille:>100"));
    CHECK(big.size() == 1 && big[0] == L"a10.txt");
    auto dirs = Names(d, SortOrder(d, COL_NAME, true, {}, L"type:dossier"));
    CHECK(IndexOf(dirs, L"zdir") >= 0 && IndexOf(dirs, L"Adir") >= 0 && IndexOf(dirs, L"a2.txt") < 0);
    DirData sr;
    sr.path = g_root;
    SearchTree(sr, L"ext:txt taille:<100", {}, {});
    std::vector<std::wstring> found;
    for (const Entry& e : sr.entries) found.emplace_back(sr.name(e));
    CHECK(IndexOf(found, L"a2.txt") >= 0 && IndexOf(found, L"B.txt") >= 0 && IndexOf(found, L"a10.txt") < 0);
    DirData none;
    none.path = g_root;
    SearchTree(none, L"   ", {}, {});
    CHECK(none.error == ERROR_INVALID_PARAMETER);
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

// ── DEFLATE et CRC (deflate.h) ──

void TestDeflate() {
    CHECK(Crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(Crc32(Crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK(Crc32Combine(Crc32(0, "1234", 4), Crc32(0, "56789", 5), 5) == 0xCBF43926u);
    // Flux connu (zlib, niveau 6) : « hello hello hello hello\n » sans en-tête.
    const BYTE known[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0xb9, 0x00};
    std::vector<BYTE> out;
    CHECK(Inflate(known, sizeof known, out));
    CHECK(std::string(out.begin(), out.end()) == "hello hello hello hello\n");
    CHECK(!Inflate(known, 4, out));  // tronqué

    auto roundTrip = [&](const std::vector<BYTE>& in) {
        const std::vector<BYTE> z = Deflate(in.data(), in.size());
        std::vector<BYTE> back;
        return Inflate(z.data(), z.size(), back) && back == in ? z.size() : (size_t)-1;
    };
    CHECK(roundTrip({}) != (size_t)-1);
    CHECK(roundTrip({'a'}) != (size_t)-1);
    std::vector<BYTE> text;
    const char* words[] = {"fichier ", "dossier ", "explorateur ", "rapide ", "Windows ", "\r\n", "copie ", "zip "};
    uint32_t seed = 12345;
    auto rnd = [&] { return seed = seed * 1103515245 + 12345, (seed >> 16) & 0x7FFF; };
    while (text.size() < 3500000) {
        const char* w = words[rnd() % 8];
        text.insert(text.end(), w, w + strlen(w));
    }
    const size_t zt = roundTrip(text);
    CHECK(zt != (size_t)-1 && zt < text.size() / 4);  // plusieurs morceaux, bien compressé
    std::vector<BYTE> noise(300000);
    for (auto& b : noise) b = (BYTE)rnd();
    const size_t zn = roundTrip(noise);
    CHECK(zn != (size_t)-1 && zn <= noise.size() + noise.size() / 1000 + 64);  // incompressible : blocs stockés
    std::vector<BYTE> same(1000000, 'x');
    CHECK(roundTrip(same) < 5000);
    std::vector<BYTE> mixed(text.begin(), text.begin() + 200000);
    mixed.insert(mixed.end(), noise.begin(), noise.end());
    mixed.insert(mixed.end(), same.begin(), same.begin() + 70000);
    CHECK(roundTrip(mixed) != (size_t)-1);
    const double t0 = NowMs();
    const std::vector<BYTE> zz = Deflate(text.data(), text.size());
    const double t1 = NowMs();
    std::vector<BYTE> back;
    Inflate(zz.data(), zz.size(), back);
    printf("  deflate : %.1f Mo en %.0f ms (%.1f %%), inflate %.0f ms\n", text.size() / 1048576.0, t1 - t0,
           100.0 * zz.size() / text.size(), NowMs() - t1);
}

// ── Archives (archive.h) ──

std::string ReadAllBytes(const std::wstring& p) {
    std::string s;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "<absent>";
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) s.append(buf, n);
    CloseHandle(f);
    return s;
}

void WriteAllBytes(const std::wstring& p, const std::string& s) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, s.data(), (DWORD)s.size(), &n, nullptr);
        CloseHandle(f);
    }
}

DWORD RunTar(const std::wstring& args, const std::wstring& dir) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring exe = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

int64_t MtimeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a);
    return (int64_t)(((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime);
}

void TestArchive() {
    const std::wstring base = Join(g_root, L"arc");
    const std::wstring src = Join(base, L"src");
    SHCreateDirectoryExW(nullptr, Join(src, L"Projet\\sous dossier\\vide").c_str(), nullptr);
    std::string big;
    for (int i = 0; big.size() < 2600000; ++i) big += "ligne " + std::to_string(i % 977) + " du fichier de test\r\n";
    WriteAllBytes(Join(src, L"Projet\\gros.txt"), big);
    WriteAllBytes(Join(src, L"Projet\\vide.txt"), "");
    WriteAllBytes(Join(src, L"Projet\\photo.jpg"), std::string(5000, '\x7f'));
    WriteAllBytes(Join(src, L"Projet\\sous dossier\\été ñ.txt"), "accents");
    WriteAllBytes(Join(src, L"seul.txt"), "seul");
    SetFileAttributesW(Join(src, L"seul.txt").c_str(), FILE_ATTRIBUTE_READONLY);

    const std::wstring zip = Join(base, L"test.zip");
    FastProgress prog;
    std::wstring detail;
    const double t0 = NowMs();
    CHECK(Zip_Create(zip, {Join(src, L"Projet"), Join(src, L"seul.txt")}, prog, &detail) == 0);
    printf("  zip : %.1f Mo -> %.1f Ko en %.0f ms\n", prog.bytesTotal / 1048576.0, ReadAllBytes(zip).size() / 1024.0,
           NowMs() - t0);
    CHECK(prog.filesDone == 5 && prog.filesTotal == 5);
    CHECK(Zip_Create(zip, {Join(src, L"seul.txt")}, prog, &detail) == ERROR_FILE_EXISTS);  // jamais par-dessus

    auto idx = Zip_ReadIndex(zip);
    CHECK(idx->error == 0);
    CHECK(idx->entries.size() == 8);  // 3 dossiers + 5 fichiers
    bool sawAccent = false;
    for (const auto& e : idx->entries) sawAccent |= e.name == L"Projet\\sous dossier\\été ñ.txt";
    CHECK(sawAccent);

    // Lu par libarchive (tar.exe de Windows) : notre ZIP est lisible ailleurs.
    const std::wstring viaTar = Join(base, L"viatar");
    CreateDirectoryW(viaTar.c_str(), nullptr);
    CHECK(RunTar(L"-x -f test.zip -C viatar", base) == 0);
    CHECK(ReadAllBytes(Join(viaTar, L"Projet\\gros.txt")) == big);
    CHECK(ReadAllBytes(Join(viaTar, L"Projet\\photo.jpg")) == std::string(5000, '\x7f'));

    // Parcours comme un dossier.
    std::wstring arc, inner;
    CHECK(Archive_Split(Join(zip, L"Projet\\sous dossier"), arc, inner));
    CHECK_STRI(arc, zip);
    CHECK_STR(inner, L"Projet\\sous dossier");
    CHECK(Archive_Split(zip, arc, inner) && inner.empty());
    CHECK(!Archive_Split(src, arc, inner));
    {
        DirData d;
        d.path = zip;
        Archive_Enumerate(d);
        CHECK(d.error == 0 && d.inArchive() && d.entries.size() == 2);
        DirData s;
        s.path = Join(zip, L"Projet");
        Archive_Enumerate(s);
        CHECK(s.entries.size() == 4);  // gros.txt, vide.txt, photo.jpg, sous dossier
        bool dir = false;
        for (const auto& e : s.entries) dir |= e.isDir() && std::wstring(s.name(e)) == L"sous dossier";
        CHECK(dir);
        DirData bad;
        bad.path = Join(zip, L"absent");
        Archive_Enumerate(bad);
        CHECK(bad.error == ERROR_PATH_NOT_FOUND);
    }

    // Extraction complète : contenu, dates, attributs.
    const std::wstring out = Join(base, L"out");
    FastProgress p2;
    std::vector<std::wstring> created;
    CHECK(Archive_Extract(zip, out, {}, p2, &created, &detail) == 0);
    CHECK(created.size() == 2);
    CHECK(ReadAllBytes(Join(out, L"Projet\\gros.txt")) == big);
    CHECK(ReadAllBytes(Join(out, L"Projet\\vide.txt")).empty());
    CHECK(ReadAllBytes(Join(out, L"Projet\\sous dossier\\été ñ.txt")) == "accents");
    CHECK(GetFileAttributesW(Join(out, L"Projet\\sous dossier\\vide").c_str()) & FILE_ATTRIBUTE_DIRECTORY);
    CHECK(MtimeOf(Join(out, L"Projet\\gros.txt")) == MtimeOf(Join(src, L"Projet\\gros.txt")));
    CHECK(GetFileAttributesW(Join(out, L"seul.txt").c_str()) & FILE_ATTRIBUTE_READONLY);
    // Encore : premier niveau renommé, rien n'est écrasé.
    created.clear();
    ExtractOptions sel;
    sel.base = L"Projet";
    sel.items = {L"sous dossier", L"vide.txt"};
    CHECK(Archive_Extract(zip, out, sel, p2, &created, &detail) == 0);
    CHECK(created.size() == 2 && GetFileAttributesW(Join(out, L"sous dossier\\été ñ.txt").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(Archive_Extract(zip, out, sel, p2, nullptr, &detail) == 0);
    CHECK(GetFileAttributesW(Join(out, L"sous dossier (2)").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(Join(out, L"vide (2).txt").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(Archive_SizeOf(zip, sel) == 7);

    // ZIP écrit par libarchive (deflate) : lu par notre moteur.
    CHECK(RunTar(L"-a -c -f fromtar.zip -C src Projet", base) == 0);
    const std::wstring out2 = Join(base, L"out2");
    CHECK(Archive_Extract(Join(base, L"fromtar.zip"), out2, {}, p2, nullptr, &detail) == 0);
    CHECK(ReadAllBytes(Join(out2, L"Projet\\gros.txt")) == big);

    // tar.gz : extrait par tar.exe.
    CHECK(RunTar(L"-c -z -f paquet.tar.gz -C src Projet", base) == 0);
    CHECK(Archive_IsArchive(L"paquet.tar.gz") && !Archive_IsZip(L"paquet.tar.gz"));
    CHECK_STR(Archive_Stem(L"paquet.tar.gz"), L"paquet");
    CHECK_STR(Archive_Stem(L"photos.zip"), L"photos");
    const std::wstring out3 = Join(base, L"out3");
    CHECK(Archive_Extract(Join(base, L"paquet.tar.gz"), out3, {}, p2, nullptr, &detail) == 0);
    CHECK(ReadAllBytes(Join(out3, L"Projet\\gros.txt")) == big);

    // Archive abîmée, et nom dangereux (« ../ ») écarté.
    std::string z = ReadAllBytes(zip);
    WriteAllBytes(Join(base, L"court.zip"), z.substr(0, z.size() / 2));
    CHECK(Zip_ReadIndex(Join(base, L"court.zip"))->error == ERROR_BAD_FORMAT);
    std::string evil = ReadAllBytes(Join(base, L"fromtar.zip"));
    for (size_t at = evil.find("Projet/gros.txt"); at != std::string::npos; at = evil.find("Projet/gros.txt", at + 1))
        memcpy(&evil[at], "../../../g.txt", 14);  // le nom (même longueur) sort du dossier d'extraction
    WriteAllBytes(Join(base, L"evil.zip"), evil);
    auto ev = Zip_ReadIndex(Join(base, L"evil.zip"));
    bool unsafe = false;
    for (const auto& e : ev->entries) unsafe |= e.name.find(L"..") != std::wstring::npos;
    CHECK(ev->error == 0 && !unsafe);
    SetFileAttributesW(Join(src, L"seul.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(Join(out, L"seul.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

// ── Corbeille et Accueil (places.h) ──

void WriteBytes(const std::wstring& path, const std::vector<BYTE>& b) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, b.data(), (DWORD)b.size(), &n, nullptr);
        CloseHandle(f);
    }
}

// Fichier $I : version 2 (longueur + chemin) ou 1 (chemin sur 260 caractères).
std::vector<BYTE> RecycleInfo(int ver, uint64_t size, int64_t when, const std::wstring& orig) {
    std::vector<BYTE> b(24);
    const int64_t v = ver;
    memcpy(b.data(), &v, 8);
    memcpy(b.data() + 8, &size, 8);
    memcpy(b.data() + 16, &when, 8);
    std::wstring p = orig;
    if (ver == 2) {
        const uint32_t n = (uint32_t)p.size() + 1;
        b.resize(28);
        memcpy(b.data() + 24, &n, 4);
        p.push_back(L'\0');
    } else {
        p.resize(260, L'\0');
    }
    const size_t at = b.size();
    b.resize(at + p.size() * 2);
    memcpy(b.data() + at, p.data(), p.size() * 2);
    return b;
}

void SetMtime(const std::wstring& path, int64_t t) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME ft{(DWORD)t, (DWORD)((uint64_t)t >> 32)};
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

void MakeLnk(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

void TestNet() {
    CHECK(Net_IsServerPath(L"\\\\nas"));
    CHECK(!Net_IsServerPath(L"\\\\nas\\films"));
    CHECK(!Net_IsServerPath(L"\\\\?\\C:\\x"));
    CHECK(!Net_IsServerPath(L"C:\\x"));
    CHECK_STR(Net_ServerOf(L"\\\\nas\\films\\a.mkv"), L"\\\\nas");
    CHECK_STR(Net_ServerOf(L"\\\\nas"), L"\\\\nas");
    CHECK_STR(Net_ServerOf(L"C:\\x"), L"");
    CHECK_STR(Net_ServerOf(L"\\\\.\\pipe\\x"), L"");
    CHECK_STR(Places_FromAlias(L"Réseau"), L"::net");
    CHECK_STR(Places_Title(L"::net"), L"Réseau");
    CHECK(Net_IsAuthError(ERROR_ACCESS_DENIED) && Net_IsAuthError(ERROR_LOGON_FAILURE) && !Net_IsAuthError(ERROR_BAD_NETPATH));
    CHECK(Net_ErrorText(NERR_NetNameNotFound).find(L"partage") != std::wstring::npos);

    // Partages de cette machine (service Serveur) : C$ est un partage d'administration, donc caché.
    {
        DirData d;
        d.path = L"\\\\localhost";
        const double t0 = NowMs();
        Net_EnumShares(d);
        CHECK(d.server);
        if (d.error) {
            std::printf("  partages de \\\\localhost : erreur %lu (service Serveur arrêté ?)\n", d.error);
        } else {
            bool c = false;
            for (const Entry& e : d.entries)
                if (_wcsicmp(d.name(e), L"C$") == 0) c = (e.attr & FILE_ATTRIBUTE_HIDDEN) && e.isDir();
            CHECK(c);
            CHECK(d.origin.size() == d.entries.size());
            std::printf("  partages de \\\\localhost : %zu en %.1f ms\n", d.entries.size(), NowMs() - t0);
        }
        DirData bad;
        bad.path = L"\\\\fe-serveur-inexistant-23";
        Net_EnumShares(bad);
        CHECK(bad.error != 0 && bad.entries.empty());
    }

    // Serveurs mémorisés : ajout, casse, retrait.
    {
        const std::wstring srv = L"\\\\FE-Test-Srv-23";
        Net_Forget(srv);
        Net_Remember(srv + L"\\partage\\dossier");
        Net_Remember(L"\\\\fe-test-srv-23\\autre");  // même serveur, autre casse : une seule entrée
        DirData d;
        Net_EnumServers(d);
        CHECK_STR(d.path, L"::net");
        int found = 0;
        for (size_t i = 0; i < d.entries.size(); ++i)
            if (_wcsicmp(d.name(d.entries[i]), L"FE-Test-Srv-23") == 0) {
                ++found;
                CHECK_STR(d.target[i], srv);
                CHECK(d.entries[i].isDir() && d.entries[i].mtime > 0);
            }
        CHECK(found == 1);
        Net_Forget(L"\\\\fe-test-srv-23");
        DirData after;
        Net_EnumServers(after);
        for (const Entry& e : after.entries) CHECK(_wcsicmp(after.name(e), L"FE-Test-Srv-23") != 0);
        std::printf("  Réseau : %zu serveur(s) connu(s)\n", after.entries.size());
    }
}

void TestPlaces() {
    const std::wstring base = Join(g_root, L"places");
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring bin = Join(base, L"bin");
    CreateDirectoryW(bin.c_str(), nullptr);

    // Lecture des $I, versions 1 et 2.
    const std::wstring orig = Join(base, L"restauré\\sous dossier\\note é.txt");
    WriteBytes(Join(bin, L"$IAB12CD.txt"), RecycleInfo(2, 1234, 133000000000000000LL, orig));
    WriteBytes(Join(bin, L"$IOLD001.txt"), RecycleInfo(1, 7, 132000000000000000LL, L"D:\\ancien\\v1.txt"));
    WriteBytes(Join(bin, L"$IBAD001.txt"), {1, 2, 3});
    std::wstring o;
    int64_t when = 0;
    uint64_t size = 0;
    CHECK(Recycle_ReadInfo(Join(bin, L"$IAB12CD.txt"), o, when, size));
    CHECK_STR(o, orig);
    CHECK(when == 133000000000000000LL && size == 1234);
    CHECK(Recycle_ReadInfo(Join(bin, L"$IOLD001.txt"), o, when, size));
    CHECK_STR(o, L"D:\\ancien\\v1.txt");
    CHECK(size == 7);
    CHECK(!Recycle_ReadInfo(Join(bin, L"$IBAD001.txt"), o, when, size));
    CHECK(!Recycle_ReadInfo(Join(bin, L"$Iabsent.txt"), o, when, size));
    CHECK_STR(Recycle_InfoOf(L"C:\\$Recycle.Bin\\S-1\\$RAB12CD.txt"), L"C:\\$Recycle.Bin\\S-1\\$IAB12CD.txt");
    CHECK_STR(Recycle_InfoOf(L"C:\\x\\fichier.txt"), L"");

    // Restaurer : dossiers d'origine recréés, $I effacé ; jamais par-dessus un élément existant.
    const std::wstring r = Join(bin, L"$RAB12CD.txt");
    WriteBytes(r, {'o', 'k'});
    std::wstring to;
    CHECK(Recycle_RestoreItem(r, &to) == 0);
    CHECK_STR(to, orig);
    CHECK(GetFileAttributesW(orig.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(r.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(Join(bin, L"$IAB12CD.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    WriteBytes(Join(bin, L"$IAB12CE.txt"), RecycleInfo(2, 2, 133000000000000001LL, orig));
    WriteBytes(Join(bin, L"$RAB12CE.txt"), {'x'});
    CHECK(Recycle_RestoreItem(Join(bin, L"$RAB12CE.txt")) == ERROR_ALREADY_EXISTS);
    CHECK(GetFileAttributesW(Join(bin, L"$RAB12CE.txt").c_str()) != INVALID_FILE_ATTRIBUTES);  // resté dans la Corbeille

    // Vraie Corbeille : chaque entrée a son élément $R et son emplacement d'origine.
    {
        DirData d;
        Recycle_Enumerate(d);
        CHECK(d.path == kBinPath && d.isVirtual() && !d.isRoot());
        CHECK(d.target.size() == d.entries.size() && d.origin.size() == d.entries.size());
        bool ok = true;
        for (const auto& t : d.target) ok = ok && t.find(L"\\$R") != std::wstring::npos;
        CHECK(ok);
    }

    // Accueil : raccourcis récents, du plus récent au plus ancien ; disparus et dossiers ignorés.
    const std::wstring recent = Join(base, L"recent");
    CreateDirectoryW(recent.c_str(), nullptr);
    const std::wstring docs = Join(base, L"docs");
    CreateDirectoryW(docs.c_str(), nullptr);
    WriteBytes(Join(docs, L"a.txt"), {'a'});
    WriteBytes(Join(docs, L"b.pdf"), {'b', 'b'});
    MakeLnk(Join(recent, L"a.txt.lnk"), Join(docs, L"a.txt"));
    MakeLnk(Join(recent, L"b.pdf.lnk"), Join(docs, L"b.pdf"));
    MakeLnk(Join(recent, L"absent.lnk"), Join(docs, L"absent.txt"));
    MakeLnk(Join(recent, L"docs.lnk"), docs);
    SetMtime(Join(recent, L"a.txt.lnk"), 133000000000000000LL);
    SetMtime(Join(recent, L"b.pdf.lnk"), 133100000000000000LL);  // le plus récent
    SetMtime(Join(recent, L"absent.lnk"), 133200000000000000LL);
    SetMtime(Join(recent, L"docs.lnk"), 133300000000000000LL);
    {
        DirData d;
        Home_EnumerateFrom(d, recent, {}, {}, 40);
        CHECK(d.path == kHomePath && d.isVirtual());
        CHECK(d.entries.size() == 2);
        if (d.entries.size() == 2) {
            CHECK_STR(d.name(d.entries[0]), L"b.pdf");
            CHECK_STRI(d.target[0], Join(docs, L"b.pdf"));
            CHECK_STRI(d.origin[0], docs);
            CHECK(d.entries[0].mtime == 133100000000000000LL && d.entries[0].size == 2);
            CHECK_STR(d.name(d.entries[1]), L"a.txt");
            const auto order = SortOrder(d, COL_DATE, false);
            CHECK(order.size() == 2 && order[0] == 0);
        }
        DirData one;
        Home_EnumerateFrom(one, recent, {}, {}, 1);
        CHECK(one.entries.size() == 1 && one.target.size() == 1);
    }

    // Tri par emplacement d'origine.
    {
        DirData d;
        d.path = kBinPath;
        for (const wchar_t* n : {L"x.txt", L"y.txt", L"z.txt"}) DirData_Add(d, n, (uint32_t)wcslen(n), FILE_ATTRIBUTE_NORMAL, 1, 1);
        d.origin = {L"C:\\b", L"C:\\a", L"C:\\c"};
        d.target = {L"1", L"2", L"3"};
        DirData_BuildKeys(d);
        const auto order = SortOrder(d, COL_ORIGIN, true);
        CHECK(order == (std::vector<uint32_t>{1, 0, 2}));
        CHECK(SortOrder(d, COL_ORIGIN, false) == (std::vector<uint32_t>{2, 0, 1}));
        ColSpec c;
        CHECK(Columns_FromId(L"origin", c) && c.key == COL_ORIGIN);
    }

    {  // durée sur le vrai dossier Recent (information)
        const ULONGLONG t0 = GetTickCount64();
        DirData d;
        Home_Enumerate(d, {});
        const ULONGLONG t1 = GetTickCount64();
        DirData b;
        Recycle_Enumerate(b);
        printf("  Accueil : %zu elements en %llu ms ; Corbeille : %zu elements en %llu ms\n", d.entries.size(), t1 - t0,
               b.entries.size(), GetTickCount64() - t1);
    }
    CHECK_STR(Places_FromAlias(L"Corbeille"), kBinPath);
    CHECK_STR(Places_FromAlias(L"shell:RecycleBinFolder"), kBinPath);
    CHECK_STR(Places_FromAlias(L"accueil"), kHomePath);
    CHECK_STR(Places_FromAlias(L"C:\\Windows"), L"");
    {  // Commandes shell: (dossiers connus)
        std::wstring p;
        CHECK(Places_ShellCommand(L"C:\\Windows", p) == 0);
        PWSTR dl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl))) {
            CHECK(Places_ShellCommand(L"shell:downloads", p) == 1);
            CHECK(_wcsicmp(p.c_str(), dl) == 0);
            CoTaskMemFree(dl);
        }
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        CHECK(Places_ShellCommand(L"shell:Windows", p) == 1 && _wcsicmp(p.c_str(), win) == 0);
        CHECK(Places_ShellCommand(L"shell:System", p) == 1 && _wcsicmp(p.c_str(), (std::wstring(win) + L"\\System32").c_str()) == 0);
        CHECK(Places_ShellCommand(L"shell:sendto", p) == 1 && p.find(L"SendTo") != std::wstring::npos);
        CHECK(Places_ShellCommand(L"shell:pas-un-dossier-connu", p) == -1);
        CHECK(Places_ShellCommand(L"shell:AppsFolder", p) == -2);  // pas de chemin
    }
    {  // Historique des chemins tapés (valeur de l'utilisateur sauvegardée puis remise)
        const std::vector<std::wstring> saved = Places_Typed();
        RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"TypedPaths");
        CHECK(Places_Typed().empty());
        for (int i = 0; i < 30; ++i) Places_RememberTyped(L"C:\\t" + std::to_wstring(i));
        Places_RememberTyped(L"c:\\T5");  // déjà là (autre casse) : remonte en tête, sans doublon
        const auto typed = Places_Typed();
        CHECK(typed.size() == 25);
        CHECK(!typed.empty() && typed[0] == L"c:\\T5");
        CHECK(typed.size() > 1 && typed[1] == L"C:\\t29");
        CHECK(std::count_if(typed.begin(), typed.end(), [](const std::wstring& s) { return _wcsicmp(s.c_str(), L"c:\\t5") == 0; }) == 1);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"TypedPaths");
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) Places_RememberTyped(*it);
        CHECK(Places_Typed() == saved);
    }
    CHECK_STR(Places_Title(kBinPath), L"Corbeille");
    const auto crumbs = Crumbs_Split(kBinPath, {});
    CHECK(crumbs.size() == 1 && crumbs[0].text == L"Corbeille" && crumbs[0].path == kBinPath);
    CHECK(IsVirtualPath(kHomePath) && !IsVirtualPath(L"C:\\") && !IsVirtualPath(L""));
}

int64_t LocalFt(int y, int m, int d, int h = 12) {
    SYSTEMTIME l{(WORD)y, (WORD)m, 0, (WORD)d, (WORD)h, 0, 0, 0}, u{};
    TzSpecificLocalTimeToSystemTime(nullptr, &l, &u);
    FILETIME f{};
    SystemTimeToFileTime(&u, &f);
    return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime);
}

void TestGroups() {
    CHECK_STR(NameBucket(L"42.txt").label, L"0 – 9");
    CHECK_STR(NameBucket(L"abc").label, L"A – H");
    CHECK_STR(NameBucket(L"École").label, L"A – H");
    CHECK_STR(NameBucket(L"Zèbre").label, L"Q – Z");
    CHECK_STR(NameBucket(L"_x").label, L"Autres");
    CHECK(NameBucket(L"I").rank == 2 && NameBucket(L"p").rank == 2);
    CHECK_STR(SizeBucket(0, false).label, L"Vide (0 Ko)");
    CHECK_STR(SizeBucket(10 * 1024, false).label, L"Minuscule (0 – 16 Ko)");
    CHECK_STR(SizeBucket(16 * 1024, false).label, L"Petit (16 Ko – 1 Mo)");
    CHECK_STR(SizeBucket(200ull << 20, false).label, L"Grand (128 Mo – 1 Go)");
    CHECK_STR(SizeBucket(5ull << 30, false).label, L"Gigantesque (> 4 Go)");
    CHECK_STR(SizeBucket(5ull << 30, true).label, L"Dossiers");
    // Mercredi 18 novembre 2026, midi (semaine du lundi 16).
    const int64_t now = LocalFt(2026, 11, 18);
    CHECK_STR(DateBucket(LocalFt(2026, 11, 18, 8), now).label, L"Aujourd'hui");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 17, 23), now).label, L"Hier");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 16, 0), now).label, L"Plus tôt cette semaine");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 15, 22), now).label, L"La semaine dernière");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 9, 1), now).label, L"La semaine dernière");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 8), now).label, L"Plus tôt ce mois-ci");
    CHECK_STR(DateBucket(LocalFt(2026, 10, 20), now).label, L"Le mois dernier");
    CHECK_STR(DateBucket(LocalFt(2026, 3, 1), now).label, L"Plus tôt cette année");
    CHECK_STR(DateBucket(LocalFt(2025, 12, 31), now).label, L"Il y a longtemps");
    CHECK_STR(DateBucket(LocalFt(2026, 11, 19), now).label, L"Dans le futur");
    CHECK_STR(DateBucket(0, now).label, L"Date inconnue");
    CHECK_STR(DateBucket(LocalFt(2025, 12, 20), LocalFt(2026, 1, 5)).label, L"Le mois dernier");  // décembre → janvier

    // Regroupement : éléments 0…5, groupes par parité (rang 0 : pairs), filtre sur « < 4 ».
    const std::vector<uint32_t> sorted = {5, 4, 3, 2, 1, 0};
    BucketFn parity = [](uint32_t i) { return Bucket{(int)(i % 2), i % 2 ? L"impairs" : L"pairs"}; };
    BucketFn low = [](uint32_t i) { return Bucket{0, i < 4 ? L"petit" : L"grand"}; };
    std::vector<GroupInfo> groups;
    std::vector<uint32_t> kept;
    auto o = ArrangeOrder(sorted, {}, parity, false, {}, groups, &kept);
    CHECK((o == std::vector<uint32_t>{kGroupRow | 0, 4, 2, 0, kGroupRow | 1, 5, 3, 1}));
    CHECK(groups.size() == 2 && groups[0].label == L"pairs" && groups[0].count == 3 && kept.size() == 6);
    o = ArrangeOrder(sorted, {}, parity, true, {L"impairs"}, groups);
    CHECK((o == std::vector<uint32_t>{kGroupRow | 0, kGroupRow | 1, 4, 2, 0}));  // inversé, impairs repliés
    CHECK(groups[0].label == L"impairs" && groups[0].collapsed && groups[0].count == 3);
    o = ArrangeOrder(sorted, {{low, {L"petit"}}}, {}, false, {}, groups, &kept);
    CHECK((o == std::vector<uint32_t>{3, 2, 1, 0}) && groups.empty() && kept == o);
    o = ArrangeOrder(sorted, {{low, {L"petit"}}, {parity, {L"impairs"}}}, parity, false, {}, groups);
    CHECK((o == std::vector<uint32_t>{kGroupRow | 0, 3, 1}) && groups.size() == 1);
    o = ArrangeOrder(sorted, {{low, {}}}, {}, false, {}, groups);  // filtre vide : rien d'écarté
    CHECK(o == sorted);
    const auto b = BucketsOf(sorted, parity);
    CHECK(b.size() == 2 && b[0].first.label == L"pairs" && b[0].second == 3 && b[1].second == 3);

    // Type de dossier
    auto kind = [](std::initializer_list<const wchar_t*> names) {
        DirData d;
        d.path = L"C:\\x";
        for (const wchar_t* n : names) DirData_Add(d, n, (uint32_t)wcslen(n), FILE_ATTRIBUTE_ARCHIVE, 1, 1);
        DirData_Add(d, L"desktop.ini", 11, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, 1, 1);
        DirData_Add(d, L"sous", 4, FILE_ATTRIBUTE_DIRECTORY, 0, 1);
        DirData_BuildKeys(d);
        return DetectFolderKind(d);
    };
    CHECK(kind({L"a.mp3", L"b.FLAC", L"c.jpg"}) == FK_MUSIC);
    CHECK(kind({L"a.jpg", L"b.png", L"c.txt", L"d.heic"}) == FK_PICTURES);
    CHECK(kind({L"a.mkv", L"b.txt"}) == FK_GENERAL);  // pas plus de la moitié
    CHECK(kind({L"a.pdf", L"b.docx", L"c"}) == FK_DOCUMENTS);
    CHECK(kind({}) == FK_GENERAL);
    CHECK(FolderKindFromName(L"Music") == FK_MUSIC && FolderKindFromName(L"MusicAlbum") == FK_MUSIC);
    CHECK(FolderKindFromName(L"Pictures") == FK_PICTURES && FolderKindFromName(L"Photos") == FK_PICTURES);
    CHECK(FolderKindFromName(L"Generic") == FK_GENERAL && FolderKindFromName(L"") == -1 && FolderKindFromName(L"Contacts") == -1);
}

void TestSessions() {
    {  // Forme enregistrée : Ce PC (chemin vide) compris
        SessionTabs s;
        s.paths = {L"C:\\Windows", L"", L"\\\\serveur\\partage", L"C:\\a,b"};
        s.active = 2;
        s.other = 0;
        SessionTabs r;
        CHECK(Session_Decode(Session_Encode(s), r));
        CHECK(r.paths == s.paths && r.active == 2 && r.other == 0);
        s.active = 9;  // hors limites : premier onglet ; autre = actif : aucun
        s.other = 0;
        CHECK(Session_Decode(Session_Encode(s), r) && r.active == 0 && r.other == -1);
        CHECK(!Session_Decode(L"", r));
        CHECK(!Session_Decode(std::wstring(L"0,-1\0", 5), r));  // aucun onglet
        CHECK(!Session_Decode(std::wstring(L"0,-1\0C:\\\0", 9), r));  // ligne sans « > »
        CHECK(!Session_Decode(std::wstring(L"x\0>C:\\\0", 8), r));
    }
    {  // Onglets de la dernière fenêtre (valeur de l'utilisateur sauvegardée puis remise)
        SessionTabs saved;
        const bool had = Session_LoadLast(saved);
        SessionTabs s;
        s.paths = {L"C:\\", L""};
        s.active = 1;
        CHECK(Session_SaveLast(s));
        SessionTabs r;
        CHECK(Session_LoadLast(r) && r.paths == s.paths && r.active == 1 && r.other == -1);
        CHECK(!Session_SaveLast(SessionTabs{}));  // rien à mémoriser
        if (had) Session_SaveLast(saved);
        else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\FastExplorer", L"LastTabs");
    }
    {  // Sessions nommées
        const std::wstring name = L"  fe-test session & co  ";
        const std::wstring trimmed = L"fe-test session & co";
        SessionTabs s;
        s.paths = {L"C:\\Windows", L"C:\\"};
        s.other = 1;
        CHECK(Session_Save(name, s));
        auto names = Session_Names();
        CHECK(std::count(names.begin(), names.end(), trimmed) == 1);
        SessionTabs r;
        CHECK(Session_Load(trimmed, r) && r.paths == s.paths && r.other == 1);
        s.paths = {L"D:\\"};
        CHECK(Session_Save(trimmed, s));  // remplacée
        CHECK(Session_Load(name, r) && r.paths.size() == 1 && r.paths[0] == L"D:\\");
        CHECK(!Session_Save(L"   ", s));
        CHECK(!Session_Save(L"vide", SessionTabs{}));
        CHECK(Session_Delete(trimmed));
        names = Session_Names();
        CHECK(std::count(names.begin(), names.end(), trimmed) == 0);
        CHECK(!Session_Load(trimmed, r));
    }
    {  // Signets (le 9 de l'utilisateur sauvegardé puis remis)
        std::wstring saved;
        const bool had = Bookmark_Get(9, saved);
        CHECK(Bookmark_Set(9, L""));  // Ce PC
        std::wstring p = L"x";
        CHECK(Bookmark_Get(9, p) && p.empty());
        CHECK(Bookmark_Set(9, L"C:\\Users"));
        CHECK(Bookmark_Get(9, p) && p == L"C:\\Users");
        CHECK(Bookmark_Clear(9) && !Bookmark_Get(9, p));
        CHECK(Bookmark_Clear(9));  // déjà absent
        CHECK(!Bookmark_Set(0, L"C:\\") && !Bookmark_Set(10, L"C:\\") && !Bookmark_Get(0, p));
        if (had) Bookmark_Set(9, saved);
    }
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
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"a2.txt"}, false, true));
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"zdir"}, true, true));
    CHECK(ShellMenu_Prepare(nullptr, g_root, {}, false, true));
    CHECK(!ShellMenu_Prepare(nullptr, g_root, {L"absent.txt"}, false, true));
    // Survols successifs (asynchrones) : seul le dernier compte, et il est
    // ensuite remis sans être reconstruit.
    ShellMenu_Prepare(nullptr, g_root, {L"a2.txt"}, false);
    ShellMenu_Prepare(nullptr, g_root, {L"zdir"}, false);
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"zdir"}, false, true));
    const double t0 = NowMs();
    CHECK(ShellMenu_Prepare(nullptr, g_root, {L"zdir"}, false, true));
    const double reuse = NowMs() - t0;
    CHECK(reuse < 25);  // marge : sous ASan et machine chargée, ~8 ms
    printf("  menu prepare : remis en %.2f ms\n", reuse);
    ShellMenu_Discard();
    ShellMenu_Discard(true);  // deux fois : sans effet
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

    // Attend le message `kind` pour `id` ; les autres sont traités comme l'interface le ferait.
    auto waitFor = [&](uintptr_t id, LPARAM kind, double ms) {
        const double t0 = NowMs();
        MSG m;
        while (NowMs() - t0 < ms) {
            while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) {
                if (m.wParam != id) continue;
                if (m.lParam == kWatchOpened) Watch_Opened(sink, id);
                if (m.lParam == kind) return NowMs() - t0;
            }
            Sleep(2);
        }
        return -1.0;
    };

    Watch_Start(sink, kMsg, dir, 41);
    CHECK(waitFor(41, kWatchOpened, 3000) >= 0);
    MakeFile(Join(dir, L"nouveau.txt"), 1);
    const double latency = waitFor(41, 0, 3000);
    CHECK(latency >= 0);
    printf("  surveillance : changement signale en %.1f ms\n", latency);

    Watch_Stop();
    Sleep(100);
    MSG m;
    while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) {}
    MakeFile(Join(dir, L"apres_arret.txt"), 1);
    CHECK(waitFor(41, 0, 500) < 0);  // arrêtée : plus aucun message

    Watch_Start(sink, kMsg, Join(g_root, L"nexiste_pas"), 42);  // dossier absent : signalé disparu
    CHECK(waitFor(42, kWatchGone, 3000) >= 0);
    Watch_Stop();

    // Dossier surveillé supprimé par une autre application : signalé disparu.
    const std::wstring tmp = Join(g_root, L"Watched");
    CreateDirectoryW(tmp.c_str(), nullptr);
    Watch_Start(sink, kMsg, tmp, 43);
    CHECK(waitFor(43, kWatchOpened, 3000) >= 0);
    CHECK(RemoveDirectoryW(tmp.c_str()));  // partage DELETE : la surveillance ne bloque pas la suppression
    CHECK(waitFor(43, kWatchGone, 3000) >= 0);
    Watch_Stop();

    // Éjection : Watch_Stop(true) rend le dossier vraiment libre (plus aucun handle ouvert).
    CreateDirectoryW(tmp.c_str(), nullptr);
    Watch_Start(sink, kMsg, tmp, 44);
    CHECK(waitFor(44, kWatchOpened, 3000) >= 0);
    Watch_Stop(true);
    HANDLE ex = CreateFileW(tmp.c_str(), FILE_LIST_DIRECTORY, 0 /* exclusif */, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(ex != INVALID_HANDLE_VALUE);
    if (ex != INVALID_HANDLE_VALUE) CloseHandle(ex);
    RemoveDirectoryW(tmp.c_str());
    while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) {}
    DestroyWindow(sink);
}

void TestEnumerateNearest() {
    CHECK(IsGoneError(ERROR_PATH_NOT_FOUND));
    CHECK(IsGoneError(ERROR_NOT_READY));
    CHECK(!IsGoneError(ERROR_ACCESS_DENIED));  // pas « disparu » : on affiche l'erreur
    DirData d;
    d.path = Join(Join(g_root, L"Adir"), L"supprime\\sous");
    EnumerateNearest(d, CancelToken{});
    CHECK(d.error == 0);
    CHECK_STR(d.path, Join(g_root, L"Adir"));
    CHECK_STR(d.vanished, Join(Join(g_root, L"Adir"), L"supprime\\sous"));
    DirData ok;
    ok.path = g_root;
    EnumerateNearest(ok, CancelToken{});
    CHECK(ok.vanished.empty());
    CHECK_STR(ok.path, g_root);
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

DWORD Attrs(const std::wstring& p) { return GetFileAttributesW(ToNtPath(p).c_str()); }

void TestNavPane() {
    // Sous-dossiers : ordre naturel, fichiers exclus, dossiers protégés du système exclus.
    const std::wstring base = Join(g_root, L"nav");
    CreateDirectoryW(base.c_str(), nullptr);
    for (const wchar_t* n : {L"b10", L"b2", L"A", L"protégé", L"caché"}) CreateDirectoryW(Join(base, n).c_str(), nullptr);
    CreateDirectoryW(Join(Join(base, L"A"), L"enfant").c_str(), nullptr);
    MakeFile(Join(base, L"fichier.txt"), 3);
    SetFileAttributesW(Join(base, L"protégé").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    SetFileAttributesW(Join(base, L"caché").c_str(), FILE_ATTRIBUTE_HIDDEN);
    const auto kids = NavPane_ReadChildren(base);
    std::vector<std::wstring> names;
    for (const auto& k : kids) names.push_back(k.text);
    CHECK(names.size() == 4);
    if (names.size() == 4) {
        CHECK_STR(names[0], L"A");
        CHECK_STR(names[1], L"b2");  // ordre naturel : b2 avant b10
        CHECK_STR(names[2], L"b10");
        CHECK_STR(names[3], L"caché");  // simplement caché : affiché (comme la liste)
        CHECK(kids[0].hasChildren && !kids[1].hasChildren);
        CHECK_STR(kids[0].path, Join(base, L"A"));
    }
    CHECK(NavPane_ReadChildren(Join(base, L"absent")).empty());
    SetFileAttributesW(Join(base, L"protégé").c_str(), FILE_ATTRIBUTE_NORMAL);

    // Ce PC : lecteurs nommés comme dans l'Explorateur.
    const auto drives = NavPane_ReadChildren(L"");
    CHECK(!drives.empty());
    bool sawC = false;
    for (const auto& d : drives) sawC |= d.path == L"C:\\" && d.text.find(L"(C:)") != std::wstring::npos;
    CHECK(sawC);
    CHECK_STR(DriveDisplayName(L"D:\\", DRIVE_FIXED, DriveInfo{L"Données"}), L"Données (D:)");
    CHECK_STR(DriveDisplayName(L"E:\\", DRIVE_REMOVABLE, DriveInfo{}), L"Disque amovible (E:)");
    const auto info = QueryDrives({L"C:\\", L"Q:\\"}, 2000);
    CHECK(info.size() == 2 && info[0].total > 0 && info[0].free <= info[0].total);
    CHECK(info.size() == 2 && info[1].total == 0);  // lecteur absent : inconnu, sans erreur

    RemoveTree(base);
}

void TestPins() {
    // Fichiers d'épingles dans un dossier de test (pas dans %APPDATA%).
    const std::wstring dir = Join(g_root, L"pins");
    CreateDirectoryW(dir.c_str(), nullptr);
    Pins_SetDir(dir);
    const std::wstring a = Join(g_root, L"épinglé A"), b = Join(g_root, L"B"), gone = Join(g_root, L"disparu");
    CreateDirectoryW(a.c_str(), nullptr);
    CreateDirectoryW(b.c_str(), nullptr);

    CHECK(Pins_LoadOwn().empty());
    CHECK(Pins_AddOwn(a) && Pins_AddOwn(b) && Pins_AddOwn(gone));
    CHECK(Pins_AddOwn(a));  // déjà épinglé : pas de doublon
    auto own = Pins_LoadOwn();
    CHECK(own.size() == 3);
    if (own.size() == 3) CHECK_STR(own[0].path, a);  // relu tel quel (UTF-8)
    std::wstring upper = b;
    CharUpperBuffW(upper.data(), (DWORD)upper.size());
    CHECK(Pins_IsOwn(upper));  // chemins comparés sans la casse
    CHECK(Pins_RemoveOwn(upper) && !Pins_IsOwn(b) && Pins_LoadOwn().size() == 2);
    CHECK(Pins_RemoveOwn(b));  // absent : rien à faire, pas une erreur
    CHECK(Pins_AddOwn(b));

    // Accès rapide de Windows injoignable : dernière copie connue + nos épingles.
    CHECK(Pins_SaveSnapshot({{a, L"Mon A"}, {gone, L""}}));
    const auto snap = Pins_LoadSnapshot();
    CHECK(snap.size() == 2 && snap[0].text == L"Mon A" && snap[1].text.empty());
    auto pins = NavPane_ReadPinned(false);
    CHECK(pins.size() == 2);  // `gone` absent du disque : caché (des deux listes)
    if (pins.size() == 2) {
        CHECK_STR(pins[0].text, L"Mon A");
        CHECK(pins[0].pin == NavPin::Own);  // épinglé des deux côtés : retirable chez nous
        CHECK_STR(pins[1].path, b);
        CHECK_STR(pins[1].text, L"B");
        CHECK(pins[1].pin == NavPin::Own && !pins[1].hasChildren);
    }
    CHECK(NavPane_PinOf(gone) == NavPin::Own);  // volet jamais affiché : lu dans le fichier

    // Plus rien d'épinglé nulle part : dossiers par défaut (Bureau, Documents…).
    CHECK(Pins_SaveSnapshot({}) && Pins_RemoveOwn(a) && Pins_RemoveOwn(b) && Pins_RemoveOwn(gone));
    pins = NavPane_ReadPinned(false);
    CHECK(pins.size() >= 3);
    for (const auto& p : pins) CHECK(p.pin == NavPin::None && GetFileAttributesW(p.path.c_str()) != INVALID_FILE_ATTRIBUTES);

    // Accès rapide de Windows : de vrais dossiers (le contenu dépend de la machine),
    // dont la copie est enregistrée pour plus tard.
    pins = NavPane_ReadPinned();
    size_t windows = 0;
    for (const auto& p : pins) {
        CHECK(GetFileAttributesW(p.path.c_str()) != INVALID_FILE_ATTRIBUTES);
        windows += p.pin == NavPin::Windows;
    }
    printf("  acces rapide : %zu dossier(s) epingle(s) dans Windows\n", windows);
    CHECK(Pins_LoadSnapshot().size() == windows);
    Pins_SetDir(L"");
    RemoveTree(dir);
}

void TestCrumbs() {
    auto texts = [](const std::vector<Crumb>& c) {
        std::wstring s;
        for (const auto& x : c) s += x.text + L"|" + x.path + L";";
        return s;
    };
    CHECK_STR(texts(Crumbs_Split(L"")), L"Ce PC|;");
    CHECK_STR(texts(Crumbs_Split(L"C:\\")), L"Ce PC|;C:|C:\\;");
    CHECK_STR(texts(Crumbs_Split(L"c:\\Dev\\Fast Explorer\\")),
              L"Ce PC|;C:|C:\\;Dev|C:\\Dev;Fast Explorer|C:\\Dev\\Fast Explorer;");
    const auto named = Crumbs_Split(L"D:\\a", [](const std::wstring& root) {
        return root == L"D:\\" ? std::wstring(L"Données (D:)") : std::wstring();
    });
    CHECK_STR(texts(named), L"Ce PC|;Données (D:)|D:\\;a|D:\\a;");
    CHECK_STR(texts(Crumbs_Split(L"\\\\srv\\partage\\x\\y")),
              L"Réseau|::net;srv|\\\\srv;partage|\\\\srv\\partage;x|\\\\srv\\partage\\x;y|\\\\srv\\partage\\x\\y;");
    CHECK_STR(texts(Crumbs_Split(L"\\\\srv\\partage")), L"Réseau|::net;srv|\\\\srv;partage|\\\\srv\\partage;");
    CHECK_STR(texts(Crumbs_Split(L"\\\\srv")), L"Réseau|::net;srv|\\\\srv;");
    CHECK_STR(texts(Crumbs_Split(L"::net")), L"Réseau|::net;");
    CHECK_STR(texts(Crumbs_Split(L"C:\\a\\\\b")), L"Ce PC|;C:|C:\\;a|C:\\a;b|C:\\a\\b;");  // séparateur doublé
}

void TestViewPrefs() {
    const DWORD H = FILE_ATTRIBUTE_HIDDEN, S = FILE_ATTRIBUTE_SYSTEM;
    const ViewPrefs all{true, true, true}, noSuper{true, false, true}, none{false, false, true}, odd{false, true, true};
    CHECK(IsShown(0, none) && IsShown(S, none));  // système seul : toujours affiché
    CHECK(IsShown(H, all) && IsShown(H, noSuper) && !IsShown(H, none) && !IsShown(H, odd));
    CHECK(IsShown(H | S, all) && !IsShown(H | S, noSuper) && !IsShown(H | S, none) && !IsShown(H | S, odd));

    // Tri filtré : les masqués sortent de l'ordre, pas des entrées.
    const std::wstring base = Join(g_root, L"vue");
    CreateDirectoryW(base.c_str(), nullptr);
    MakeFile(Join(base, L"normal.txt"), 1);
    MakeFile(Join(base, L"cache.txt"), 1);
    MakeFile(Join(base, L"protege.txt"), 1);
    CreateDirectoryW(Join(base, L"dossier_cache").c_str(), nullptr);
    SetFileAttributesW(Join(base, L"cache.txt").c_str(), H);
    SetFileAttributesW(Join(base, L"protege.txt").c_str(), H | S);
    SetFileAttributesW(Join(base, L"dossier_cache").c_str(), H);
    DirData d;
    d.path = base;
    Enumerate(d);
    auto names = [&](const ViewPrefs& p) {
        std::wstring s;
        for (uint32_t i : SortOrder(d, COL_NAME, true, p)) s += std::wstring(d.name(d.entries[i])) + L";";
        return s;
    };
    CHECK(d.entries.size() == 4);
    CHECK_STR(names(all), L"dossier_cache;cache.txt;normal.txt;protege.txt;");
    CHECK_STR(names(noSuper), L"dossier_cache;cache.txt;normal.txt;");
    CHECK_STR(names(none), L"normal.txt;");
    CHECK_STR(names(ViewPrefs{}), L"dossier_cache;cache.txt;normal.txt;protege.txt;");  // défaut : tout
    for (const wchar_t* n : {L"cache.txt", L"protege.txt", L"dossier_cache"})
        SetFileAttributesW(Join(base, n).c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveTree(base);

    // Lecteurs de Ce PC : jamais filtrés.
    DirData pc;
    Enumerate(pc);
    CHECK(SortOrder(pc, COL_NAME, true, none).size() == pc.entries.size());

    // Extensions masquées : types connus seulement.
    auto hid = [](const wchar_t* n, bool show) {
        const uint32_t len = (uint32_t)wcslen(n);
        const wchar_t* dot = wcsrchr(n, L'.');
        return HiddenExtLen(n, len, dot ? (uint32_t)(dot - n) : len, show);
    };
    CHECK(hid(L"lisez-moi.txt", false) == 4 && hid(L"lisez-moi.txt", true) == 0);
    CHECK(hid(L"Raccourci.lnk", true) == 4);   // NeverShowExt : toujours masquée, comme l'Explorateur
    CHECK(hid(L"x.zzqqtype", false) == 0);     // type inconnu : affichée
    CHECK(hid(L".gitignore", false) == 0);     // nom qui n'est qu'une extension
    CHECK(hid(L"sans_extension", false) == 0);
    CHECK(hid(L"point_final.", false) == 0);

    // Volet : les mêmes règles.
    const std::wstring nav = Join(g_root, L"vue_volet");
    CreateDirectoryW(nav.c_str(), nullptr);
    CreateDirectoryW(Join(nav, L"visible").c_str(), nullptr);
    CreateDirectoryW(Join(nav, L"cache").c_str(), nullptr);
    SetFileAttributesW(Join(nav, L"cache").c_str(), H);
    NavPane_SetViewPrefs(none);
    CHECK(NavPane_ReadChildren(nav).size() == 1);
    NavPane_SetViewPrefs(noSuper);
    CHECK(NavPane_ReadChildren(nav).size() == 2);
    SetFileAttributesW(Join(nav, L"cache").c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveTree(nav);
}

void TestSelectionText() {
    CHECK_STR(SelectionText(0, false, 0), L"");
    CHECK_STR(SelectionText(1, true, 0), L"1 élément sélectionné");          // dossier : pas de taille
    CHECK_STR(SelectionText(3, true, 5000), L"3 éléments sélectionnés");     // mélange : pas de taille
    CHECK_STR(SelectionText(2, false, 5000, false), L"2 éléments sélectionnés");  // lecteurs : jamais
    wchar_t b[64];
    StrFormatByteSizeEx(4740000, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, _countof(b));  // format de la machine
    CHECK_STR(SelectionText(3, false, 4740000), std::wstring(L"3 éléments sélectionnés  ") + b);
    StrFormatByteSizeEx(0, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, b, _countof(b));
    CHECK_STR(SelectionText(1, false, 0), std::wstring(L"1 élément sélectionné  ") + b);  // fichier vide
}

void TestAqs() {
    SYSTEMTIME st{2026, 3, 0, 15, 12, 0, 0, 0};
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    const int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    auto m = [&](const wchar_t* q, const wchar_t* name, uint32_t attr = 0, uint64_t size = 10, int64_t mtime = 0) {
        return SearchQuery(q, now).Match(name, (uint32_t)wcslen(name), attr, size, mtime);
    };
    // Mots : tous requis, comme avant.
    CHECK(m(L"rap txt", L"Rapport.txt") && !m(L"rap doc", L"Rapport.txt"));
    CHECK(m(L"rap AND txt", L"rapport.txt") && m(L"rap ET txt", L"rapport.txt"));
    // OU, NON, « - », parenthèses.
    CHECK(m(L"facture OR devis", L"devis 2026.pdf") && m(L"facture OU devis", L"facture.pdf"));
    CHECK(!m(L"facture OR devis", L"contrat.pdf"));
    CHECK(m(L"rapport NOT brouillon", L"rapport final.docx") && !m(L"rapport NOT brouillon", L"rapport brouillon.docx"));
    CHECK(m(L"rapport -brouillon", L"rapport.docx") && !m(L"rapport -brouillon", L"brouillon rapport.docx"));
    CHECK(m(L"(facture OR devis) ext:pdf", L"devis.pdf") && !m(L"(facture OR devis) ext:pdf", L"devis.docx"));
    CHECK(m(L"or", L"Ordre.txt"));  // « or » en minuscules : un mot
    // Guillemets : phrase exacte ; nom:.
    CHECK(m(L"\"rapport final\"", L"le rapport final.txt") && !m(L"\"rapport final\"", L"final rapport.txt"));
    CHECK(m(L"nom:\"rapport final\"", L"Rapport Final v2.txt"));
    CHECK(m(L"nom:*.txt", L"a.txt") && !m(L"nom:*.txt", L"a.txt.bak"));
    CHECK(m(L"\"OR\"", L"OR.txt"));  // opérateur entre guillemets : un mot
    // Catégories et tailles nommées.
    CHECK(m(L"type:image", L"vacances.JPG") && !m(L"type:image", L"notes.txt"));
    CHECK(m(L"kind:music", L"a.flac") && m(L"type:vidéo", L"film.mkv") && m(L"type:document", L"cv.pdf"));
    CHECK(m(L"type:dossier", L"x", FILE_ATTRIBUTE_DIRECTORY) && !m(L"type:dossier", L"x"));
    CHECK(m(L"size:gigantic", L"iso", 0, 5ull << 30) && !m(L"taille:gigantesque", L"iso", 0, 3ull << 30));
    CHECK(m(L"taille:vide", L"z", 0, 0) && !m(L"taille:vide", L"z", 0, 1));
    CHECK(m(L"taille:petit", L"p", 0, 100 * 1024) && !m(L"taille:petit", L"p", 0, 2 << 20));
    CHECK(m(L"-taille:minuscule", L"p", 0, 100 * 1024));
    // Dates JJ/MM/AAAA.
    SYSTEMTIME l{2026, 1, 0, 10, 12, 0, 0, 0}, u;
    TzSpecificLocalTimeToSystemTime(nullptr, &l, &u);
    SystemTimeToFileTime(&u, &ft);
    const int64_t jan10 = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    CHECK(m(L"datemodified:01/01/2026..31/01/2026", L"a", 0, 1, jan10));
    CHECK(!m(L"datemodified:01/02/2026..28/02/2026", L"a", 0, 1, jan10));
    CHECK(m(L"date:10/01/2026", L"a", 0, 1, jan10));
    // Chemin relatif : les mots portent sur le nom, l'extension sur la fin.
    CHECK(!m(L"photos", L"photos\\a.txt") && m(L"ext:txt", L"photos\\a.txt"));
    // Requête vide, demandes à l'index des noms.
    CHECK(SearchQuery(L"  ", now).empty() && !SearchQuery(L"-x", now).empty());
    auto asks = SearchQuery(L"facture OR devis ext:pdf", now).NameAsks();
    CHECK(asks.size() == 2 && asks[0] == L"facture" && asks[1] == L"devis");
    asks = SearchQuery(L"type:image", now).NameAsks();
    CHECK(asks.size() == 1 && asks[0].rfind(L"*.jpg;*.", 0) == 0);  // une seule passe sur l'index
    auto nm = [](const wchar_t* q, const wchar_t* n) { return NameMatches(q, n, (uint32_t)wcslen(n)); };
    CHECK(nm(L"*.jpg;*.PNG", L"Vacances.png") && nm(L"*.jpg;*.png", L"a.JPG") && !nm(L"*.jpg;*.png", L"a.gif"));
    CHECK(!nm(L"*.jpg", L"a.jpg.bak") && nm(L"*.jpg", L".jpg") && !nm(L"*.jpg", L"jpg"));
    CHECK(nm(L"*.jpg;rap*", L"rapport.txt") && nm(L"été*.txt;*.zz", L"Été 2026.txt"));
    CHECK(nm(L"*.jpg;*.png vac", L"vacances.png") && !nm(L"*.jpg;*.png vac", L"noel.png"));
    CHECK(nm(L"*.é", L"x.é") && !nm(L"*.é", L"x.e"));
    CHECK(SearchQuery(L"rapport OR taille:>1Go", now).NameAsks().empty());  // une branche sans nom : parcours
    CHECK(SearchQuery(L"-tmp", now).NameAsks().empty());
    CHECK(SearchQuery(L"taille:>1Mo", now).needsStat() && !SearchQuery(L"ext:pdf", now).needsStat());
    // Contenu : tenu pour vrai sans fonction (filtre instantané), sinon demandé, et jamais pour un dossier.
    SearchQuery c(L"rapport contenu:budget", now);
    CHECK(c.hasContent() && c.contentWords().size() == 1 && c.contentWords()[0] == L"budget");
    CHECK(c.Match(L"rapport.txt", 11, 0, 1, 0));
    ContentFn yes = [](const std::wstring&) { return true; }, no = [](const std::wstring&) { return false; };
    CHECK(c.Match(L"rapport.txt", 11, 0, 1, 0, &yes) && !c.Match(L"rapport.txt", 11, 0, 1, 0, &no));
    CHECK(!c.Match(L"rapport", 7, FILE_ATTRIBUTE_DIRECTORY, 0, 0, &yes));
    int asked = 0;
    ContentFn count = [&](const std::wstring&) { ++asked; return true; };
    c.Match(L"autre.txt", 9, 0, 1, 0, &count);
    CHECK(asked == 0);  // le nom ne correspond pas : le fichier n'est pas lu

    // Lecture du contenu (hors index) : UTF-8, UTF-16, ANSI ; binaire ignoré.
    const std::wstring base = Join(g_root, L"aqs");
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(Join(base, L"sous").c_str(), nullptr);
    auto write = [](const std::wstring& p, const std::string& bytes) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
        CloseHandle(h);
    };
    write(Join(base, L"utf8.txt"), "\xEF\xBB\xBFLe budget de l'\xC3\xA9t\xC3\xA9 2026");
    write(Join(base, L"sous\\ansi.md"), "Prevision du BUDGET \xE9t\xE9");
    write(Join(base, L"utf16.txt"), std::string("\xFF\xFE" "b\0u\0d\0g\0e\0t\0", 14));
    write(Join(base, L"binaire.bin"), std::string("budget\0\0\0budget", 15));
    write(Join(base, L"autre.txt"), "rien ici");
    CHECK(FileContainsText(Join(base, L"utf8.txt"), L"ETE 2026"));  // sans casse ni accents
    CHECK(FileContainsText(Join(base, L"sous\\ansi.md"), L"budget été"));
    CHECK(FileContainsText(Join(base, L"utf16.txt"), L"budget"));
    CHECK(!FileContainsText(Join(base, L"binaire.bin"), L"budget"));
    CHECK(!FileContainsText(Join(base, L"autre.txt"), L"budget"));
    DirData r;
    r.path = base;
    SearchTree(r, L"contenu:budget -utf16", {}, {});
    std::vector<std::wstring> names;
    for (const auto& e : r.entries) names.emplace_back(r.name(e), e.nameLen);
    std::sort(names.begin(), names.end());
    CHECK(names.size() == 2 && names[0] == L"sous\\ansi.md" && names[1] == L"utf8.txt");
    DirData o;
    o.path = base;
    SearchTree(o, L"autre OR binaire", {}, {});
    CHECK(o.entries.size() == 2);
    RemoveTree(base);

    // Windows Search : requête SQL, et l'index répond (ou se dit indisponible) sans planter.
    const std::wstring sql = WSearch_Sql(L"C:\\Users\\a'b", L"fac\"ture", 50);
    CHECK(sql.find(L"SCOPE='file:C:/Users/a''b'") != std::wstring::npos);
    CHECK(sql.find(L"'\"facture*\"'") != std::wstring::npos && sql.find(L"TOP 50") != std::wstring::npos);
    CHECK(WSearch_Sql(L"", L"deux mots", 10).find(L"'\"deux mots\"'") != std::wstring::npos);
    CHECK_STR(WSearch_PathOfUrl(L"file:C:/Users/x/a b.txt"), L"C:\\Users\\x\\a b.txt");
    CHECK_STR(WSearch_PathOfUrl(L"file:///C:/x"), L"C:\\x");
    CHECK_STR(WSearch_PathOfUrl(L"file://srv/partage/y"), L"\\\\srv\\partage\\y");
    CHECK(WSearch_PathOfUrl(L"mapi://x").empty());
    // Un fichier des Documents, s'ils sont indexés, est retrouvé par son contenu, sous son vrai chemin.
    wchar_t docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)) && WSearch_Indexed(docs) == 1) {
        const std::wstring probe = Join(docs, L"fe_test_contenu.txt");
        MakeFile(probe, 0);
        HANDLE hf = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD wr = 0;
        WriteFile(hf, "bloubiboulgaquantique", 21, &wr, nullptr);
        CloseHandle(hf);
        std::wstring low = probe;
        CharLowerBuffW(low.data(), (DWORD)low.size());
        bool found = false;
        const double t0 = NowMs();
        while (!found && NowMs() - t0 < 60000) {
            std::unordered_set<std::wstring> set;
            found = WSearch_Contains(docs, L"bloubiboulga", set, 50) && set.count(low);
            if (!found) Sleep(1000);
        }
        printf("  Windows Search : fichier des Documents retrouve par son contenu en %.0f s\n", (NowMs() - t0) / 1000);
        CHECK(found);
        DeleteFileW(probe.c_str());
    }
    const int idx = WSearch_Indexed(g_root);
    CHECK(idx >= -1 && idx <= 1);
    if (WSearch_Indexed(L"") == 1) {
        std::unordered_set<std::wstring> set;
        const double t0 = NowMs();
        CHECK(WSearch_Contains(L"", L"zzqqxxyyneverfound", set, 10));
        printf("  Windows Search : requete de contenu en %.0f ms\n", NowMs() - t0);
        CHECK(set.empty());
    }

    // Recherches récentes : la plus récente d'abord, sans doublon, limitée.
    auto h = HistoryPush({L"b", L"a"}, L"  A ");
    CHECK(h.size() == 2 && h[0] == L"A" && h[1] == L"b");
    CHECK(HistoryPush(h, L"   ").size() == 2);
    std::vector<std::wstring> many;
    for (int i = 0; i < 40; ++i) many = HistoryPush(many, L"q" + std::to_wstring(i));
    CHECK(many.size() == kSearchHistoryMax && many[0] == L"q39");
}

void TestSearch() {
    auto m = [](const wchar_t* q, const wchar_t* n) { return NameMatches(q, n, (uint32_t)wcslen(n)); };
    CHECK(m(L"rap", L"Rapport.txt"));
    CHECK(m(L"resume", L"RÉSUMÉ.doc"));      // ni casse ni accents
    CHECK(m(L"*.txt", L"notes.TXT") && !m(L"*.txt", L"notes.txt.bak"));
    CHECK(m(L"rap txt", L"rapport.txt"));    // tous les mots
    CHECK(!m(L"rap doc", L"rapport.txt"));
    CHECK(m(L"  rap  ", L"rapport.txt"));
    CHECK(!m(L"", L"rapport.txt") && !m(L"   ", L"rapport.txt"));
    CHECK(m(L"rap*", L"Rapport.txt") && !m(L"?ap", L"Rapport.txt"));

    const std::wstring base = Join(g_root, L"recherche");
    for (const wchar_t* d : {L"", L"a", L"a\\b", L"cache", L"vide"}) CreateDirectoryW(Join(base, d).c_str(), nullptr);
    MakeFile(Join(base, L"a\\b\\Rapport final.txt"), 10);
    MakeFile(Join(base, L"a\\rapport_2.TXT"), 20);
    MakeFile(Join(base, L"autre.txt"), 1);
    MakeFile(Join(base, L"cache\\rapport_cache.txt"), 1);
    CreateDirectoryW(Join(base, L"a\\Rapports").c_str(), nullptr);  // un dossier qui correspond
    SetFileAttributesW(Join(base, L"cache").c_str(), FILE_ATTRIBUTE_HIDDEN);
    _wsystem((L"cmd /c mklink /J \"" + Join(base, L"jonction") + L"\" \"" + Join(base, L"a") + L"\" >nul").c_str());

    auto names = [](const DirData& d) {
        std::vector<std::wstring> v;
        for (const Entry& e : d.entries) v.emplace_back(d.name(e));
        std::sort(v.begin(), v.end());
        std::wstring s;
        for (auto& x : v) s += x + L";";
        return s;
    };
    const ViewPrefs noHidden{false, false, true}, all{true, true, true};
    DirData d;
    d.path = base;
    int partials = 0;
    SearchTree(d, L"rapport", noHidden, CancelToken{}, [&](std::shared_ptr<DirData>) { ++partials; });
    CHECK(d.error == 0 && d.searchDone && !d.truncated);
    // Pas le dossier caché, pas de doublon par la jonction.
    CHECK_STR(names(d), L"a\\Rapports;a\\b\\Rapport final.txt;a\\rapport_2.TXT;");  // ordre ordinal
    CHECK(d.scanned >= 4);
    CHECK(d.nDirs == 1 && d.nFiles == 2);
    // L'extension se lit dans le nom, pas dans le chemin : « a\Rapports » n'en a pas.
    for (const Entry& e : d.entries)
        if (e.isDir()) CHECK(e.extOff == e.nameLen);
    // Triable comme un dossier ordinaire.
    CHECK(SortOrder(d, COL_NAME, true, noHidden).size() == 3);

    DirData h;
    h.path = base;
    SearchTree(h, L"rapport", all, CancelToken{});
    CHECK(h.entries.size() == 4);  // + le fichier du dossier caché

    DirData none;
    none.path = base;
    SearchTree(none, L"zzzintrouvable", all, CancelToken{});
    CHECK(none.error == 0 && none.entries.empty() && none.searchDone);

    DirData bad;
    bad.path = Join(base, L"absent");
    SearchTree(bad, L"x", all, CancelToken{});
    CHECK(bad.error != 0);

    std::atomic<uint64_t> gen{5};
    DirData cancelled;
    cancelled.path = base;
    SearchTree(cancelled, L"rapport", all, CancelToken{&gen, 4});
    CHECK(cancelled.error == ERROR_CANCELLED);

    RemoveDirectoryW(Join(base, L"jonction").c_str());
    SetFileAttributesW(Join(base, L"cache").c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveTree(base);
}

void TestFastCopy() {
    const std::wstring base = Join(g_root, L"fast");
    const std::wstring src = Join(base, L"src"), dst = Join(base, L"dst");
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW(dst.c_str(), nullptr);
    const std::wstring tree = Join(src, L"Arbre");
    CreateDirectoryW(tree.c_str(), nullptr);
    CreateDirectoryW(Join(tree, L"sous").c_str(), nullptr);
    CreateDirectoryW(Join(tree, L"masqué").c_str(), nullptr);
    SetFileAttributesW(Join(tree, L"masqué").c_str(), FILE_ATTRIBUTE_HIDDEN);
    CreateDirectoryW(Join(tree, L"vide").c_str(), nullptr);
    for (int i = 0; i < 300; ++i) MakeFile(Join(Join(tree, L"sous"), (L"f" + std::to_wstring(i) + L".bin").c_str()), 1000);
    MakeFile(Join(Join(tree, L"masqué"), L"lecture seule.txt"), 5);
    SetFileAttributesW(Join(Join(tree, L"masqué"), L"lecture seule.txt").c_str(), FILE_ATTRIBUTE_READONLY);
    MakeFile(Join(src, L"seul.txt"), 70000);

    // Copie : arbre complet, attributs conservés, noms créés renvoyés.
    {
        FastProgress p;
        FastOutcome o;
        const double t0 = NowMs();
        CHECK(FastTransfer({tree, Join(src, L"seul.txt")}, dst, false, p, o));
        printf("  copie rapide : 302 fichiers en %.0f ms\n", NowMs() - t0);
        CHECK(o.created.size() == 2 && o.shellLeft.empty() && !o.cancelled);
        CHECK(p.filesDone == 302 && p.filesTotal == 302);
        CHECK(p.bytesDone == 300 * 1000 + 5 + 70000);
        CHECK(Exists(Join(Join(Join(dst, L"Arbre"), L"sous"), L"f299.bin")));
        CHECK(Exists(Join(Join(dst, L"Arbre"), L"vide")));
        CHECK(Attrs(Join(Join(dst, L"Arbre"), L"masqué")) & FILE_ATTRIBUTE_HIDDEN);
        CHECK(Attrs(Join(Join(Join(dst, L"Arbre"), L"masqué"), L"lecture seule.txt")) & FILE_ATTRIBUTE_READONLY);
        CHECK(Exists(Join(tree, L"sous")));  // copie : la source reste
    }
    // Ce que le Shell doit faire lui-même : rien n'est touché.
    {
        FastProgress p;
        FastOutcome o;
        CHECK(!FastTransfer({tree}, dst, false, p, o));                  // conflit de nom
        CHECK(!FastTransfer({Join(src, L"seul.txt")}, src, false, p, o));  // même dossier : « - Copie »
        CHECK(!FastTransfer({tree}, Join(tree, L"sous"), false, p, o));    // dossier copié dans lui-même
        CHECK(!FastTransfer({Join(src, L"absent")}, base, false, p, o));
        CHECK(!FastTransfer({tree}, L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", false, p, o));  // pas un chemin
        // Jonction dans l'arbre : le Shell sait quoi en faire, pas nous.
        const std::wstring withLink = Join(base, L"AvecLien");
        CreateDirectoryW(withLink.c_str(), nullptr);
        const std::wstring cmd = L"cmd /c mklink /J \"" + Join(withLink, L"lien") + L"\" \"" + src + L"\" >nul";
        _wsystem(cmd.c_str());
        CHECK(Attrs(Join(withLink, L"lien")) & FILE_ATTRIBUTE_REPARSE_POINT);
        CHECK(!FastTransfer({withLink}, dst, false, p, o));
        CHECK(!Exists(Join(dst, L"AvecLien")));
        RemoveDirectoryW(Join(withLink, L"lien").c_str());  // la jonction seule, pas sa cible
        RemoveDirectoryW(withLink.c_str());
    }
    // Un fichier verrouillé : reprise, ce qui est copié reste ; lui seul est rendu au Shell, vers son dossier.
    {
        const std::wstring locked = Join(Join(tree, L"sous"), L"f150.bin");
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        const std::wstring d2 = Join(base, L"dst2");
        CreateDirectoryW(d2.c_str(), nullptr);
        FastProgress p;
        FastOutcome o;
        CHECK(FastTransfer({tree, Join(src, L"seul.txt")}, d2, false, p, o));
        CHECK(o.shellLeft.size() == 1 && o.shellLeft[0].first == locked &&
              o.shellLeft[0].second == Join(Join(d2, L"Arbre"), L"sous"));
        CHECK(o.created.size() == 2 && o.created[0] == L"Arbre" && o.created[1] == L"seul.txt");
        CHECK(p.filesDone == 301 && Exists(Join(Join(Join(d2, L"Arbre"), L"sous"), L"f149.bin")));
        CHECK(!Exists(Join(Join(Join(d2, L"Arbre"), L"sous"), L"f150.bin")));
        CloseHandle(h);
        SetFileAttributesW(Join(Join(Join(d2, L"Arbre"), L"masqué"), L"lecture seule.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    // Annulation : rien n'est copié, c'est signalé.
    {
        const std::wstring d3 = Join(base, L"dst3");
        CreateDirectoryW(d3.c_str(), nullptr);
        FastProgress p;
        p.cancel = true;
        FastOutcome o;
        FastTransfer({tree}, d3, false, p, o);
        CHECK(o.cancelled && o.shellLeft.empty() && p.filesDone == 0);
    }
    // Déplacement sur le même volume : un renommage, la source disparaît.
    {
        const std::wstring d4 = Join(base, L"dst4");
        CreateDirectoryW(d4.c_str(), nullptr);
        FastProgress p;
        FastOutcome o;
        const double t0 = NowMs();
        CHECK(FastTransfer({tree}, d4, true, p, o));
        printf("  deplacement (meme volume) : %.1f ms\n", NowMs() - t0);
        CHECK(o.created.size() == 1 && o.shellLeft.empty());
        CHECK(!Exists(tree));
        CHECK(Exists(Join(Join(Join(d4, L"Arbre"), L"sous"), L"f0.bin")));
    }
    CHECK(FastCopyThreads(src, dst) >= 1);
    // Ménage : retirer les attributs lecture seule avant de supprimer.
    for (const wchar_t* d : {L"dst", L"dst4"})
        SetFileAttributesW(Join(Join(Join(Join(base, d), L"Arbre"), L"masqué"), L"lecture seule.txt").c_str(),
                           FILE_ATTRIBUTE_NORMAL);
    RemoveTree(base);
}

void TestBatchRename() {
    std::vector<RenameItem> items{{L"Photo 1.JPG"}, {L"photo 2.jpg"}, {L"Dossier photo", true}};
    RenameRule r;
    r.find = L"photo";
    r.replace = L"Vacances";
    auto n = BatchNames(r, items, L"Images");
    CHECK(n.size() == 3);
    CHECK_STR(n[0], L"Vacances 1.JPG");
    CHECK_STR(n[1], L"Vacances 2.jpg");
    CHECK_STR(n[2], L"Dossier Vacances");  // dossier : pas d'extension
    r.matchCase = true;
    n = BatchNames(r, items, L"Images");
    CHECK_STR(n[0], L"Photo 1.JPG");  // casse respectée : « Photo » ne correspond pas
    CHECK_STR(n[1], L"Vacances 2.jpg");

    RenameRule c;
    c.pattern = L"{dossier} {n:3} ({nom})";
    c.start = 9;
    c.step = 2;
    n = BatchNames(c, items, L"Images");
    CHECK_STR(n[0], L"Images 009 (Photo 1).JPG");
    CHECK_STR(n[1], L"Images 011 (photo 2).jpg");
    c.pattern = L"{nom}.{ext} {{x}} {inconnu}";
    n = BatchNames(c, std::vector<RenameItem>{RenameItem{L"a.txt"}}, L"");
    CHECK_STR(n[0], L"a.txt {x} {inconnu}.txt");

    RenameRule re;
    re.regex = true;
    re.find = L"(\\d+)";
    re.replace = L"[$1]";
    n = BatchNames(re, items, L"");
    CHECK_STR(n[0], L"Photo [1].JPG");
    re.find = L"(";
    std::wstring err;
    n = BatchNames(re, items, L"", &err);
    CHECK(!err.empty() && n[0] == L"Photo 1.JPG");

    RenameRule lc;
    lc.nameCase = NameCase::Lower;
    lc.withExtension = true;
    n = BatchNames(lc, items, L"");
    CHECK_STR(n[0], L"photo 1.jpg");
    lc.withExtension = false;
    lc.nameCase = NameCase::Title;
    n = BatchNames(lc, std::vector<RenameItem>{RenameItem{L"le chat-noir (été).TXT"}}, L"");
    CHECK_STR(n[0], L"Le Chat-Noir (Été).TXT");
    lc.nameCase = NameCase::Sentence;
    n = BatchNames(lc, std::vector<RenameItem>{RenameItem{L"LE CHAT.txt"}}, L"");
    CHECK_STR(n[0], L"Le chat.txt");
    lc.nameCase = NameCase::Upper;
    n = BatchNames(lc, std::vector<RenameItem>{RenameItem{L"été.txt"}}, L"");
    CHECK_STR(n[0], L"ÉTÉ.txt");

    RenameRule d;
    d.pattern = L"{date} {nom}";
    RenameItem dated{L"x.png"};
    dated.modified = LocalFt(2026, 3, 7);
    n = BatchNames(d, {dated}, L"");
    CHECK_STR(n[0], L"2026-03-07 x.png");

    CHECK(NameProblem(L"ok.txt").empty());
    CHECK(!NameProblem(L"").empty());
    CHECK(!NameProblem(L"a?b").empty());
    CHECK(!NameProblem(L"con.txt").empty());
    CHECK(!NameProblem(L"LPT1").empty());
    CHECK(!NameProblem(L"fin.").empty());
    CHECK(NameProblem(L"console.txt").empty());
    const auto p = BatchProblems({L"a.txt", L"A.TXT", L"b?.txt", L"con.txt", L"pris.txt", L"libre.txt"}, {L"PRIS.txt"});
    CHECK(!p[0].empty() && !p[1].empty());  // doublon dans le lot (sans la casse)
    CHECK(!p[2].empty() && !p[3].empty() && !p[4].empty());
    CHECK(p[5].empty());

    CHECK_STR(ShortcutName(L"a.txt", [](const std::wstring&) { return false; }), L"a.txt - Raccourci.lnk");
    CHECK_STR(ShortcutName(L"a.txt", [](const std::wstring& s) { return s == L"a.txt - Raccourci.lnk"; }),
              L"a.txt - Raccourci (2).lnk");
}

void TestBlock28Ops() {
    const std::wstring base = Join(g_root, L"b28");
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring src = Join(base, L"src");
    CreateDirectoryW(src.c_str(), nullptr);

    // Vérification : identique, puis différent (même taille), puis taille différente.
    const std::wstring a = Join(src, L"a.bin"), b = Join(src, L"b.bin"), c = Join(src, L"c.bin");
    MakeFile(a, 300000);
    MakeFile(c, 1000);
    CHECK(CopyFileW(a.c_str(), b.c_str(), FALSE));
    CHECK(FastVerify(a, b));
    {
        HANDLE h = CreateFileW(b.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        SetFilePointer(h, 150000, nullptr, FILE_BEGIN);
        DWORD w = 0;
        const char x = 0x5A;
        WriteFile(h, &x, 1, &w, nullptr);
        CloseHandle(h);
    }
    CHECK(!FastVerify(a, b));
    CHECK(!FastVerify(a, c));

    // Copie vérifiée : relue en entier, comptée deux fois dans la progression.
    {
        const std::wstring d = Join(base, L"dverif");
        CreateDirectoryW(d.c_str(), nullptr);
        FastProgress p;
        p.verify = true;
        FastOutcome o;
        CHECK(FastTransfer({a, c}, d, false, p, o));
        CHECK(o.created.size() == 2 && o.shellLeft.empty() && o.verifyFailed == 0);
        CHECK(p.bytesTotal == 2ull * 301000 && p.bytesDone == p.bytesTotal);
        CHECK(FastVerify(a, Join(d, L"a.bin")));
    }

    // Reprise : un fichier ouvert un court instant est réessayé et passe.
    {
        const std::wstring d = Join(base, L"dretry");
        CreateDirectoryW(d.c_str(), nullptr);
        HANDLE h = CreateFileW(c.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        std::thread release([h] {
            Sleep(120);
            CloseHandle(h);
        });
        FastProgress p;
        FastOutcome o;
        CHECK(FastTransfer({c}, d, false, p, o));
        release.join();
        CHECK(o.created.size() == 1 && o.shellLeft.empty() && o.retried == 1);
        CHECK(Exists(Join(d, L"c.bin")));
    }
    CHECK(FastTransient(ERROR_SHARING_VIOLATION) && FastTransient(ERROR_NETNAME_DELETED));
    CHECK(!FastTransient(ERROR_ACCESS_DENIED) && !FastTransient(ERROR_DISK_FULL));

    // Perte de propriétés : un flux nommé compte, Zone.Identifier non.
    {
        const std::wstring tree = Join(base, L"props");
        CreateDirectoryW(tree.c_str(), nullptr);
        const std::wstring withProps = Join(tree, L"avec.txt"), zoneOnly = Join(tree, L"zone.txt");
        MakeFile(withProps, 10);
        MakeFile(zoneOnly, 10);
        MakeFile(withProps + L":Résumé", 20);
        MakeFile(zoneOnly + L":Zone.Identifier", 20);
        auto lost = FileOps_PropertyLoss({tree}, 0);
        CHECK(lost.size() == 1 && lost[0] == withProps);
        CHECK(FileOps_PropertyLoss({tree}, FILE_NAMED_STREAMS | FILE_SUPPORTS_ENCRYPTION).empty());
        CHECK(FileOps_PropertyLoss({zoneOnly}, 0).empty());
        bool enc = true;
        FileOps_PropertyLoss({withProps}, FILE_NAMED_STREAMS, &enc);
        CHECK(!enc);
    }

    // Qui utilise ce fichier : ce processus, qui le garde ouvert (directement et via son dossier).
    {
        HANDLE h = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        const DWORD me = GetCurrentProcessId();
        auto has = [&](const std::vector<Locker>& v) {
            return std::any_of(v.begin(), v.end(), [&](const Locker& l) { return l.pid == me && !l.app.empty(); });
        };
        CHECK(has(FileOps_LockerList({a})));
        CHECK(has(FileOps_LockerList({src})));
        CloseHandle(h);
        CHECK(!has(FileOps_LockerList({a})));
    }

    // Coller en tant que raccourci : « nom - Raccourci.lnk », numéroté ensuite.
    {
        HWND sink = CreateWindowW(L"FeTestSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(sink != nullptr);
        constexpr UINT kMsg = WM_APP + 11;
        auto run = [&](FileOp op) -> std::unique_ptr<FileOpResult> {
            op.silent = true;
            FileOps_Start(nullptr, sink, kMsg, std::move(op));
            const double t0 = NowMs();
            MSG m;
            while (NowMs() - t0 < 15000) {
                if (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) return std::unique_ptr<FileOpResult>((FileOpResult*)m.lParam);
                Sleep(2);
            }
            return nullptr;
        };
        const std::wstring links = Join(base, L"liens");
        CreateDirectoryW(links.c_str(), nullptr);
        auto link = [&](std::vector<std::wstring> paths) {
            FileOp op;
            op.kind = FileOpKind::Link;
            op.folder = links;
            op.paths = std::move(paths);
            return run(std::move(op));
        };
        auto r = link({a, src});
        CHECK(r && SUCCEEDED(r->hr) && r->created.size() == 2);
        CHECK(r && r->created.size() == 2 && r->created[0] == L"a.bin - Raccourci.lnk" && r->created[1] == L"src - Raccourci.lnk");
        std::wstring target;
        CHECK(ResolveShortcut(Join(links, L"a.bin - Raccourci.lnk"), target) && _wcsicmp(target.c_str(), a.c_str()) == 0);
        r = link({a});
        CHECK(r && r->created.size() == 1 && r->created[0] == L"a.bin - Raccourci (2).lnk");

        // Copie par la file : reprise d'un fichier verrouillé dans un dossier (le reste est copié, le Shell, en
        // silence, échoue sur lui seul), puis copie vérifiée.
        const std::wstring tree = Join(base, L"arbre");
        CreateDirectoryW(tree.c_str(), nullptr);
        for (int i = 0; i < 20; ++i) MakeFile(Join(tree, (L"f" + std::to_wstring(i) + L".bin").c_str()), 500);
        const std::wstring d = Join(base, L"dfile");
        CreateDirectoryW(d.c_str(), nullptr);
        FileOps_SetVerify(true);
        FileOp cp;
        cp.kind = FileOpKind::Copy;
        cp.folder = d;
        cp.paths = {tree};
        r = run(std::move(cp));
        FileOps_SetVerify(false);
        CHECK(r && SUCCEEDED(r->hr) && r->verified && r->verifyFailed == 0 && r->created.size() == 1);
        CHECK(Exists(Join(Join(d, L"arbre"), L"f19.bin")));
        DestroyWindow(sink);
    }
    RemoveTree(base);
}

void TestFileOps() {    HWND sink = CreateWindowW(L"FeTestSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
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
    // Données d'annulation : source complète et nouveau nom.
    CHECK(r && r->sources.size() == 1 && r->sources[0] == Join(ops, L"Nouveau dossier (2)") && r->newName == L"renommé");

    {  // Nouveau document texte : un fichier vide, numéro avant l'extension
        const std::wstring nd = Join(ops, L"nouveau");
        CreateDirectoryW(nd.c_str(), nullptr);
        auto mk = [&] {
            FileOp f;
            f.kind = FileOpKind::NewFolder;
            f.folder = nd;
            f.newName = L"Nouveau document texte.txt";
            f.newFile = true;
            return f;
        };
        r = run(mk());
        CHECK_STR(created(r), L"Nouveau document texte.txt");
        CHECK(r && r->newFile);
        r = run(mk());
        CHECK_STR(created(r), L"Nouveau document texte (2).txt");
        const DWORD a = GetFileAttributesW(Join(nd, L"Nouveau document texte (2).txt").c_str());
        CHECK(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
        RemoveTree(nd);
    }

    {  // Renommage multiple : échange de noms, casse seule, nom pris hors sélection
        const std::wstring md = Join(ops, L"multi");
        CreateDirectoryW(md.c_str(), nullptr);
        MakeFile(Join(md, L"a.txt"), 1);
        MakeFile(Join(md, L"b.txt"), 2);
        MakeFile(Join(md, L"c.txt"), 3);
        MakeFile(Join(md, L"pris.txt"), 4);
        FileOp m;
        m.kind = FileOpKind::Rename;
        m.folder = md;
        m.paths = {Join(md, L"a.txt"), Join(md, L"b.txt"), Join(md, L"c.txt")};
        m.newNames = {L"b.txt", L"a.txt", L"C.TXT"};  // a ↔ b, c → C
        r = run(std::move(m));
        CHECK(r && SUCCEEDED(r->hr));
        CHECK(r && r->created.size() == 3 && r->renamedFrom.size() == 3);
        auto sizeOf = [](const std::wstring& p) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) ? (int)d.nFileSizeLow : -1;
        };
        CHECK(sizeOf(Join(md, L"b.txt")) == 1 && sizeOf(Join(md, L"a.txt")) == 2);
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(Join(md, L"c.txt").c_str(), &fd);
            CHECK(h != INVALID_HANDLE_VALUE && wcscmp(fd.cFileName, L"C.TXT") == 0);
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
        }
        // « pris.txt » n'est pas renommé : la cible n'est pas dans la sélection.
        FileOp m2;
        m2.kind = FileOpKind::Rename;
        m2.folder = md;
        m2.paths = {Join(md, L"a.txt"), Join(md, L"b.txt")};
        m2.newNames = {L"pris.txt", L"x.txt"};
        r = run(std::move(m2));
        CHECK(r && r->created.size() == 1 && r->created[0] == L"x.txt");
        CHECK(sizeOf(Join(md, L"pris.txt")) == 4 && sizeOf(Join(md, L"a.txt")) == 2 && sizeOf(Join(md, L"x.txt")) == 1);
        // Aucun nom temporaire ne traîne.
        int temps = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(md, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do temps += wcsstr(fd.cFileName, L".fe-ren-") != nullptr;
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        CHECK(temps == 0);
        RemoveTree(md);
    }

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

    // Annuler une suppression : retour depuis la Corbeille (fichiers $I / $R).
    {
        const std::wstring bin = Join(ops, L"corbeille é.txt");
        MakeFile(bin, 123);
        FileOp rb;
        rb.kind = FileOpKind::Delete;
        rb.folder = ops;
        rb.paths = {bin};
        r = run(std::move(rb));
        CHECK(r && SUCCEEDED(r->hr) && !r->permanent && r->started > 0 && r->sources.size() == 1);
        CHECK(!Exists(bin));
        const int64_t since = r ? r->started : 0;
        CHECK(Recycle_Restore(bin, since) == 0);
        CHECK(Exists(bin));
        CHECK(Recycle_Restore(bin, since) == ERROR_FILE_NOT_FOUND);  // déjà restauré
        // Même chose par une opération Restore (celle de Ctrl+Z).
        FileOp rb2;
        rb2.kind = FileOpKind::Delete;
        rb2.folder = ops;
        rb2.paths = {bin};
        r = run(std::move(rb2));
        FileOp rs;
        rs.kind = FileOpKind::Restore;
        rs.folder = ops;
        rs.paths = {bin};
        rs.since = r ? r->started : 0;
        r = run(std::move(rs));
        CHECK(r && SUCCEEDED(r->hr));
        CHECK_STR(created(r), L"corbeille é.txt");
        CHECK(Exists(bin));
        CHECK(Recycle_Restore(Join(ops, L"jamais supprimé.txt"), 0) == ERROR_FILE_NOT_FOUND);
        // Fichier ouvert : Restart Manager nomme le processus qui le garde.
        HANDLE h = CreateFileW(bin.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const std::wstring who = FileOps_Lockers({bin});
        printf("  fichier ouvert par : [%ls]\n", who.c_str());
        CHECK(!who.empty());
        CloseHandle(h);
        CHECK(FileOps_Lockers({bin}).empty());
    }

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

// Dépôt : comme ListDropTarget, on confie les éléments à la cible du Shell
// (dossier, ou élément survolé) avec les touches de l'utilisateur.
void TestDrop() {
    const std::wstring src = Join(g_root, L"dnd_src"), dst = Join(g_root, L"dnd_dst");
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW(dst.c_str(), nullptr);
    CreateDirectoryW(Join(dst, L"sous").c_str(), nullptr);
    MakeFile(Join(src, L"copie.txt"), 5);
    MakeFile(Join(src, L"deplace.txt"), 5);
    MakeFile(Join(src, L"dans_sous.txt"), 5);
    auto waitFor = [](const std::wstring& p, bool present) {
        for (int i = 0; i < 500; ++i) {
            if ((GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) == present) return true;
            MSG m;  // la cible Shell peut poster des messages à son thread
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Sleep(10);
        }
        return false;
    };
    auto drop = [&](const std::wstring& file, const std::wstring& onItem, DWORD keys) -> DWORD {
        IDataObject* data = nullptr;
        IDropTarget* target = nullptr;
        if (!ShellObjectOf(nullptr, src, {file}, IID_IDataObject, (void**)&data)) return DROPEFFECT_NONE;
        DWORD e = DROPEFFECT_NONE;
        if (DragDrop_ShellTarget(nullptr, dst, onItem, &target)) {
            const POINTL pt{0, 0};
            e = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->DragEnter(data, MK_LBUTTON | keys, pt, &e);
            e = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->DragOver(MK_LBUTTON | keys, pt, &e);
            e = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            target->Drop(data, keys, pt, &e);
            target->Release();
        }
        data->Release();
        return e;
    };
    IDropTarget* none = nullptr;
    CHECK(DragDrop_ShellTarget(nullptr, dst, L"", &none));  // cible du dossier
    if (none) none->Release();
    CHECK(DragDrop_ShellTarget(nullptr, dst, L"sous", &none));  // cible d'un sous-dossier
    if (none) none->Release();

    CHECK(drop(L"copie.txt", L"", MK_CONTROL) == DROPEFFECT_COPY);  // Ctrl : copier
    CHECK(waitFor(Join(dst, L"copie.txt"), true));
    CHECK(GetFileAttributesW(Join(src, L"copie.txt").c_str()) != INVALID_FILE_ATTRIBUTES);

    drop(L"deplace.txt", L"", MK_SHIFT);  // Maj : déplacer
    CHECK(waitFor(Join(dst, L"deplace.txt"), true));
    CHECK(waitFor(Join(src, L"deplace.txt"), false));

    drop(L"dans_sous.txt", L"sous", 0);  // même volume, sans touche : déplacer, dans le dossier survolé
    CHECK(waitFor(Join(dst, L"sous\\dans_sous.txt"), true));
    CHECK(waitFor(Join(src, L"dans_sous.txt"), false));
}

void TestCopyQueue() {
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring kWin = FileOps_DiskKey(win), kRoot = FileOps_DiskKey(g_root);
    CHECK(!kWin.empty());
    CHECK_STR(FileOps_DiskKey(Join(win, L"System32")), kWin);  // même volume : même clé
    CHECK_STR(FileOps_DiskKey(L"\\\\Serveur\\partage\\x"), L"net:serveur");
    printf("  disques : %ls, %ls\n", kWin.c_str(), kRoot.c_str());

    HWND sink = CreateWindowW(L"FeTestSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    constexpr UINT kMsg = WM_APP + 11;
    const std::wstring base = Join(g_root, L"queue");
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring big = Join(base, L"gros.bin");
    {
        HANDLE h = CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        std::string block(1 << 20, 'x');
        DWORD w = 0;
        for (int i = 0; i < 64; ++i) WriteFile(h, block.data(), (DWORD)block.size(), &w, nullptr);
        CloseHandle(h);
    }
    auto copyTo = [&](const wchar_t* sub) {
        const std::wstring d = Join(base, sub);
        CreateDirectoryW(d.c_str(), nullptr);
        FileOp op;
        op.kind = FileOpKind::Copy;
        op.folder = d;
        op.paths = {big};
        op.silent = true;
        FileOps_Start(nullptr, sink, kMsg, std::move(op));
        return Join(d, L"gros.bin");
    };
    auto waitResults = [&](int n) {
        std::vector<std::unique_ptr<FileOpResult>> v;
        const double t0 = NowMs();
        MSG m;
        while ((int)v.size() < n && NowMs() - t0 < 20000) {
            if (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) v.emplace_back((FileOpResult*)m.lParam);
            else Sleep(2);
        }
        return v;
    };
    auto state = [](uint64_t id, bool& running, bool& paused) {
        for (const auto& j : FileOps_Jobs())
            if (j.id == id) {
                running = j.running;
                paused = j.paused;
                return true;
            }
        return false;
    };

    // A puis B sur le même disque : B attend. Retenue, B ne démarre pas même après A.
    const std::wstring a = copyTo(L"a"), b = copyTo(L"b");
    auto jobs = FileOps_Jobs();
    CHECK(jobs.size() == 2);
    if (jobs.size() != 2) return;
    const uint64_t idB = jobs[1].id;
    CHECK(jobs[0].title.find(L"gros.bin") != std::wstring::npos);
    FileOps_Pause(idB, true);
    auto r = waitResults(1);  // A
    CHECK(r.size() == 1 && SUCCEEDED(r[0]->hr));
    CHECK(Exists(a));
    Sleep(200);
    bool running = true, paused = false;
    CHECK(state(idB, running, paused) && !running && paused);
    CHECK(!Exists(b));
    FileOps_Pause(idB, false);  // reprise : B passe
    r = waitResults(1);
    CHECK(r.size() == 1 && SUCCEEDED(r[0]->hr));
    CHECK(Exists(b));

    // C retenue puis annulée : rien n'est copié, résultat « annulé ».
    const std::wstring c = copyTo(L"c");
    jobs = FileOps_Jobs();
    if (!jobs.empty()) FileOps_CancelJob(jobs.back().id);
    r = waitResults(1);
    CHECK(r.size() == 1 && r[0]->aborted && !FileOps_IsFailure(*r[0]));
    CHECK(FileOps_Jobs().empty());
    DestroyWindow(sink);
    RemoveTree(base);
}

void TestPane() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = Join(tmp, L"fe_test_pane");
    CreateDirectoryW(dir.c_str(), nullptr);
    auto write = [&](const wchar_t* name, const std::string& bytes) {
        const std::wstring p = Join(dir, name);
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
        CloseHandle(h);
        return p;
    };
    bool cut = true;
    CHECK_STR(Pane_ReadText(write(L"u8.txt", "\xEF\xBB\xBFh\xC3\xA9\nl\xC3\xA0"), 1000, cut), L"hé\r\nlà");
    CHECK(!cut);
    CHECK_STR(Pane_ReadText(write(L"nobom.txt", "a\r\nb\rc"), 1000, cut), L"a\r\nb\r\nc");
    CHECK_STR(Pane_ReadText(write(L"u16.txt", std::string("\xFF\xFEx\0y\0", 6)), 1000, cut), L"xy");
    CHECK_STR(Pane_ReadText(write(L"acp.txt", "caf\xE9"), 1000, cut).substr(0, 3), L"caf");  // pas de l'UTF-8 : page locale
    // Tronqué au milieu d'un caractère UTF-8 : le caractère coupé disparaît.
    CHECK_STR(Pane_ReadText(write(L"long.txt", "ab\xC3\xA9\xC3\xA9"), 3, cut), L"ab");
    CHECK(cut);
    CHECK_STR(Pane_ReadText(write(L"nul.txt", std::string("a\0b", 3)), 1000, cut), L"a b");
    CHECK_STR(Pane_ReadText(Join(dir, L"absent.txt"), 1000, cut), L"");

    const std::wstring f = write(L"web.pdf", "x");
    CHECK(!Pane_FromInternet(f));
    write(L"web.pdf:Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
    CHECK(Pane_FromInternet(f));
    write(L"web.pdf:Zone.Identifier", "[ZoneTransfer]\r\nZoneId=1\r\n");
    CHECK(!Pane_FromInternet(f));
    for (const wchar_t* n : {L"u8.txt", L"nobom.txt", L"u16.txt", L"acp.txt", L"long.txt", L"nul.txt", L"web.pdf"})
        DeleteFileW(Join(dir, n).c_str());
    RemoveDirectoryW(dir.c_str());
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
    TestFolderSize();
    TestVolumeIndex();
    TestSearchFilters();
    TestColumns();
    TestPane();
    TestEdgeCases();
    TestLongPath();
    TestParallelMatchesSequential();
    TestCancelHungNetworkPath();
    TestShortcut();
    TestEnumerateFileIsNotDirectory();
    TestPrepareMenu();
    TestFileNames();
    TestWatch();
    TestEnumerateNearest();
    TestNavPane();
    TestPins();
    TestCrumbs();
    TestViewPrefs();
    TestSelectionText();
    TestSearch();
    TestAqs();
    TestFastCopy();
    TestFileOps();
    TestBatchRename();
    TestBlock28Ops();
    TestPlaces();
    TestSessions();
    TestGroups();
    TestNet();
    TestDeflate();
    TestArchive();
    TestCopyQueue();  // après TestFileOps : classe de fenêtre FeTestSink
    TestDrop();
    RemoveTree(g_root);
    printf("\n%d verifications OK, %d echec(s)\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
