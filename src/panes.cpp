#include "panes.h"

#include <propsys.h>
#include <commctrl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <memory>

#pragma comment(lib, "propsys")
#pragma comment(lib, "msimg32")

namespace {

// ───────────────────────────── État partagé ─────────────────────────────

enum Kind { K_NONE, K_MESSAGE, K_IMAGE, K_TEXT, K_HANDLER, K_DETAILS };

struct Row {
    PROPERTYKEY key{};
    std::wstring name, value;
    bool writable = false;
};

// Messages vers le thread de travail.
enum : UINT { W_PREVIEW = WM_APP + 1, W_RESIZE, W_UNLOAD, W_DETAILS, W_WRITE, W_QUIT };
// Messages vers le volet.
enum : UINT { P_RESULT = WM_APP + 20, P_WRITTEN };

struct Request {
    uint64_t seq = 0;
    PaneMode mode = PaneMode::None;
    std::vector<PaneItem> items;
    std::wstring folderTitle, folderText;
    HWND host = nullptr;
    RECT rc{};      // zone de l'aperçu (coordonnées de host)
    int imgPx = 0;  // taille de l'image voulue
    COLORREF bk = 0, fg = 0;
};

struct Result {
    uint64_t seq = 0;
    Kind kind = K_NONE;
    HBITMAP image = nullptr;
    std::wstring title, subtitle, message, text;
    std::vector<Row> rows;
    ~Result() {
        if (image) DeleteObject(image);
    }
};

struct WriteReq {
    std::vector<std::wstring> paths;
    PROPERTYKEY key{};
    std::wstring text;
};

struct WriteResult {
    bool ok = false;
    std::wstring message;
};

std::atomic<uint64_t> g_seq{0};  // dernière demande : les plus anciennes sont abandonnées
DWORD g_workerTid = 0;
HANDLE g_worker = nullptr;

HWND g_pane, g_props, g_text, g_host, g_editor;
PaneMode g_mode = PaneMode::None;
HFONT g_font = nullptr, g_bold = nullptr, g_mono = nullptr;
std::function<void(const std::wstring&)> g_status;

// Ce qui est affiché.
std::unique_ptr<Result> g_shown;
std::vector<PaneItem> g_items;  // dernière sélection demandée
std::wstring g_folderTitle, g_folderText;
std::vector<std::wstring> g_rowsPaths;  // chemins concernés par les propriétés affichées
int g_editRow = -1;

constexpr uint32_t kNotLocal = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;

int Dpi() { return g_pane ? (int)GetDpiForWindow(g_pane) : 96; }
int S(int v) { return MulDiv(v, Dpi(), 96); }

// ───────────────────────────── Thread de travail ─────────────────────────────

IPreviewHandler* g_handler = nullptr;

void Unload() {
    if (!g_handler) return;
    g_handler->Unload();
    g_handler->Release();
    g_handler = nullptr;
}

// Image 32 bits du Shell : miniature, ou icône agrandie.
HBITMAP ImageOf(const std::wstring& path, int px, bool local) {
    if (px <= 0) return nullptr;
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP bmp = nullptr;
    // Fichier cloud absent du disque : l'icône seulement (une miniature le téléchargerait).
    const SIIGBF fl = local ? (SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK) : SIIGBF_ICONONLY;
    if (FAILED(f->GetImage(SIZE{px, px}, fl, &bmp))) bmp = nullptr;
    f->Release();
    if (!bmp) return nullptr;
    // Image opaque livrée avec un canal alpha nul : rendue opaque (dessin par AlphaBlend).
    DIBSECTION ds{};
    if (GetObjectW(bmp, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits) {
        auto* p = (uint32_t*)ds.dsBm.bmBits;
        const size_t n = (size_t)ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);
        bool alpha = false;
        for (size_t i = 0; i < n && !alpha; ++i) alpha = (p[i] >> 24) != 0;
        if (!alpha)
            for (size_t i = 0; i < n; ++i) p[i] |= 0xFF000000u;
    }
    return bmp;
}

bool IsTextExt(const wchar_t* ext) {
    static const wchar_t* kText[] = {
        L".txt",  L".log",  L".md",   L".markdown", L".json", L".xml",  L".csv",  L".tsv",  L".ini",  L".cfg",
        L".conf", L".yml",  L".yaml", L".toml",     L".c",    L".cc",   L".cpp",  L".cxx",  L".h",    L".hh",
        L".hpp",  L".hxx",  L".cs",   L".java",     L".js",   L".mjs",  L".cjs",  L".ts",   L".tsx",  L".jsx",
        L".py",   L".rb",   L".go",   L".rs",       L".php",  L".lua",  L".pl",   L".swift", L".kt",  L".kts",
        L".sql",  L".sh",   L".bash", L".ps1",      L".psm1", L".psd1", L".bat",  L".cmd",  L".css",  L".scss",
        L".less", L".vue",      L".svelte", L".gitignore", L".gitattributes", L".editorconfig",
        L".env",  L".properties", L".gradle", L".cmake", L".mk", L".makefile", L".diff", L".patch", L".srt", L".reg",
        L".nfo",  L".tex",  L".r",    L".m",        L".asm",  L".s",    L".vb",   L".fs",   L".dart", L".zig"};
    if (!ext || !*ext) return false;
    for (const wchar_t* e : kText)
        if (!_wcsicmp(ext, e)) return true;
    return false;
}

bool IsImage(const std::wstring& path) {
    PERCEIVED p{};
    PERCEIVEDFLAG f{};
    return SUCCEEDED(AssocGetPerceivedType(PathFindExtensionW(path.c_str()), &p, &f, nullptr)) && p == PERCEIVED_TYPE_IMAGE;
}

bool HandlerFor(const std::wstring& path, CLSID& clsid) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!*ext) return false;
    wchar_t buf[64];
    DWORD cch = 64;
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, ext,
                                 L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", buf, &cch)))
        return false;
    return SUCCEEDED(CLSIDFromString(buf, &clsid));
}

