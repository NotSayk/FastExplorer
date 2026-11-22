#include "sessions.h"

#include <algorithm>
#include <cwchar>

namespace {

constexpr const wchar_t* kAppKey = L"Software\\FastExplorer";
constexpr const wchar_t* kSessionsKey = L"Software\\FastExplorer\\Sessions";
constexpr const wchar_t* kBookmarksKey = L"Software\\FastExplorer\\Bookmarks";

bool WriteMulti(const wchar_t* key, const wchar_t* name, const std::wstring& multi) {
    const std::wstring data = multi + L'\0';  // zéro final de la liste
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_MULTI_SZ, data.c_str(),
                           (DWORD)(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool ReadValue(const wchar_t* key, const wchar_t* name, DWORD type, std::wstring& out) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, type == REG_MULTI_SZ ? RRF_RT_REG_MULTI_SZ : RRF_RT_REG_SZ, nullptr,
                     nullptr, &bytes) != ERROR_SUCCESS)
        return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    bytes = (DWORD)(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, type == REG_MULTI_SZ ? RRF_RT_REG_MULTI_SZ : RRF_RT_REG_SZ, nullptr,
                     buf.data(), &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(bytes / sizeof(wchar_t));
    while (!buf.empty() && buf.back() == L'\0' && (type != REG_MULTI_SZ || buf.size() >= 2 && buf[buf.size() - 2] == L'\0'))
        buf.pop_back();  // REG_SZ : sans son zéro ; REG_MULTI_SZ : chaque chaîne garde le sien
    out = std::move(buf);
    return true;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::wstring Session_Encode(const SessionTabs& s) {
    std::wstring m = std::to_wstring(s.active) + L',' + std::to_wstring(s.other);
    m += L'\0';
    for (const auto& p : s.paths) {
        m += L'>';
        m += p;
        m += L'\0';
    }
    return m;
}

bool Session_Decode(const std::wstring& multi, SessionTabs& s) {
    s = SessionTabs{};
    std::vector<std::wstring> parts;
    for (size_t i = 0; i < multi.size();) {
        const size_t z = multi.find(L'\0', i);
        const size_t end = z == std::wstring::npos ? multi.size() : z;
        parts.push_back(multi.substr(i, end - i));
        i = end + 1;
    }
    if (parts.size() < 2) return false;
    int a = 0, o = -1;
    if (swscanf_s(parts[0].c_str(), L"%d,%d", &a, &o) != 2) return false;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].empty() || parts[i][0] != L'>') return false;
        s.paths.push_back(parts[i].substr(1));
    }
    const int n = (int)s.paths.size();
    s.active = a >= 0 && a < n ? a : 0;
    s.other = o >= 0 && o < n && o != s.active ? o : -1;
    return true;
}

bool Session_SaveLast(const SessionTabs& s) {
    if (s.paths.empty()) return false;
    return WriteMulti(kAppKey, L"LastTabs", Session_Encode(s));
}

bool Session_LoadLast(SessionTabs& s) {
    std::wstring m;
    return ReadValue(kAppKey, L"LastTabs", REG_MULTI_SZ, m) && Session_Decode(m, s);
}

bool Session_RestoreEnabled() {
    DWORD v = 1, bytes = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"RestoreTabs", RRF_RT_REG_DWORD, nullptr, &v, &bytes) != ERROR_SUCCESS)
        return true;
    return v != 0;
}

void Session_SetRestore(bool on) {
    const DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"RestoreTabs", REG_DWORD, &v, sizeof v);
}

std::vector<std::wstring> Session_Names() {
    std::vector<std::wstring> v;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSessionsKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return v;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD len = 512, type = 0;
        const LONG r = RegEnumValueW(k, i, name, &len, nullptr, &type, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && type == REG_MULTI_SZ && len) v.emplace_back(name, len);
    }
    RegCloseKey(k);
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), -1,
                               b.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return v;
}

bool Session_Save(const std::wstring& name, const SessionTabs& s) {
    const std::wstring n = Trim(name);
    if (n.empty() || n.size() >= 256 || s.paths.empty()) return false;
    return WriteMulti(kSessionsKey, n.c_str(), Session_Encode(s));
}

bool Session_Load(const std::wstring& name, SessionTabs& s) {
    const std::wstring n = Trim(name);
    std::wstring m;
    return !n.empty() && ReadValue(kSessionsKey, n.c_str(), REG_MULTI_SZ, m) && Session_Decode(m, s);
}

bool Session_Delete(const std::wstring& name) {
    const std::wstring n = Trim(name);
    return !n.empty() && RegDeleteKeyValueW(HKEY_CURRENT_USER, kSessionsKey, n.c_str()) == ERROR_SUCCESS;
}

