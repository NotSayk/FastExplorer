#include "palette.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

namespace {

constexpr wchar_t kKeysKey[] = L"Software\\FastExplorer\\Keys";

int Px(HWND h, int v) { return MulDiv(v, (int)GetDpiForWindow(h), 96); }

// Position de `word` dans `text` (sans casse ni accents), -1 si absent.
int FindWord(const std::wstring& text, const std::wstring& word) {
    int found = 0;
    const int at = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                                   text.c_str(), (int)text.size(), word.c_str(), (int)word.size(), &found, nullptr, nullptr, 0);
    return at;
}

std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_RETURN: return L"Entrée";
        case VK_DELETE: return L"Suppr";
        case VK_BACK: return L"Retour arrière";
        case VK_SPACE: return L"Espace";
        case VK_ESCAPE: return L"Échap";
        case VK_TAB: return L"Tab";
        case VK_LEFT: return L"←";
        case VK_RIGHT: return L"→";
        case VK_UP: return L"↑";
        case VK_DOWN: return L"↓";
        case VK_PRIOR: return L"Page préc.";
        case VK_NEXT: return L"Page suiv.";
        case VK_HOME: return L"Début";
        case VK_END: return L"Fin";
        case VK_INSERT: return L"Inser";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Pavé " + std::to_wstring(vk - VK_NUMPAD0);
    // Autres touches : le caractère de la disposition du clavier (« ù », « ² »…).
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
    if (ch > 32) return std::wstring(1, (wchar_t)towupper((wint_t)ch));
    wchar_t name[64] = {};
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 64) > 0) return name;
    return L"#" + std::to_wstring(vk);
}

// ───────────────────────────── Palette ─────────────────────────────

struct PaletteState {
    HWND wnd = nullptr, edit = nullptr, list = nullptr;
    std::vector<PaletteItem> items;
    std::map<std::wstring, KeyChord> keys;
    std::vector<size_t> shown;  // index dans items, dans l'ordre affiché
    int chosen = -1;
    bool done = false;
    HFONT font = nullptr;
};
PaletteState* g_pal = nullptr;

std::wstring KeysOf(const PaletteState& s, const PaletteItem& it) {
    if (auto k = s.keys.find(it.id); k != s.keys.end() && k->second) return Chord_Text(k->second);
    return it.keys;
}

void Filter(PaletteState& s) {
    wchar_t buf[256] = {};
    GetWindowTextW(s.edit, buf, 256);
    const std::wstring q = buf;
    std::vector<std::pair<int, size_t>> scored;
    for (size_t i = 0; i < s.items.size(); ++i)
        if (const int sc = Palette_Score(q, s.items[i].id); sc >= 0) scored.push_back({sc, i});
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    s.shown.clear();
    for (const auto& x : scored) s.shown.push_back(x.second);
    SendMessageW(s.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s.list, LB_RESETCONTENT, 0, 0);
    for (size_t k = 0; k < s.shown.size(); ++k) SendMessageW(s.list, LB_ADDSTRING, 0, (LPARAM)L"");
    // Première commande disponible sélectionnée.
    int first = 0;
    while (first < (int)s.shown.size() && s.items[s.shown[first]].disabled) ++first;
    SendMessageW(s.list, LB_SETCURSEL, first < (int)s.shown.size() ? first : (s.shown.empty() ? -1 : 0), 0);
    SendMessageW(s.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s.list, nullptr, TRUE);
}