// Gestionnaire d'aperçu de Windows, hors processus (prevhost.exe) : un plantage
// ou une lenteur n'atteint pas l'application. Jamais chargé dans le processus.
bool LoadHandler(const Request& r, const std::wstring& path) {
    CLSID clsid;
    if (!HandlerFor(path, clsid)) return false;
    IPreviewHandler* ph = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&ph)))) return false;
    bool init = false;
    if (IInitializeWithFile* i = nullptr; SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&i)))) {
        init = SUCCEEDED(i->Initialize(path.c_str(), STGM_READ));
        i->Release();
    }
    if (!init)
        if (IInitializeWithItem* i = nullptr; SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&i)))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                init = SUCCEEDED(i->Initialize(item, STGM_READ));
                item->Release();
            }
            i->Release();
        }
    if (!init)
        if (IInitializeWithStream* i = nullptr; SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&i)))) {
            IStream* st = nullptr;
            if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st))) {
                init = SUCCEEDED(i->Initialize(st, STGM_READ));
                st->Release();
            }
            i->Release();
        }
    if (!init || r.seq != g_seq.load()) {
        ph->Release();
        return false;
    }
    if (IPreviewHandlerVisuals* v = nullptr; SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&v)))) {
        v->SetBackgroundColor(r.bk);
        v->SetTextColor(r.fg);
        v->Release();
    }
    if (FAILED(ph->SetWindow(r.host, &r.rc)) || FAILED(ph->DoPreview())) {
        ph->Unload();
        ph->Release();
        return false;
    }
    g_handler = ph;
    return true;
}

std::wstring ErrorText(HRESULT hr) {
    wchar_t* s = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)hr, 0, (LPWSTR)&s, 0, nullptr);
    std::wstring t = s ? s : L"";
    if (s) LocalFree(s);
    while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'.')) t.pop_back();
    if (t.empty()) {
        wchar_t b[32];
        swprintf_s(b, L"erreur 0x%08X", (unsigned)hr);
        t = b;
    }
    return t;
}

