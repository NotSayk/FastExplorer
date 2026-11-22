#include "copyqueue.h"

#include "fileops.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

enum { ID_LIST = 1, ID_PAUSE, ID_NOW, ID_CANCEL };
constexpr UINT_PTR TIMER_REFRESH = 1;

HWND g_wnd, g_list, g_btnPause, g_btnNow, g_btnCancel, g_empty;
HFONT g_font;
std::vector<FileOpJob> g_jobs;  // ce qui est affiché, dans l'ordre de la liste

int S(int v) { return MulDiv(v, g_wnd ? (int)GetDpiForWindow(g_wnd) : 96, 96); }

std::wstring Size(uint64_t b) {
    wchar_t s[64];
    StrFormatByteSizeEx(b, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, s, 64);
    return s;
}

std::wstring StateText(const FileOpJob& j) {
    if (!j.running) return j.paused ? L"Retenue" : L"En attente";
    if (j.shell) return L"En cours (fenêtre de Windows)";
    return j.paused ? L"En pause" : L"En cours";
}

std::wstring ProgressText(const FileOpJob& j) {
    if (!j.running || j.shell) return L"";
    if (!j.bytesTotal && !j.filesTotal) return L"Préparation…";
    std::wstring s;
    if (j.bytesTotal) {
        s = std::to_wstring(j.bytesDone * 100 / j.bytesTotal) + L" %  •  " + Size(j.bytesDone) + L" / " + Size(j.bytesTotal);
        const ULONGLONG ms = GetTickCount64() - j.start;
        if (ms > 1000 && !j.paused) s += L"  •  " + Size(j.bytesDone * 1000 / ms) + L"/s";
    } else {
        s = std::to_wstring(j.filesDone) + L" / " + std::to_wstring(j.filesTotal) + L" éléments";
    }
    return s;
}

const FileOpJob* Selected() {
    const int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    return i >= 0 && i < (int)g_jobs.size() ? &g_jobs[i] : nullptr;
}

void UpdateButtons() {
    const FileOpJob* j = Selected();
    EnableWindow(g_btnPause, j && !(j->running && j->shell));
    SetWindowTextW(g_btnPause, j && j->paused ? L"Reprendre" : L"Suspendre");
    EnableWindow(g_btnNow, j && !j->running);
    EnableWindow(g_btnCancel, j && !(j->running && j->shell));
}

void Refresh() {
    std::vector<FileOpJob> jobs = FileOps_Jobs();
    const FileOpJob* sel = Selected();
    const uint64_t selId = sel ? sel->id : 0;
    bool same = jobs.size() == g_jobs.size();
    for (size_t i = 0; same && i < jobs.size(); ++i) same = jobs[i].id == g_jobs[i].id;
    if (!same) {
        SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_list);
        for (size_t i = 0; i < jobs.size(); ++i) {
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.pszText = (LPWSTR)jobs[i].title.c_str();
            ListView_InsertItem(g_list, &it);
        }
        int keep = -1;
        for (size_t i = 0; i < jobs.size(); ++i)
            if (jobs[i].id == selId) keep = (int)i;
        if (keep < 0 && !jobs.empty()) keep = 0;
        if (keep >= 0) ListView_SetItemState(g_list, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    }
    g_jobs = std::move(jobs);
    for (size_t i = 0; i < g_jobs.size(); ++i) {
        const std::wstring st = StateText(g_jobs[i]), pr = ProgressText(g_jobs[i]);
        ListView_SetItemText(g_list, (int)i, 1, (LPWSTR)st.c_str());
        ListView_SetItemText(g_list, (int)i, 2, (LPWSTR)pr.c_str());
    }
    ShowWindow(g_empty, g_jobs.empty() ? SW_SHOW : SW_HIDE);
    UpdateButtons();
}

void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int m = S(10), bh = S(28), bw = S(150);
    const int listB = rc.bottom - m - bh - m;
    MoveWindow(g_list, m, m, rc.right - 2 * m, std::max(0, listB - m), TRUE);
    MoveWindow(g_empty, m + S(10), m + S(40), rc.right - 2 * m - S(20), S(24), TRUE);
    int x = m;
    for (HWND b : {g_btnPause, g_btnNow, g_btnCancel}) {
        MoveWindow(b, x, listB + m, bw, bh, TRUE);
        x += bw + S(8);
    }
    const int w = rc.right - 2 * m - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(g_wnd)) - S(4);
    ListView_SetColumnWidth(g_list, 0, std::max(S(120), w - S(130) - S(230)));
    ListView_SetColumnWidth(g_list, 1, S(130));
    ListView_SetColumnWidth(g_list, 2, S(230));
}

