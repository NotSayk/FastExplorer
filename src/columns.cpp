#include "columns.h"

#include "listing.h"

#include <commctrl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>

#pragma comment(lib, "propsys")

// ───────────────────────────── Catalogue ─────────────────────────────

namespace {

struct Builtin {
    int key;
    const wchar_t* id;
    const wchar_t* title;
    int width96;
    bool right;
    const PROPERTYKEY* pk;  // propriété équivalente (non proposée en double dans « Plus… »)
};

const Builtin kBuiltins[] = {
    {COL_NAME, L"name", L"Nom", 380, false, &PKEY_ItemNameDisplay},
    {COL_SIZE, L"size", L"Taille", 110, true, &PKEY_Size},
    {COL_TYPE, L"type", L"Type", 170, false, &PKEY_ItemTypeText},
    {COL_DATE, L"mtime", L"Modifié le", 170, false, &PKEY_DateModified},
    {COL_CTIME, L"ctime", L"Date de création", 170, false, &PKEY_DateCreated},
    {COL_ATIME, L"atime", L"Date d'accès", 170, false, &PKEY_DateAccessed},
    {COL_ATTR, L"attr", L"Attributs", 90, false, &PKEY_FileAttributes},
    {COL_ORIGIN, L"origin", L"Emplacement d'origine", 260, false, nullptr},  // emplacements virtuels seulement,
};

const Builtin* FindBuiltin(int key) {
    for (const auto& b : kBuiltins)
        if (b.key == key) return &b;
    return nullptr;
}

const Builtin* BuiltinForProp(const PROPERTYKEY& pk) {
    for (const auto& b : kBuiltins)
        if (b.pk && IsEqualPropertyKey(*b.pk, pk)) return &b;
    return nullptr;
}

}  // namespace

bool SameCol(const ColSpec& a, const ColSpec& b) {
    return a.key == b.key && (a.key != COL_PROP || IsEqualPropertyKey(a.pk, b.pk));
}

ColSpec Columns_Builtin(int key) {
    ColSpec c;
    const Builtin* b = FindBuiltin(key);
    if (!b) b = &kBuiltins[0];
    c.key = b->key;
    c.title = b->title;
    c.width96 = b->width96;
    c.right = b->right;
    return c;
}

std::vector<ColSpec> Columns_Default() {
    return {Columns_Builtin(COL_NAME), Columns_Builtin(COL_SIZE), Columns_Builtin(COL_TYPE), Columns_Builtin(COL_DATE)};
}

ColSpec Columns_Prop(const PROPERTYKEY& pk) {
    if (const Builtin* b = BuiltinForProp(pk)) return Columns_Builtin(b->key);
    ColSpec c;
    c.key = COL_PROP;
    c.pk = pk;
    IPropertyDescription* pd = nullptr;
    if (SUCCEEDED(PSGetPropertyDescription(pk, IID_PPV_ARGS(&pd)))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(pd->GetDisplayName(&name)) && name) {
            c.title = name;
            CoTaskMemFree(name);
        }
        UINT cch = 0;
        if (SUCCEEDED(pd->GetDefaultColumnWidth(&cch)) && cch) c.width96 = std::clamp((int)cch * 7 + 16, 100, 400);
        PROPDESC_DISPLAYTYPE dt{};
        if (SUCCEEDED(pd->GetDisplayType(&dt))) c.right = dt == PDDT_NUMBER;
        pd->Release();
    }
    if (c.title.empty()) {
        wchar_t s[64];
        if (SUCCEEDED(PSStringFromPropertyKey(pk, s, 64))) c.title = s;
    }
    return c;
}

std::vector<ColSpec> Columns_Common() {
    std::vector<ColSpec> v;
    for (int k : {COL_SIZE, COL_TYPE, COL_DATE, COL_CTIME, COL_ATIME, COL_ATTR}) v.push_back(Columns_Builtin(k));
    for (const PROPERTYKEY* pk : {&PKEY_ItemFolderPathDisplay, &PKEY_Author, &PKEY_Title, &PKEY_Image_Dimensions,
                                  &PKEY_Photo_DateTaken, &PKEY_Media_Duration, &PKEY_Music_Artist, &PKEY_Music_AlbumTitle,
                                  &PKEY_FileVersion, &PKEY_FileOwner})
        v.push_back(Columns_Prop(*pk));
    return v;
}