void Preview(const Request& r, Result& out) {
    Unload();
    if (r.items.empty()) {
        out.kind = K_MESSAGE;
        out.message = L"Sélectionnez un fichier pour afficher son aperçu.";
        return;
    }
    const PaneItem& it = r.items.front();
    const bool local = !(it.attr & kNotLocal);
    auto imageWith = [&](const std::wstring& msg) {
        out.kind = K_IMAGE;
        out.image = ImageOf(it.path, r.imgPx, local);
        out.message = msg;
    };
    if (r.items.size() > 1) return imageWith(std::to_wstring(r.items.size()) + L" éléments sélectionnés");
    if (it.attr & FILE_ATTRIBUTE_DIRECTORY) return imageWith(L"");
    if (!local) return imageWith(L"Fichier disponible en ligne uniquement : pas d'aperçu (il faudrait le télécharger).");
    const wchar_t* ext = PathFindExtensionW(it.path.c_str());
    // Comme l'Explorateur : pas d'aperçu des fichiers venus d'Internet (sauf PDF).
    if (_wcsicmp(ext, L".pdf") != 0 && Pane_FromInternet(it.path))
        return imageWith(L"Aperçu bloqué : ce fichier provient d'Internet. Débloquez-le dans ses propriétés pour l'afficher.");
    if (IsImage(it.path)) return imageWith(L"");  // plus rapide que le gestionnaire de Windows
    if (IsTextExt(ext) || (!*ext && it.size < (1u << 20))) {
        bool truncated = false;
        out.kind = K_TEXT;
        out.text = Pane_ReadText(it.path, 512 * 1024, truncated);
        if (truncated) out.text += L"\r\n…";
        return;
    }
    if (LoadHandler(r, it.path)) {
        out.kind = K_HANDLER;
        return;
    }
    imageWith(L"Pas d'aperçu pour ce type de fichier.");
}

std::wstring FormatValue(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring s;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY) {
        PWSTR t = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, pv, PDFF_DEFAULT, &t)) && t) {
            s = t;
            CoTaskMemFree(t);
        }
    }
    PropVariantClear(&pv);
    return s;
}

// Propriétés que l'on ne modifie pas ici (nom : renommer ; dates et taille : système de fichiers).
bool NeverEdited(const PROPERTYKEY& k) {
    for (const PROPERTYKEY* p : {&PKEY_ItemNameDisplay, &PKEY_ItemTypeText, &PKEY_Size, &PKEY_DateModified,
                                 &PKEY_DateCreated, &PKEY_DateAccessed, &PKEY_FileAttributes, &PKEY_ItemFolderPathDisplay,
                                 &PKEY_FileOwner, &PKEY_ComputerName, &PKEY_FileName})
        if (IsEqualPropertyKey(*p, k)) return true;
    return false;
}

void Details(const Request& r, Result& out) {
    out.kind = K_DETAILS;
    if (r.items.empty()) {
        out.title = r.folderTitle;
        out.subtitle = r.folderText;
        return;
    }
    const PaneItem& first = r.items.front();
    const bool allLocal = std::none_of(r.items.begin(), r.items.end(), [](const PaneItem& i) { return (i.attr & kNotLocal) != 0; });
    out.image = ImageOf(first.path, r.imgPx, !(first.attr & kNotLocal));
    IShellItem2* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(first.path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        out.title = PathFindFileNameW(first.path.c_str());
        return;
    }
    // Liste des propriétés que l'Explorateur montre pour ce type de fichier.
    IPropertyDescriptionList* pl = nullptr;
    if (FAILED(item->GetPropertyDescriptionList(PKEY_PropList_PreviewDetails, IID_PPV_ARGS(&pl))))
        PSGetPropertyDescriptionListFromString(L"prop:System.ItemTypeText;System.Size;System.DateModified;System.DateCreated",
                                               IID_PPV_ARGS(&pl));
    // Magasins de propriétés des éléments (lecture), 200 au plus.
    std::vector<IPropertyStore*> stores;
    for (size_t i = 0; i < r.items.size() && i < 200 && r.seq == g_seq.load(); ++i) {
        IPropertyStore* ps = nullptr;
        const GETPROPERTYSTOREFLAGS fl = (r.items[i].attr & kNotLocal) ? GPS_FASTPROPERTIESONLY : GPS_BESTEFFORT;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(r.items[i].path.c_str(), nullptr, fl, IID_PPV_ARGS(&ps))))
            stores.push_back(ps);
    }
    if (r.items.size() == 1) {
        out.title = PathFindFileNameW(first.path.c_str());
        if (!stores.empty()) out.subtitle = FormatValue(stores[0], PKEY_ItemTypeText);
    } else {
        uint64_t bytes = 0;
        bool dirs = false;
        for (const auto& i : r.items) {
            if (i.attr & FILE_ATTRIBUTE_DIRECTORY) dirs = true;
            else bytes += i.size;
        }
        out.title = std::to_wstring(r.items.size()) + L" éléments sélectionnés";
        wchar_t b[64] = L"";
        if (!dirs) StrFormatByteSizeW((LONGLONG)bytes, b, 64);
        out.subtitle = dirs ? L"" : std::wstring(L"Taille totale : ") + b;
    }
    IPropertyStoreCapabilities* caps = nullptr;
    if (!stores.empty()) stores[0]->QueryInterface(IID_PPV_ARGS(&caps));
    UINT n = 0;
    if (pl) pl->GetCount(&n);
    for (UINT k = 0; k < n && r.seq == g_seq.load(); ++k) {
        IPropertyDescription* pd = nullptr;
        if (FAILED(pl->GetAt(k, IID_PPV_ARGS(&pd)))) continue;
        Row row;
        PWSTR name = nullptr;
        PROPDESC_TYPE_FLAGS tf{};
        if (SUCCEEDED(pd->GetPropertyKey(&row.key)) && SUCCEEDED(pd->GetDisplayName(&name)) && name) {
            row.name = name;
            pd->GetTypeFlags(PDTF_MASK_ALL, &tf);
            row.writable = allLocal && !stores.empty() && !(tf & PDTF_ISINNATE) && !NeverEdited(row.key) &&
                           (!caps || caps->IsPropertyWritable(row.key) == S_OK);
            bool same = true;
            for (size_t i = 0; i < stores.size(); ++i) {
                const std::wstring v = FormatValue(stores[i], row.key);
                if (i == 0) row.value = v;
                else if (v != row.value) { same = false; break; }
            }
            if (!same) row.value = L"(plusieurs valeurs)";
            // Les dates et le type sont déjà au-dessus pour un seul élément : on garde ce qui informe.
            if (!row.value.empty() || row.writable) out.rows.push_back(std::move(row));
        }
        CoTaskMemFree(name);
        pd->Release();
    }
    if (caps) caps->Release();
    for (auto* ps : stores) ps->Release();
    if (pl) pl->Release();
    item->Release();
}