void ApplyFont() {
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, GetDpiForWindow(g_wnd));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {g_list, g_btnPause, g_btnNow, g_btnCancel, g_empty}) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_wnd = h;
            HINSTANCE inst = ((CREATESTRUCTW*)lp)->hInstance;
            g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                                         LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                     0, 0, 0, 0, h, (HMENU)ID_LIST, inst, nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowTheme(g_list, L"Explorer", nullptr);
            const wchar_t* titles[] = {L"Opération", L"État", L"Progression"};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = (LPWSTR)titles[i];
                c.cx = 100;
                c.iSubItem = i;
                ListView_InsertColumn(g_list, i, &c);
            }
            auto btn = [&](const wchar_t* t, int id) {
                return CreateWindowExW(0, WC_BUTTONW, t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, h,
                                       (HMENU)(INT_PTR)id, inst, nullptr);
            };
            g_btnPause = btn(L"Suspendre", ID_PAUSE);
            g_btnNow = btn(L"Démarrer maintenant", ID_NOW);
            g_btnCancel = btn(L"Annuler", ID_CANCEL);
            g_empty = CreateWindowExW(0, WC_STATICW, L"Aucune copie en cours ni en attente.", WS_CHILD | SS_CENTER, 0, 0,
                                      0, 0, h, nullptr, inst, nullptr);
            ApplyFont();
            Refresh();
            SetTimer(h, TIMER_REFRESH, 300, nullptr);
            return 0;
        }
        case WM_SIZE: Layout(); return 0;
        case WM_DPICHANGED: {
            const RECT* r = (const RECT*)lp;
            ApplyFont();
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_TIMER: Refresh(); return 0;
        case WM_NOTIFY:
            if (((NMHDR*)lp)->code == LVN_ITEMCHANGED) UpdateButtons();
            return 0;
        case WM_COMMAND: {
            const FileOpJob* j = Selected();
            if (!j) return 0;
            switch (LOWORD(wp)) {
                case ID_PAUSE: FileOps_Pause(j->id, !j->paused); break;
                case ID_NOW: FileOps_StartNow(j->id); break;
                case ID_CANCEL: FileOps_CancelJob(j->id); break;
                default: return 0;
            }
            Refresh();
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_REFRESH);
            if (g_font) DeleteObject(g_font);
            g_font = nullptr;
            g_wnd = nullptr;
            g_jobs.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool CopyQueue_Visible() { return g_wnd && IsWindowVisible(g_wnd); }

void CopyQueue_Show(HWND owner, bool activate) {
    if (g_wnd) {
        if (activate) {
            ShowWindow(g_wnd, SW_SHOWNORMAL);
            SetForegroundWindow(g_wnd);
        } else if (!IsWindowVisible(g_wnd) || IsIconic(g_wnd)) {
            ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        }
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.hIcon = (HICON)SendMessageW(owner, WM_GETICON, ICON_SMALL, 0);
        wc.lpszClassName = L"FastExplorerCopyQueue";
        registered = RegisterClassW(&wc) != 0;
    }
    // En bas à droite de la fenêtre principale, sur son écran.
    const int dpi = (int)GetDpiForWindow(owner);
    const int w = MulDiv(720, dpi, 96), hgt = MulDiv(300, dpi, 96);
    RECT o;
    GetWindowRect(owner, &o);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = std::clamp((int)o.right - w - MulDiv(24, dpi, 96), (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
    int y = std::clamp((int)o.bottom - hgt - MulDiv(48, dpi, 96), (int)wa.top, std::max((int)wa.top, (int)wa.bottom - hgt));
    CreateWindowExW(0, L"FastExplorerCopyQueue", L"File d'attente des copies",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN, x, y, w, hgt,
                    owner, nullptr, inst, nullptr);
    if (g_wnd) {
        ShowWindow(g_wnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        Layout();
    }
}