std::wstring Columns_Id(const ColSpec& c) {
    if (c.key != COL_PROP) {
        const Builtin* b = FindBuiltin(c.key);
        return b ? b->id : L"name";
    }
    wchar_t s[64];
    return SUCCEEDED(PSStringFromPropertyKey(c.pk, s, 64)) ? s : L"";
}

bool Columns_FromId(const std::wstring& id, ColSpec& c) {
    for (const auto& b : kBuiltins)
        if (id == b.id) {
            c = Columns_Builtin(b.key);
            return true;
        }
    PROPERTYKEY pk;
    if (id.empty() || id[0] != L'{' || FAILED(PSPropertyKeyFromString(id.c_str(), &pk))) return false;
    c = Columns_Prop(pk);
    return true;
}

// ───────────────────────────── « Plus… » ─────────────────────────────

namespace {

struct ChoiceItem {
    ColSpec spec;
    bool checked = false;
};

struct ChooseState {
    std::vector<ChoiceItem> items;
    std::vector<ColSpec>* cols = nullptr;
    HWND list = nullptr, edit = nullptr;
    bool filling = false;
};

constexpr int ID_FILTER = 100, ID_LIST = 101;

void FillChoices(ChooseState& st) {
    wchar_t f[256] = {};
    GetWindowTextW(st.edit, f, 256);
    st.filling = true;
    SendMessageW(st.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st.list);
    int row = 0;
    for (size_t i = 0; i < st.items.size(); ++i) {
        if (f[0] && !StrStrIW(st.items[i].spec.title.c_str(), f)) continue;
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row++;
        it.pszText = (LPWSTR)st.items[i].spec.title.c_str();
        it.lParam = (LPARAM)i;
        const int at = ListView_InsertItem(st.list, &it);
        ListView_SetCheckState(st.list, at, st.items[i].checked);
    }
    if (row) ListView_SetItemState(st.list, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    ListView_SetColumnWidth(st.list, 0, LVSCW_AUTOSIZE_USEHEADER);  // toute la largeur, sans défilement horizontal
    SendMessageW(st.list, WM_SETREDRAW, TRUE, 0);
    st.filling = false;
}

INT_PTR CALLBACK ChooseProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (ChooseState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG: {
            st = (ChooseState*)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            const HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
            RECT rc;
            GetClientRect(dlg, &rc);
            RECT u{7, 7, 50, 14};  // marge, bouton (unités de dialogue)
            MapDialogRect(dlg, &u);
            const int m = u.left, bw = u.right, bh = u.bottom, W = rc.right, H = rc.bottom;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dlg, (HMENU)(INT_PTR)id,
                                         nullptr, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
                return c;
            };
            st->edit = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, m, m, W - 2 * m, bh, ID_FILTER);
            Edit_SetCueBannerTextFocused(st->edit, L"Filtrer les propriétés", TRUE);
            const int ly = m + bh + m / 2, lh = H - ly - bh - 2 * m;
            st->list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS, m, ly,
                            W - 2 * m, lh, ID_LIST);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(st->list, L"Explorer", nullptr);
            LVCOLUMNW c{};
            c.mask = LVCF_WIDTH;
            c.cx = W - 2 * m - GetSystemMetrics(SM_CXVSCROLL) - 4;
            ListView_InsertColumn(st->list, 0, &c);
            make(0, WC_BUTTONW, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, W - 2 * bw - 2 * m, H - m - bh, bw, bh, IDOK);
            make(0, WC_BUTTONW, L"Annuler", WS_TABSTOP, W - bw - m, H - m - bh, bw, bh, IDCANCEL);
            FillChoices(*st);
            // Entièrement visible sur l'écran de la fenêtre (centrée sur elle, elle peut déborder).
            RECT wr;
            GetWindowRect(dlg, &wr);
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi)) {
                const RECT& wa = mi.rcWork;
                const int w = wr.right - wr.left, h = std::min<int>(wr.bottom - wr.top, wa.bottom - wa.top);
                const int x = std::clamp<int>(wr.left, wa.left, std::max<int>(wa.left, wa.right - w));
                const int y = std::clamp<int>(wr.top, wa.top, std::max<int>(wa.top, wa.bottom - h));
                SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            SetFocus(st->edit);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                // Nom d'abord ; les colonnes gardées restent à leur place ; les nouvelles à la fin.
                std::vector<ColSpec> out{st->cols->empty() ? Columns_Builtin(COL_NAME) : st->cols->front()};
                auto checked = [&](const ColSpec& c) {
                    for (const auto& i : st->items)
                        if (i.checked && SameCol(i.spec, c)) return true;
                    return false;
                };
                for (size_t k = 1; k < st->cols->size(); ++k)
                    if (checked((*st->cols)[k])) out.push_back((*st->cols)[k]);
                for (const auto& i : st->items)
                    if (i.checked && std::none_of(out.begin(), out.end(), [&](const ColSpec& c) { return SameCol(c, i.spec); }))
                        out.push_back(i.spec);
                *st->cols = std::move(out);
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(wp) == ID_FILTER && HIWORD(wp) == EN_CHANGE) FillChoices(*st);
            break;
        case WM_NOTIFY: {
            auto* h = (NMHDR*)lp;
            if (st && h->hwndFrom == st->list && h->code == LVN_ITEMCHANGED && !st->filling) {
                auto* nm = (NMLISTVIEW*)lp;
                if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
                    LVITEMW it{};
                    it.mask = LVIF_PARAM;
                    it.iItem = nm->iItem;
                    if (ListView_GetItem(st->list, &it) && (size_t)it.lParam < st->items.size())
                        st->items[it.lParam].checked = ListView_GetCheckState(st->list, nm->iItem) != 0;
                }
            }
            break;
        }
    }
    return FALSE;
}

}  // namespace