bool Bookmark_Get(int n, std::wstring& path) {
    if (n < 1 || n > kBookmarks) return false;
    std::wstring v;
    if (!ReadValue(kBookmarksKey, std::to_wstring(n).c_str(), REG_SZ, v) || v.empty() || v[0] != L'>') return false;
    path = v.substr(1);
    return true;
}

bool Bookmark_Set(int n, const std::wstring& path) {
    if (n < 1 || n > kBookmarks) return false;
    const std::wstring v = L">" + path;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kBookmarksKey, std::to_wstring(n).c_str(), REG_SZ, v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool Bookmark_Clear(int n) {
    if (n < 1 || n > kBookmarks) return false;
    const LONG r = RegDeleteKeyValueW(HKEY_CURRENT_USER, kBookmarksKey, std::to_wstring(n).c_str());
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

// ───────────────────────────── Recherches récentes ─────────────────────────────

std::vector<std::wstring> HistoryPush(std::vector<std::wstring> list, const std::wstring& query, size_t max) {
    const std::wstring q = Trim(query);
    if (q.empty()) return list;
    std::erase_if(list, [&](const std::wstring& s) { return _wcsicmp(s.c_str(), q.c_str()) == 0; });
    list.insert(list.begin(), q);
    if (list.size() > max) list.resize(max);
    return list;
}

namespace {
void WriteHistory(const std::vector<std::wstring>& v) {
    if (v.empty()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kAppKey, L"SearchHistory");
        return;
    }
    std::wstring multi;
    for (const auto& s : v) multi += s + L'\0';
    WriteMulti(kAppKey, L"SearchHistory", multi);
}
}  // namespace

std::vector<std::wstring> SearchHistory_Get() {
    std::vector<std::wstring> v;
    std::wstring m;
    if (!ReadValue(kAppKey, L"SearchHistory", REG_MULTI_SZ, m)) return v;
    for (size_t a = 0; a < m.size();) {
        size_t b = m.find(L'\0', a);
        if (b == std::wstring::npos) b = m.size();
        if (b > a) v.emplace_back(m, a, b - a);
        a = b + 1;
    }
    return v;
}

void SearchHistory_Add(const std::wstring& query) { WriteHistory(HistoryPush(SearchHistory_Get(), query)); }

void SearchHistory_Remove(const std::wstring& query) {
    auto v = SearchHistory_Get();
    std::erase_if(v, [&](const std::wstring& s) { return _wcsicmp(s.c_str(), query.c_str()) == 0; });
    WriteHistory(v);
}

void SearchHistory_Clear() { WriteHistory({}); }

// ───────────────────────────── Boîte de saisie ─────────────────────────────

namespace {

enum { ID_PROMPT = 100, ID_TEXT };

struct AskState {
    std::wstring prompt;
    std::wstring* text;
};

INT_PTR CALLBACK AskProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (AskState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG: {
            st = (AskState*)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            const HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                            DWORD ex = 0) {
                RECT r{x, y, x + w, y + h};
                MapDialogRect(dlg, &r);
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                                         r.bottom - r.top, dlg, (HMENU)(INT_PTR)id, nullptr, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
                return c;
            };
            make(L"STATIC", st->prompt.c_str(), SS_LEFT | SS_NOPREFIX, ID_PROMPT, 7, 7, 206, 10);
            HWND ed = make(L"EDIT", st->text->c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, ID_TEXT, 7, 19, 206, 13);
            SendMessageW(ed, EM_LIMITTEXT, 200, 0);
            make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 109, 40, 50, 14);
            make(L"BUTTON", L"Annuler", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 163, 40, 50, 14);
            SetFocus(ed);
            SendMessageW(ed, EM_SETSEL, 0, -1);
            return FALSE;  // focus déjà placé
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                HWND ed = GetDlgItem(dlg, ID_TEXT);
                std::wstring t(GetWindowTextLengthW(ed) + 1, L'\0');
                t.resize(GetWindowTextW(ed, t.data(), (int)t.size()));
                *st->text = Trim(t);
                EndDialog(dlg, st->text->empty() ? IDCANCEL : IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

bool AskText(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& text) {
    AskState st{prompt, &text};
    std::vector<WORD> t(sizeof(DLGTEMPLATE) / sizeof(WORD), 0);
    auto* d = (DLGTEMPLATE*)t.data();
    d->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cx = 220;
    d->cy = 62;
    t.push_back(0);  // pas de menu
    t.push_back(0);  // classe par défaut
    for (const wchar_t* s = title.c_str();; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    t.push_back(9);
    for (const wchar_t* s = L"Segoe UI";; ++s) {
        t.push_back(*s);
        if (!*s) break;
    }
    return DialogBoxIndirectParamW(nullptr, (DLGTEMPLATE*)t.data(), owner, AskProc, (LPARAM)&st) == IDOK;
}