void Write(const WriteReq& w, WriteResult& out) {
    VARTYPE vt = VT_LPWSTR;
    IPropertyDescription* pd = nullptr;
    if (SUCCEEDED(PSGetPropertyDescription(w.key, IID_PPV_ARGS(&pd)))) {
        pd->GetPropertyType(&vt);
        pd->Release();
    }
    HRESULT last = S_OK;
    size_t done = 0;
    for (const auto& path : w.paths) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        HRESULT hr = S_OK;
        if (!w.text.empty()) {
            if (vt == (VT_VECTOR | VT_LPWSTR)) hr = InitPropVariantFromStringAsVector(w.text.c_str(), &pv);
            else {
                PROPVARIANT s;
                hr = InitPropVariantFromString(w.text.c_str(), &s);
                if (SUCCEEDED(hr)) {
                    if (vt == VT_LPWSTR || vt == VT_EMPTY || vt == VT_NULL) pv = s;
                    else {
                        hr = PropVariantChangeType(&pv, s, PVCHF_DEFAULT, vt);
                        PropVariantClear(&s);
                    }
                }
            }
            if (SUCCEEDED(hr)) {
                const HRESULT c = PSCoerceToCanonicalValue(w.key, &pv);
                if (FAILED(c)) hr = c;
            }
        }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(hr)) hr = SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(&ps));
        if (SUCCEEDED(hr)) hr = ps->SetValue(w.key, pv);
        if (SUCCEEDED(hr)) hr = ps->Commit();
        if (ps) ps->Release();
        PropVariantClear(&pv);
        if (SUCCEEDED(hr)) ++done;
        else last = hr;
    }
    out.ok = done == w.paths.size();
    if (out.ok) out.message = w.paths.size() > 1 ? L"Propriété enregistrée pour " + std::to_wstring(done) + L" éléments"
                                                 : L"Propriété enregistrée";
    else out.message = L"Impossible d'enregistrer la propriété : " + ErrorText(last);
}

DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // crée la file de messages
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        switch (m.message) {
            case W_PREVIEW:
            case W_DETAILS: {
                std::unique_ptr<Request> r((Request*)m.lParam);
                if (r->seq != g_seq.load()) break;  // une demande plus récente suit
                auto res = std::make_unique<Result>();
                res->seq = r->seq;
                if (m.message == W_PREVIEW) Preview(*r, *res);
                else {
                    Unload();
                    Details(*r, *res);
                }
                if (r->seq != g_seq.load()) {
                    Unload();
                    break;
                }
                if (PostMessageW(g_pane, P_RESULT, 0, (LPARAM)res.get())) res.release();
                break;
            }
            case W_RESIZE:
                if (g_handler) {
                    const RECT rc{(LONG)LOWORD(m.wParam), (LONG)HIWORD(m.wParam), (LONG)LOWORD(m.lParam), (LONG)HIWORD(m.lParam)};
                    g_handler->SetRect(&rc);
                }
                break;
            case W_UNLOAD: Unload(); break;
            case W_WRITE: {
                std::unique_ptr<WriteReq> w((WriteReq*)m.lParam);
                auto res = std::make_unique<WriteResult>();
                Write(*w, *res);
                if (PostMessageW(g_pane, P_WRITTEN, 0, (LPARAM)res.get())) res.release();
                break;
            }
            case W_QUIT:
                Unload();
                CoUninitialize();
                return 0;
        }
    }
    Unload();
    CoUninitialize();
    return 0;
}

void Post(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_worker) {
        g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, &g_workerTid);
        if (g_worker) SetThreadPriority(g_worker, THREAD_PRIORITY_BELOW_NORMAL);
    }
    // La file du thread n'existe qu'une fois qu'il a démarré : on réessaie un court instant.
    for (int i = 0; i < 200; ++i) {
        if (PostThreadMessageW(g_workerTid, msg, wp, lp)) return;
        Sleep(1);
    }
    if (msg == W_PREVIEW || msg == W_DETAILS) delete (Request*)lp;
    if (msg == W_WRITE) delete (WriteReq*)lp;
}

// ───────────────────────────── Volet (thread de l'interface) ─────────────────────────────

struct Areas {
    RECT image{}, title{}, subtitle{}, body{}, message{};
};

// Disposition : détails = image, nom, type, puis propriétés ; aperçu = tout l'espace.
Areas Layout() {
    Areas a;
    RECT rc;
    GetClientRect(g_pane, &rc);
    const int m = S(10), w = rc.right - 2 * m;
    TEXTMETRICW tm{};
    if (HDC dc = GetDC(g_pane)) {
        HGDIOBJ old = SelectObject(dc, g_font);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(g_pane, dc);
    }
    const int lh = tm.tmHeight + S(2);
    if (g_mode == PaneMode::Details) {
        const int img = std::clamp(w, 0, S(180));
        a.image = {m, m, rc.right - m, m + img};
        a.title = {m, a.image.bottom + S(8), rc.right - m, a.image.bottom + S(8) + lh * 2};
        a.subtitle = {m, a.title.bottom, rc.right - m, a.title.bottom + lh};
        a.body = {S(2), a.subtitle.bottom + S(8), rc.right - S(2), rc.bottom};
    } else {
        a.body = {S(1), 0, rc.right, rc.bottom};
        a.message = {m, rc.bottom - m - lh * 3, rc.right - m, rc.bottom - m};
        a.image = {m, m, rc.right - m, a.message.top - S(8)};
    }
    return a;
}