bool Columns_ChooseDialog(HWND owner, std::vector<ColSpec>& cols) {
    ChooseState st;
    st.cols = &cols;
    // Colonnes affichées (dans leur ordre), puis les autres colonnes intégrées, puis toutes les propriétés.
    auto add = [&](const ColSpec& c, bool checked) {
        if (c.key == COL_NAME || c.title.empty()) return;
        for (const auto& i : st.items)
            if (SameCol(i.spec, c) || i.spec.title == c.title) return;
        st.items.push_back({c, checked});
    };
    for (const auto& c : cols) add(c, true);
    for (int k : {COL_SIZE, COL_TYPE, COL_DATE, COL_CTIME, COL_ATIME, COL_ATTR}) add(Columns_Builtin(k), false);
    std::vector<ColSpec> props;
    IPropertyDescriptionList* pl = nullptr;
    if (SUCCEEDED(PSEnumeratePropertyDescriptions(PDEF_VIEWABLE, IID_PPV_ARGS(&pl)))) {
        UINT n = 0;
        pl->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IPropertyDescription* pd = nullptr;
            if (FAILED(pl->GetAt(i, IID_PPV_ARGS(&pd)))) continue;
            PROPERTYKEY pk;
            PWSTR name = nullptr;  // propriété sans nom affichable : pas proposée
            if (SUCCEEDED(pd->GetPropertyKey(&pk)) && !BuiltinForProp(pk) && SUCCEEDED(pd->GetDisplayName(&name)) && name &&
                *name)
                props.push_back(Columns_Prop(pk));
            CoTaskMemFree(name);
            pd->Release();
        }
        pl->Release();
    }
    std::sort(props.begin(), props.end(), [](const ColSpec& a, const ColSpec& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.title.c_str(), -1, b.title.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    for (const auto& c : props) add(c, false);

    // Modèle de dialogue en mémoire : les contrôles sont créés à WM_INITDIALOG.
    std::vector<WORD> t(sizeof(DLGTEMPLATE) / sizeof(WORD), 0);
    auto* d = (DLGTEMPLATE*)t.data();
    d->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cx = 240;
    d->cy = 260;
    t.push_back(0);  // pas de menu
    t.push_back(0);  // classe par défaut
    for (const wchar_t* s = L"Choisir les colonnes"; ; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    t.push_back(9);
    for (const wchar_t* s = L"Segoe UI"; ; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    return DialogBoxIndirectParamW(nullptr, (DLGTEMPLATE*)t.data(), owner, ChooseProc, (LPARAM)&st) == IDOK;
}

// ───────────────────────────── Lecture des propriétés ─────────────────────────────

namespace {

struct Request {
    uint64_t serial;
    uint32_t idx;
    bool local;
    std::wstring path;
    std::vector<PROPERTYKEY> keys;
};

CRITICAL_SECTION g_cs;
HANDLE g_wake = nullptr;
std::deque<Request> g_queue;  // fin : urgentes (lignes visibles, les plus récentes d'abord) ; début : lecture de fond
std::atomic<uint64_t> g_serial{0};
std::atomic<HWND> g_target{nullptr};
UINT g_msg = 0;

void Read(const Request& r, PropResult& out) {
    const size_t n = r.keys.size();
    out.text.assign(n, {});
    out.num.assign(n, std::numeric_limits<double>::quiet_NaN());
    IPropertyStore* ps = nullptr;
    // Fichier absent du disque (cloud) : seulement ce qui est en cache, jamais de téléchargement.
    const GETPROPERTYSTOREFLAGS fl = r.local ? GPS_BESTEFFORT : GPS_FASTPROPERTIESONLY;
    if (FAILED(SHGetPropertyStoreFromParsingName(r.path.c_str(), nullptr, fl, IID_PPV_ARGS(&ps)))) return;
    for (size_t i = 0; i < n; ++i) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(r.keys[i], &pv)) && pv.vt != VT_EMPTY) {
            PWSTR s = nullptr;
            if (SUCCEEDED(PSFormatForDisplayAlloc(r.keys[i], pv, PDFF_DEFAULT, &s)) && s) {
                out.text[i] = s;
                CoTaskMemFree(s);
            }
            double v = 0;
            if (pv.vt == VT_FILETIME)
                out.num[i] = (double)(((uint64_t)pv.filetime.dwHighDateTime << 32) | pv.filetime.dwLowDateTime);
            else if (pv.vt != VT_LPWSTR && pv.vt != VT_BSTR && !(pv.vt & VT_VECTOR) && SUCCEEDED(PropVariantToDouble(pv, &v)))
                out.num[i] = v;
        }
        PropVariantClear(&pv);
    }
    ps->Release();
}

DWORD WINAPI Worker(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            Request r;
            EnterCriticalSection(&g_cs);
            if (g_queue.empty()) {
                LeaveCriticalSection(&g_cs);
                break;
            }
            r = std::move(g_queue.back());
            g_queue.pop_back();
            LeaveCriticalSection(&g_cs);
            if (r.serial != g_serial.load()) continue;
            auto* res = new PropResult{r.serial, r.idx, r.keys, {}, {}};
            Read(r, *res);
            HWND h = g_target.load();
            if (!h || !PostMessageW(h, g_msg, 0, (LPARAM)res)) delete res;
        }
    }
}

}  // namespace

void Props_Start(HWND target, UINT msg) {
    g_msg = msg;
    g_target.store(target);
    if (g_wake) return;
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < 2; ++i)
        if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) {
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(h);
        }
}

void Props_Stop() { g_target.store(nullptr); }

void Props_SetSerial(uint64_t serial) {
    g_serial.store(serial);
    if (!g_wake) return;
    EnterCriticalSection(&g_cs);
    g_queue.clear();
    LeaveCriticalSection(&g_cs);
}

void Props_Request(uint64_t serial, uint32_t idx, std::wstring path, bool local, std::vector<PROPERTYKEY> keys,
                   bool urgent) {
    if (!g_wake || serial != g_serial.load() || keys.empty()) return;
    EnterCriticalSection(&g_cs);
    Request r{serial, idx, local, std::move(path), std::move(keys)};
    if (urgent) g_queue.push_back(std::move(r));
    else g_queue.push_front(std::move(r));
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
    SetEvent(g_wake);
}