void Choose(PaletteState& s) {
    const int sel = (int)SendMessageW(s.list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s.shown.size() || s.items[s.shown[sel]].disabled) return;
    s.chosen = (int)s.shown[sel];
    s.done = true;
}

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && g_pal) {
        PaletteState& s = *g_pal;
        const int n = (int)s.shown.size(), cur = (int)SendMessageW(s.list, LB_GETCURSEL, 0, 0);
        switch (wp) {
            case VK_DOWN:
            case VK_UP:
            case VK_NEXT:
            case VK_PRIOR: {
                if (!n) return 0;
                const int step = wp == VK_DOWN ? 1 : wp == VK_UP ? -1 : wp == VK_NEXT ? 8 : -8;
                SendMessageW(s.list, LB_SETCURSEL, std::clamp(cur + step, 0, n - 1), 0);
                return 0;
            }
            case VK_RETURN: Choose(s); return 0;
            case VK_ESCAPE: s.done = true; return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // pas de bip
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK PaletteProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PaletteState* s = g_pal;
    switch (msg) {
        case WM_COMMAND:
            if (s && (HWND)lp == s->edit && HIWORD(wp) == EN_CHANGE) Filter(*s);
            if (s && (HWND)lp == s->list && HIWORD(wp) == LBN_DBLCLK) Choose(*s);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && s) s->done = true;  // clic ailleurs : fermée
            return 0;
        case WM_MEASUREITEM: {
            auto* mi = (MEASUREITEMSTRUCT*)lp;
            mi->itemHeight = Px(h, 26);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* di = (DRAWITEMSTRUCT*)lp;
            if (!s || di->itemID == (UINT)-1 || di->itemID >= s->shown.size()) return TRUE;
            const PaletteItem& it = s->items[s->shown[di->itemID]];
            const bool sel = di->itemState & ODS_SELECTED;
            FillRect(di->hDC, &di->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
            SetBkMode(di->hDC, TRANSPARENT);
            const COLORREF fg = GetSysColor(it.disabled ? COLOR_GRAYTEXT : sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
            SetTextColor(di->hDC, fg);
            HGDIOBJ old = SelectObject(di->hDC, s->font);
            RECT r = di->rcItem;
            r.left += Px(h, 10);
            r.right -= Px(h, 10);
            const std::wstring text = (it.checked ? L"✓ " : L"") + it.id;
            const std::wstring keys = KeysOf(*s, it);
            if (!keys.empty()) {
                SetTextColor(di->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_GRAYTEXT));
                DrawTextW(di->hDC, keys.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(di->hDC, fg);
                SIZE ks{};
                GetTextExtentPoint32W(di->hDC, keys.c_str(), (int)keys.size(), &ks);
                r.right -= ks.cx + Px(h, 16);
            }
            DrawTextW(di->hDC, text.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(di->hDC, old);
            return TRUE;
        }
        case WM_CLOSE:
            if (s) s->done = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ───────────────────────────── Raccourcis ─────────────────────────────

struct KeysState {
    HWND wnd = nullptr, filter = nullptr, list = nullptr, hot = nullptr, note = nullptr;
    const std::vector<PaletteItem>* items = nullptr;
    std::map<std::wstring, KeyChord>* keys = nullptr;
    std::vector<size_t> shown;
    bool done = false, changed = false;
};
KeysState* g_keys = nullptr;

enum { ID_FILTER = 101, ID_LIST, ID_HOT, ID_SET, ID_CLEAR, ID_CLOSE, ID_NOTE };

void KeysFill(KeysState& s) {
    wchar_t buf[256] = {};
    GetWindowTextW(s.filter, buf, 256);
    const int keep = ListView_GetNextItem(s.list, -1, LVNI_SELECTED);
    const std::wstring keepId = keep >= 0 && keep < (int)s.shown.size() ? (*s.items)[s.shown[keep]].id : L"";
    s.shown.clear();
    for (size_t i = 0; i < s.items->size(); ++i)
        if (Palette_Score(buf, (*s.items)[i].id) >= 0) s.shown.push_back(i);
    ListView_DeleteAllItems(s.list);
    for (size_t k = 0; k < s.shown.size(); ++k) {
        const PaletteItem& it = (*s.items)[s.shown[k]];
        LVITEMW li{LVIF_TEXT, (int)k};
        li.pszText = (LPWSTR)it.id.c_str();
        ListView_InsertItem(s.list, &li);
        ListView_SetItemText(s.list, (int)k, 1, (LPWSTR)it.keys.c_str());
        const auto c = s.keys->find(it.id);
        std::wstring mine = c != s.keys->end() ? Chord_Text(c->second) : L"";
        ListView_SetItemText(s.list, (int)k, 2, mine.data());
        if (it.id == keepId) ListView_SetItemState(s.list, (int)k, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

int KeysCurrent(KeysState& s) {
    const int k = ListView_GetNextItem(s.list, -1, LVNI_SELECTED);
    return k >= 0 && k < (int)s.shown.size() ? (int)s.shown[k] : -1;
}

void KeysSet(KeysState& s, bool clear) {
    const int i = KeysCurrent(s);
    if (i < 0) {
        SetWindowTextW(s.note, L"Choisissez d'abord une commande dans la liste.");
        return;
    }
    const std::wstring& id = (*s.items)[i].id;
    if (clear) {
        s.keys->erase(id);
        SetWindowTextW(s.note, (L"Raccourci retiré : " + id).c_str());
    } else {
        const WORD hk = (WORD)SendMessageW(s.hot, HKM_GETHOTKEY, 0, 0);
        const UINT vk = LOBYTE(hk), mods = HIBYTE(hk);
        if (!vk) {
            SetWindowTextW(s.note, L"Tapez d'abord la combinaison dans la zone « Nouveau raccourci ».");
            return;
        }
        const KeyChord c = Chord_Make(vk, mods & HOTKEYF_CONTROL, mods & HOTKEYF_SHIFT, mods & HOTKEYF_ALT);
        std::wstring note = L"« " + Chord_Text(c) + L" » : " + id;
        for (auto it = s.keys->begin(); it != s.keys->end();)  // une combinaison ne sert qu'une commande
            if (it->second == c && it->first != id) {
                note += L"  (retiré de « " + it->first + L" »)";
                it = s.keys->erase(it);
            } else {
                ++it;
            }
        for (const auto& other : *s.items)  // raccourci par défaut d'une autre commande : il est remplacé
            if (other.id != id && !other.keys.empty() && other.keys == Chord_Text(c)) {
                note += L"  (remplace celui de « " + other.id + L" »)";
                break;
            }
        (*s.keys)[id] = c;
        SetWindowTextW(s.note, note.c_str());
    }
    s.changed = true;
    KeysFill(s);
}

LRESULT CALLBACK KeysProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    KeysState* s = g_keys;
    switch (msg) {
        case WM_COMMAND:
            if (!s) return 0;
            if (LOWORD(wp) == ID_FILTER && HIWORD(wp) == EN_CHANGE) KeysFill(*s);
            if (LOWORD(wp) == ID_SET) KeysSet(*s, false);
            if (LOWORD(wp) == ID_CLEAR) KeysSet(*s, true);
            if (LOWORD(wp) == ID_CLOSE || LOWORD(wp) == IDCANCEL) s->done = true;
            return 0;
        case WM_CLOSE:
            if (s) s->done = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RegisterClassOnce(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof wc};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), name, &wc)) return;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

}  // namespace

KeyChord Chord_Make(UINT vk, bool ctrl, bool shift, bool alt) {
    if (!vk) return 0;
    return (vk & 0xFFFF) | (ctrl ? kChordCtrl : 0) | (shift ? kChordShift : 0) | (alt ? kChordAlt : 0);
}

KeyChord Chord_FromKey(UINT vk) {
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 0;
    return Chord_Make(vk, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0);
}

std::wstring Chord_Text(KeyChord c) {
    if (!(c & 0xFFFF)) return L"";
    std::wstring s;
    if (c & kChordCtrl) s += L"Ctrl+";
    if (c & kChordShift) s += L"Maj+";
    if (c & kChordAlt) s += L"Alt+";
    return s + KeyName(c & 0xFFFF);
}

std::map<std::wstring, KeyChord> Keys_Load() {
    std::map<std::wstring, KeyChord> out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeysKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD n = 1024, type = 0, v = 0, cb = sizeof v;
        const LSTATUS r = RegEnumValueW(k, i, name, &n, nullptr, &type, (BYTE*)&v, &cb);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && type == REG_DWORD && (v & 0xFFFF)) out[name] = v;
    }
    RegCloseKey(k);
    return out;
}

void Keys_Save(const std::map<std::wstring, KeyChord>& keys) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kKeysKey);
    if (keys.empty()) return;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeysKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    for (const auto& [id, c] : keys) {
        const DWORD v = c;
        RegSetValueExW(k, id.c_str(), 0, REG_DWORD, (const BYTE*)&v, sizeof v);
    }
    RegCloseKey(k);
}

int Palette_Score(const std::wstring& query, const std::wstring& text) {
    int score = 0;
    size_t i = 0;
    bool any = false;
    while (i < query.size()) {
        while (i < query.size() && iswspace(query[i])) ++i;
        size_t j = i;
        while (j < query.size() && !iswspace(query[j])) ++j;
        if (j > i) {
            any = true;
            const std::wstring w = query.substr(i, j - i);
            const int at = FindWord(text, w);
            if (at < 0) return -1;
            // Début d'un mot du texte : bien plus pertinent (« dét » → « Détails » avant « … détecté »).
            const bool start = at == 0 || !iswalnum(text[at - 1]);
            score += (start ? 0 : 1000) + at;
        }
        i = j;
    }
    return any ? score : 0;
}

void Palette_Show(HWND owner, const RECT& anchor, HFONT font, std::vector<PaletteItem> items,
                  const std::map<std::wstring, KeyChord>& keys) {
    if (g_pal) return;
    RegisterClassOnce(L"FastExplorerPalette", PaletteProc);
    PaletteState s;
    s.items = std::move(items);
    s.keys = keys;
    s.font = font;
    g_pal = &s;
    const int w = std::max<int>(Px(owner, 560), (anchor.right - anchor.left) * 2 / 3);
    const int x = (anchor.left + anchor.right) / 2 - w / 2, y = anchor.top;
    const int rowH = Px(owner, 26), editH = Px(owner, 30), rows = 14;
    const int h = editH + rows * rowH + Px(owner, 12);
    s.wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"FastExplorerPalette", L"Palette de commandes", WS_POPUP | WS_BORDER, x, y,
                            w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, Px(owner, 4), Px(owner, 4),
                             w - Px(owner, 10), editH, s.wnd, nullptr, nullptr, nullptr);
    s.list = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             Px(owner, 4), editH + Px(owner, 8), w - Px(owner, 10), rows * rowH, s.wnd, nullptr, nullptr,
                             nullptr);
    SendMessageW(s.edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(s.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Tapez une commande (Entrée : exécuter, Échap : fermer)");
    SetWindowSubclass(s.edit, EditProc, 1, 0);
    Filter(s);
    ShowWindow(s.wnd, SW_SHOW);
    SetForegroundWindow(s.wnd);
    SetFocus(s.edit);
    MSG m{};
    while (!s.done && GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_KEYDOWN && m.hwnd == s.list && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)) {
            if (m.wParam == VK_RETURN) Choose(s); else s.done = true;
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (m.message == WM_QUIT) PostQuitMessage((int)m.wParam);
    g_pal = nullptr;
    DestroyWindow(s.wnd);
    SetForegroundWindow(owner);
    if (s.chosen >= 0 && s.items[s.chosen].run) s.items[s.chosen].run();
}

bool Keys_Edit(HWND owner, HFONT font, const std::vector<PaletteItem>& items, std::map<std::wstring, KeyChord>& keys) {
    if (g_keys) return false;
    RegisterClassOnce(L"FastExplorerKeys", KeysProc);
    KeysState s;
    s.items = &items;
    s.keys = &keys;
    g_keys = &s;
    RECT ow;
    GetWindowRect(owner, &ow);
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    const int w = std::min<int>(Px(owner, 760), mi.rcWork.right - mi.rcWork.left - Px(owner, 20));
    const int h = std::min<int>(Px(owner, 560), mi.rcWork.bottom - mi.rcWork.top - Px(owner, 20));
    int x = (ow.left + ow.right) / 2 - w / 2, y = (ow.top + ow.bottom) / 2 - h / 2;
    x = std::clamp<int>(x, mi.rcWork.left, mi.rcWork.right - w);
    y = std::clamp<int>(y, mi.rcWork.top, mi.rcWork.bottom - h);
    s.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastExplorerKeys", L"Raccourcis clavier",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT rc;
    GetClientRect(s.wnd, &rc);
    const int m = Px(owner, 10), lineH = Px(owner, 26), btnW = Px(owner, 110);
    const int cw = rc.right - 2 * m;
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int px, int py, int pw, int ph, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px, py, pw, ph, s.wnd, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    s.filter = make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, ID_FILTER, m, m, cw, lineH);
    SendMessageW(s.filter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filtrer les commandes");
    const int bottom = rc.bottom - m - lineH;  // ligne des boutons
    const int listH = bottom - 2 * lineH - 3 * m - (m + lineH);
    s.list = make(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, ID_LIST, m,
                  m + lineH + m, cw, listH);
    ListView_SetExtendedListViewStyle(s.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH};
    const wchar_t* titles[] = {L"Commande", L"Par défaut", L"Le vôtre"};
    const int widths[] = {cw * 56 / 100, cw * 20 / 100, cw * 20 / 100};
    for (int i = 0; i < 3; ++i) {
        col.pszText = (LPWSTR)titles[i];
        col.cx = widths[i];
        ListView_InsertColumn(s.list, i, &col);
    }
    const int rowY = m + lineH + m + listH + m;
    make(L"STATIC", L"Nouveau raccourci :", SS_CENTERIMAGE, 0, m, rowY, Px(owner, 140), lineH);
    s.hot = make(HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, ID_HOT, m + Px(owner, 145), rowY, Px(owner, 200), lineH);
    make(L"BUTTON", L"Attribuer", WS_TABSTOP | BS_PUSHBUTTON, ID_SET, m + Px(owner, 355), rowY, btnW, lineH);
    make(L"BUTTON", L"Retirer", WS_TABSTOP | BS_PUSHBUTTON, ID_CLEAR, m + Px(owner, 355) + btnW + m, rowY, btnW, lineH);
    s.note = make(L"STATIC", L"Choisissez une commande, tapez la combinaison puis « Attribuer ». Vos raccourcis passent avant ceux par défaut.",
                  SS_ENDELLIPSIS, ID_NOTE, m, rowY + lineH + m, cw, lineH);
    make(L"BUTTON", L"Fermer", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_CLOSE, rc.right - m - btnW, bottom, btnW, lineH);
    KeysFill(s);
    EnableWindow(owner, FALSE);
    ShowWindow(s.wnd, SW_SHOW);
    SetFocus(s.filter);
    MSG msg{};
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && msg.hwnd != s.hot) {
            s.done = true;
            continue;
        }
        if (IsDialogMessageW(s.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage((int)msg.wParam);
    EnableWindow(owner, TRUE);
    DestroyWindow(s.wnd);
    SetForegroundWindow(owner);
    g_keys = nullptr;
    if (s.changed) Keys_Save(keys);
    return s.changed;
}