void Arrange() {
    if (!g_pane) return;
    const Areas a = Layout();
    const Kind k = g_shown ? g_shown->kind : K_NONE;
    auto place = [](HWND h, bool show, const RECT& r) {
        if (show) SetWindowPos(h, nullptr, r.left, r.top, std::max(0L, r.right - r.left), std::max(0L, r.bottom - r.top),
                               SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else ShowWindow(h, SW_HIDE);
    };
    place(g_props, k == K_DETAILS, a.body);
    place(g_text, k == K_TEXT, a.body);
    place(g_host, k == K_HANDLER, a.body);
    if (k == K_HANDLER) {
        RECT hr;
        GetClientRect(g_host, &hr);
        PostThreadMessageW(g_workerTid, W_RESIZE, MAKEWPARAM(0, 0), MAKELPARAM(hr.right, hr.bottom));
    }
    if (k == K_DETAILS) {
        RECT lr;
        GetClientRect(g_props, &lr);
        const int nameW = std::max(S(80), (int)lr.right * 2 / 5);
        ListView_SetColumnWidth(g_props, 0, nameW);
        ListView_SetColumnWidth(g_props, 1, std::max(S(60), (int)lr.right - nameW));
    }
    InvalidateRect(g_pane, nullptr, TRUE);
}

void EndEdit(bool commit);

void ShowResult(Result* raw) {
    std::unique_ptr<Result> r(raw);
    if (r->seq != g_seq.load()) return;
    EndEdit(false);
    g_shown = std::move(r);
    if (g_shown->kind == K_TEXT) SetWindowTextW(g_text, g_shown->text.c_str());
    if (g_shown->kind == K_DETAILS) {
        SendMessageW(g_props, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_props);
        for (size_t i = 0; i < g_shown->rows.size(); ++i) {
            const Row& row = g_shown->rows[i];
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = (int)i;
            it.pszText = (LPWSTR)row.name.c_str();
            it.lParam = (LPARAM)i;
            ListView_InsertItem(g_props, &it);
            const std::wstring v = row.value.empty() && row.writable ? L"Ajouter…" : row.value;
            ListView_SetItemText(g_props, (int)i, 1, (LPWSTR)v.c_str());
        }
        SendMessageW(g_props, WM_SETREDRAW, TRUE, 0);
    }
    Arrange();
}

// Demande au thread ce qu'il faut afficher pour la sélection courante.
void Request_() {
    if (!g_pane || g_mode == PaneMode::None) return;
    EndEdit(false);
    auto* r = new Request;
    r->seq = ++g_seq;
    r->mode = g_mode;
    r->items = g_items;
    r->folderTitle = g_folderTitle;
    r->folderText = g_folderText;
    r->host = g_host;
    const Areas a = Layout();
    r->rc = {0, 0, a.body.right - a.body.left, a.body.bottom - a.body.top};
    r->imgPx = std::max(0, (int)std::min(a.image.right - a.image.left, a.image.bottom - a.image.top));
    r->bk = GetSysColor(COLOR_WINDOW);
    r->fg = GetSysColor(COLOR_WINDOWTEXT);
    g_rowsPaths.clear();
    for (const auto& i : g_items) g_rowsPaths.push_back(i.path);
    if (g_shown && g_shown->kind == K_HANDLER) PostThreadMessageW(g_workerTid, W_UNLOAD, 0, 0);
    Post(g_mode == PaneMode::Preview ? W_PREVIEW : W_DETAILS, 0, (LPARAM)r);
}

void Paint(HDC dc) {
    RECT rc;
    GetClientRect(g_pane, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    // Filet à gauche : séparation d'avec la liste.
    RECT edge{0, 0, 1, rc.bottom};
    FillRect(dc, &edge, GetSysColorBrush(COLOR_3DLIGHT));
    if (!g_shown) return;
    const Areas a = Layout();
    const Result& r = *g_shown;
    if (r.image && (r.kind == K_IMAGE || r.kind == K_DETAILS)) {
        BITMAP bm{};
        GetObjectW(r.image, sizeof bm, &bm);
        const int bw = bm.bmWidth, bh = std::abs(bm.bmHeight);
        const int aw = a.image.right - a.image.left, ah = a.image.bottom - a.image.top;
        if (bw > 0 && bh > 0 && aw > 0 && ah > 0) {
            const double k = std::min({1.0 * aw / bw, 1.0 * ah / bh, 1.0});  // jamais agrandie : elle serait floue
            const int w = (int)(bw * k), h = (int)(bh * k);
            const int x = a.image.left + (aw - w) / 2, y = a.image.top + (ah - h) / 2;
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, r.image);
            SetStretchBltMode(dc, HALFTONE);
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, x, y, w, h, mem, 0, 0, bw, bh, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
    }
    SetBkMode(dc, TRANSPARENT);
    if (r.kind == K_DETAILS) {
        HGDIOBJ old = SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        RECT t = a.title;
        DrawTextW(dc, r.title.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, g_font);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        RECT s = a.subtitle;
        DrawTextW(dc, r.subtitle.c_str(), -1, &s, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    if ((r.kind == K_IMAGE || r.kind == K_MESSAGE) && !r.message.empty()) {
        HGDIOBJ old = SelectObject(dc, g_font);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        RECT m = r.kind == K_MESSAGE ? RECT{a.image.left, a.image.top, a.image.right, a.message.bottom} : a.message;
        DrawTextW(dc, r.message.c_str(), -1, &m,
                  DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | (r.kind == K_MESSAGE ? DT_VCENTER : 0));
        SelectObject(dc, old);
    }
}

// ── Modification d'une propriété (double-clic, F2 ou Entrée sur une valeur) ──

WNDPROC g_editProc = nullptr;

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { EndEdit(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { EndEdit(false); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KILLFOCUS) {
        LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp);
        PostMessageW(g_pane, WM_APP + 30, 0, 0);  // valider hors du changement de focus
        return r;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

void BeginEdit(int row) {
    if (!g_shown || g_shown->kind != K_DETAILS || row < 0 || row >= (int)g_shown->rows.size()) return;
    const Row& r = g_shown->rows[row];
    if (!r.writable) {
        MessageBeep(MB_OK);
        return;
    }
    EndEdit(false);
    RECT rc;
    ListView_GetSubItemRect(g_props, row, 1, LVIR_LABEL, &rc);
    const std::wstring v = r.value == L"(plusieurs valeurs)" ? L"" : r.value;
    g_editor = CreateWindowExW(0, WC_EDITW, v.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, rc.left, rc.top,
                               rc.right - rc.left, rc.bottom - rc.top, g_props, nullptr, nullptr, nullptr);
    SendMessageW(g_editor, WM_SETFONT, (WPARAM)g_font, FALSE);
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_editor, GWLP_WNDPROC, (LONG_PTR)EditProc);
    g_editRow = row;
    SendMessageW(g_editor, EM_SETSEL, 0, -1);
    SetFocus(g_editor);
}

void EndEdit(bool commit) {
    if (!g_editor) return;
    HWND ed = g_editor;
    g_editor = nullptr;
    const int row = g_editRow;
    g_editRow = -1;
    if (commit && g_shown && g_shown->kind == K_DETAILS && row >= 0 && row < (int)g_shown->rows.size()) {
        wchar_t buf[2048] = {};
        GetWindowTextW(ed, buf, 2048);
        Row& r = g_shown->rows[row];
        std::wstring text = buf;
        if (text != r.value && !(text.empty() && r.value == L"(plusieurs valeurs)")) {
            auto* w = new WriteReq{g_rowsPaths, r.key, text};
            r.value = text;
            const std::wstring shown = text.empty() ? L"Ajouter…" : text;
            ListView_SetItemText(g_props, row, 1, (LPWSTR)shown.c_str());
            if (g_status) g_status(L"Enregistrement de « " + r.name + L" »…");
            Post(W_WRITE, 0, (LPARAM)w);
        }
    }
    DestroyWindow(ed);
    if (GetFocus() == nullptr) SetFocus(g_props);
}

LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void MakeFonts(HFONT base) {
    g_font = base;
    LOGFONTW lf{};
    GetObjectW(base, sizeof lf, &lf);
    if (g_bold) DeleteObject(g_bold);
    if (g_mono) DeleteObject(g_mono);
    LOGFONTW b = lf;
    b.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&b);
    LOGFONTW m = lf;
    wcscpy_s(m.lfFaceName, L"Consolas");
    m.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    g_mono = CreateFontIndirectW(&m);
    SendMessageW(g_props, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)g_mono, FALSE);
}

LRESULT CALLBACK PaneProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETFONT:
            MakeFonts((HFONT)wp);
            Arrange();
            return 0;
        case WM_SIZE: Arrange(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:  // texte de l'aperçu (lecture seule) : fond blanc, comme l'Explorateur
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case P_RESULT: ShowResult((Result*)lp); return 0;
        case P_WRITTEN: {
            std::unique_ptr<WriteResult> w((WriteResult*)lp);
            if (g_status) g_status(w->message);
            if (!w->ok) {  // valeurs réelles relues
                g_items.clear();
                Request_();
            }
            return 0;
        }
        case WM_APP + 30: EndEdit(true); return 0;
        case WM_NOTIFY: {
            auto* n = (NMHDR*)lp;
            if (n->hwndFrom != g_props) break;
            if (n->code == NM_DBLCLK) BeginEdit(((NMITEMACTIVATE*)lp)->iItem);
            else if (n->code == NM_RETURN ||
                     (n->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_F2))
                BeginEdit(ListView_GetNextItem(g_props, -1, LVNI_FOCUSED));
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND Pane_Create(HWND parent, int id, std::function<void(const std::wstring&)> status) {
    g_status = std::move(status);
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    WNDCLASSW wc{};
    wc.lpfnWndProc = PaneProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FastExplorerPane";
    RegisterClassW(&wc);
    WNDCLASSW hc{};
    hc.lpfnWndProc = HostProc;
    hc.hInstance = inst;
    hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hc.lpszClassName = L"FastExplorerPreviewHost";
    RegisterClassW(&hc);
    g_pane = CreateWindowExW(0, L"FastExplorerPane", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, inst, nullptr);
    g_props = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 0, 0, g_pane, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(g_props, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g_props, L"Explorer", nullptr);
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH | LVCF_SUBITEM;
    c.cx = 100;
    ListView_InsertColumn(g_props, 0, &c);
    c.iSubItem = 1;
    ListView_InsertColumn(g_props, 1, &c);
    g_text = CreateWindowExW(0, WC_EDITW, L"",
                             WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                 ES_NOHIDESEL,
                             0, 0, 0, 0, g_pane, nullptr, inst, nullptr);
    SendMessageW(g_text, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g_text, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    g_host = CreateWindowExW(0, L"FastExplorerPreviewHost", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, g_pane, nullptr,
                             inst, nullptr);
    return g_pane;
}

PaneMode Pane_Mode() { return g_mode; }

void Pane_SetMode(PaneMode m) {
    if (m == g_mode) return;
    g_mode = m;
    if (g_shown && g_shown->kind == K_HANDLER) PostThreadMessageW(g_workerTid, W_UNLOAD, 0, 0);
    g_shown.reset();
    ++g_seq;  // demandes en vol : périmées
    Arrange();
    ShowWindow(g_pane, m == PaneMode::None ? SW_HIDE : SW_SHOW);
    if (m != PaneMode::None) Request_();
}

void Pane_Show(const std::wstring& folderTitle, const std::wstring& folderText, std::vector<PaneItem> items) {
    if (g_mode == PaneMode::None) {
        g_items = std::move(items);
        g_folderTitle = folderTitle;
        g_folderText = folderText;
        return;
    }
    auto same = [&] {
        if (!g_shown || items.size() != g_items.size()) return false;
        if (items.empty()) return folderTitle == g_folderTitle && folderText == g_folderText;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].mtime != g_items[i].mtime || items[i].size != g_items[i].size ||
                _wcsicmp(items[i].path.c_str(), g_items[i].path.c_str()) != 0)
                return false;
        return true;
    };
    if (same()) return;
    g_items = std::move(items);
    g_folderTitle = folderTitle;
    g_folderText = folderText;
    Request_();
}

void Pane_Shutdown() {
    ++g_seq;
    if (!g_worker) return;
    PostThreadMessageW(g_workerTid, W_QUIT, 0, 0);
    WaitForSingleObject(g_worker, 2000);  // un gestionnaire bloqué ne retient pas la fermeture
    CloseHandle(g_worker);
    g_worker = nullptr;
}

bool Pane_FromInternet(const std::wstring& path) {
    HANDLE h = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[1024] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(h);
    const char* z = strstr(buf, "ZoneId=");
    return z && (z[7] == '3' || z[7] == '4');
}

std::wstring Pane_ReadText(const std::wstring& path, size_t maxBytes, bool& truncated) {
    truncated = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const size_t want = (size_t)std::min<long long>(size.QuadPart, (long long)maxBytes);
    truncated = (unsigned long long)size.QuadPart > maxBytes;
    std::string b(want, '\0');
    DWORD got = 0;
    if (want) ReadFile(h, b.data(), (DWORD)want, &got, nullptr);
    CloseHandle(h);
    b.resize(got);
    std::wstring w;
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {
        w.assign((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    } else {
        size_t start = b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF ? 3 : 0;
        size_t end = b.size();
        if (truncated)  // ne pas couper un caractère UTF-8 en deux
            for (int k = 0; k < 3 && end > start && ((unsigned char)b[end - 1] & 0xC0) == 0x80; ++k) --end;
        if (truncated && end > start && (unsigned char)b[end - 1] >= 0xC0) --end;
        const char* p = b.data() + start;
        const int n = (int)(end - start);
        UINT cp = CP_UTF8;
        int len = n ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, nullptr, 0) : 0;
        if (n && !len) {
            cp = CP_ACP;
            len = MultiByteToWideChar(cp, 0, p, n, nullptr, 0);
        }
        w.resize(len);
        if (len) MultiByteToWideChar(cp, 0, p, n, w.data(), len);
    }
    // Contrôle d'édition : fins de ligne CRLF ; caractères nuls remplacés.
    std::wstring out;
    out.reserve(w.size() + w.size() / 16);
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (c == L'\n' && (i == 0 || w[i - 1] != L'\r')) out += L'\r';
        if (c == L'\r' && (i + 1 >= w.size() || w[i + 1] != L'\n')) {
            out += L"\r\n";
            continue;
        }
        out += c ? c : L' ';
    }
    return out;
}
