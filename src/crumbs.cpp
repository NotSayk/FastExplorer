#include "crumbs.h"

#include "navpane.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <thread>

std::vector<Crumb> Crumbs_Split(const std::wstring& path,
                                const std::function<std::wstring(const std::wstring&)>& driveText) {
    std::vector<Crumb> out;
    std::wstring p = path;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.empty()) return {{L"Ce PC", L""}};

    std::wstring cur, rest;
    if (p.size() >= 2 && p[1] == L':') {  // C:\…
        std::wstring root = p.substr(0, 2) + L"\\";
        root[0] = (wchar_t)towupper(root[0]);
        std::wstring text = driveText ? driveText(root) : std::wstring();
        if (text.empty()) text = root.substr(0, 2);
        out.push_back({L"Ce PC", L""});
        out.push_back({text, root});
        cur = root;
        rest = p.size() > 3 ? p.substr(3) : std::wstring();
    } else if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\') {  // \\serveur\partage\…
        const size_t server = p.find(L'\\', 2);
        const size_t share = server == std::wstring::npos ? server : p.find(L'\\', server + 1);
        cur = p.substr(0, share);
        out.push_back({cur, cur});
        rest = share == std::wstring::npos ? std::wstring() : p.substr(share + 1);
    } else {
        return {{p, p}};
    }
    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t end = rest.find(L'\\', pos);
        if (end == std::wstring::npos) end = rest.size();
        if (end > pos) {
            const std::wstring part = rest.substr(pos, end - pos);
            if (cur.back() != L'\\') cur += L'\\';
            cur += part;
            out.push_back({part, cur});
        }
        pos = end + 1;
    }
    return out;
}

namespace {

constexpr UINT WM_CRUMB_MENU = WM_APP + 1;  // sous-dossiers lus : afficher le menu
// Sonde (tests à la souris) : wp = n° de partie ; renvoie -1 au-delà, sinon
// type (bits 40+), segment (32+), centre x (16+) et y (0+) en coordonnées client.
constexpr UINT WM_CRUMB_PROBE = WM_APP + 2;
constexpr UINT kMaxMenuItems = 1000;

enum class Part { Text, Chevron, Overflow };

struct Hit {
    RECT rc;
    Part part;
    int index;  // segment (Overflow : -1)
};

struct MenuResult {
    unsigned serial;
    int index;
    std::vector<NavFolder> folders;
};

HWND g_wnd = nullptr;
CrumbHost g_host;
HFONT g_font = nullptr;
std::wstring g_path;
std::vector<Crumb> g_crumbs;
std::vector<Hit> g_hits;
int g_firstShown = 0;     // segments avant : dans le menu « « »
int g_hot = -1;           // partie survolée (index dans g_hits)
int g_pressed = -1;       // partie enfoncée (clic en cours, menu ouvert)
bool g_tracking = false;  // TrackMouseEvent armé
unsigned g_menuSerial = 0;

int FontHeight(HDC dc) {
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    return std::max(8, (int)tm.tmHeight);
}

// Place les segments de droite à gauche : les derniers (les plus proches du
// dossier ouvert) restent visibles, les premiers passent dans le menu « « ».
void LayoutParts() {
    g_hits.clear();
    g_firstShown = 0;
    if (!g_wnd) return;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    HDC dc = GetDC(g_wnd);
    HGDIOBJ old = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
    const int fh = FontHeight(dc);
    const int pad = fh / 2, chev = fh, over = fh + fh / 2;
    std::vector<int> textW(g_crumbs.size());
    int total = 0;
    for (size_t i = 0; i < g_crumbs.size(); ++i) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, g_crumbs[i].text.c_str(), (int)g_crumbs[i].text.size(), &sz);
        textW[i] = sz.cx + 2 * pad;
        total += textW[i] + chev;
    }
    SelectObject(dc, old);
    ReleaseDC(g_wnd, dc);

    const int avail = std::max(0, (int)rc.right - fh * 2);  // garde une marge cliquable pour la saisie
    int first = 0;
    while (first + 1 < (int)g_crumbs.size() && total + (first ? over : 0) > avail) {
        total -= textW[first] + chev;
        ++first;
    }
    g_firstShown = first;
    int x = 0;
    if (first) {
        g_hits.push_back({{x, 0, x + over, rc.bottom}, Part::Overflow, -1});
        x += over;
    }
    for (int i = first; i < (int)g_crumbs.size(); ++i) {
        g_hits.push_back({{x, 0, x + textW[i], rc.bottom}, Part::Text, i});
        x += textW[i];
        g_hits.push_back({{x, 0, x + chev, rc.bottom}, Part::Chevron, i});
        x += chev;
    }
}

int HitTest(POINT pt) {
    for (size_t i = 0; i < g_hits.size(); ++i)
        if (PtInRect(&g_hits[i].rc, pt)) return (int)i;
    return -1;
}

void Chevron(HDC dc, const RECT& r, bool down, COLORREF color) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int s = std::max(2, (int)(r.right - r.left) / 5);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, s / 3), color);
    HGDIOBJ old = SelectObject(dc, pen);
    if (down) {
        POINT p[3] = {{cx - s, cy - s / 2}, {cx, cy + s / 2}, {cx + s, cy - s / 2}};
        Polyline(dc, p, 3);
    } else {
        POINT p[3] = {{cx - s / 2, cy - s}, {cx + s / 2, cy}, {cx - s / 2, cy + s}};
        Polyline(dc, p, 3);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Paint(HDC target, const RECT& rc) {
    // Dessin hors écran : pas de scintillement au survol.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1, (int)rc.right), std::max(1, (int)rc.bottom));
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    HGDIOBJ oldFont = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    const COLORREF text = GetSysColor(COLOR_WINDOWTEXT), gray = GetSysColor(COLOR_GRAYTEXT);
    for (size_t i = 0; i < g_hits.size(); ++i) {
        const Hit& h = g_hits[i];
        RECT r = h.rc;
        InflateRect(&r, 0, -1);
        if ((int)i == g_pressed || (int)i == g_hot) {
            HBRUSH b = CreateSolidBrush((int)i == g_pressed ? RGB(204, 232, 255) : RGB(229, 243, 255));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        switch (h.part) {
            case Part::Text: {
                SetTextColor(dc, text);
                RECT t = h.rc;
                DrawTextW(dc, g_crumbs[h.index].text.c_str(), (int)g_crumbs[h.index].text.size(), &t,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            case Part::Chevron: Chevron(dc, h.rc, (int)i == g_pressed, gray); break;
            case Part::Overflow: {
                SetTextColor(dc, gray);
                RECT t = h.rc;
                DrawTextW(dc, L"«", 1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                break;
            }
        }
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

std::wstring MenuText(const std::wstring& s) {  // « & » s'afficherait comme un raccourci
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&') out += L'&';
        out += c;
    }
    return out;
}

// Affiche un menu sous la partie `hit` ; renvoie le chemin choisi ou nullptr.
const std::wstring* TrackUnder(int hit, HMENU menu, const std::vector<const std::wstring*>& paths) {
    RECT r = g_hits[hit].rc;
    MapWindowPoints(g_wnd, nullptr, (POINT*)&r, 2);
    g_pressed = hit;
    InvalidateRect(g_wnd, nullptr, FALSE);
    const UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL,
                                            r.left, r.bottom, g_wnd, nullptr);
    g_pressed = -1;
    InvalidateRect(g_wnd, nullptr, FALSE);
    return cmd >= 1 && cmd <= paths.size() ? paths[cmd - 1] : nullptr;
}

void ShowOverflow(int hit) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    std::vector<const std::wstring*> paths;
    for (int i = g_firstShown - 1; i >= 0; --i) {  // le plus proche d'abord, comme l'Explorateur
        paths.push_back(&g_crumbs[i].path);
        AppendMenuW(menu, MF_STRING, paths.size(), MenuText(g_crumbs[i].text).c_str());
    }
    const std::wstring* pick = TrackUnder(hit, menu, paths);
    DestroyMenu(menu);
    if (pick && g_host.navigate) g_host.navigate(std::wstring(*pick));
}

// Clic sur un chevron : lecture des sous-dossiers sur un thread, menu au retour.
void RequestChevron(int hit) {
    const int index = g_hits[hit].index;
    const unsigned serial = ++g_menuSerial;
    g_pressed = hit;
    InvalidateRect(g_wnd, nullptr, FALSE);
    std::thread([serial, index, path = g_crumbs[index].path, wnd = g_wnd] {
        auto r = std::make_unique<MenuResult>(MenuResult{serial, index, NavPane_ReadChildren(path)});
        if (PostMessageW(wnd, WM_CRUMB_MENU, 0, (LPARAM)r.get())) r.release();
    }).detach();
}

void ShowChevron(std::unique_ptr<MenuResult> r) {
    if (r->serial != g_menuSerial || r->index >= (int)g_crumbs.size()) return;  // fil changé entre-temps
    int hit = -1;
    for (size_t i = 0; i < g_hits.size(); ++i)
        if (g_hits[i].part == Part::Chevron && g_hits[i].index == r->index) hit = (int)i;
    if (hit < 0) {
        g_pressed = -1;
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    std::vector<const std::wstring*> paths;
    const std::wstring* next = r->index + 1 < (int)g_crumbs.size() ? &g_crumbs[r->index + 1].path : nullptr;
    for (const auto& f : r->folders) {
        if (paths.size() >= kMaxMenuItems) break;
        paths.push_back(&f.path);
        // Le dossier suivant dans le fil est en gras, comme dans l'Explorateur.
        const bool current = next && _wcsicmp(next->c_str(), f.path.c_str()) == 0;
        AppendMenuW(menu, MF_STRING, paths.size(), MenuText(f.text).c_str());
        if (current) SetMenuDefaultItem(menu, (UINT)paths.size(), FALSE);
    }
    if (paths.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(aucun sous-dossier)");
    const std::wstring* pick = TrackUnder(hit, menu, paths);
    DestroyMenu(menu);
    if (pick && g_host.navigate) g_host.navigate(std::wstring(*pick));
}

void SetHot(int hot) {
    if (hot == g_hot) return;
    g_hot = hot;
    InvalidateRect(g_wnd, nullptr, FALSE);
}

LRESULT CALLBACK CrumbsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETFONT:
            g_font = (HFONT)wp;
            LayoutParts();
            if (lp) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETFONT: return (LRESULT)g_font;
        case WM_SIZE:
            LayoutParts();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Paint(dc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&t);
            }
            SetHot(HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            SetHot(-1);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hit < 0) {  // dans le vide : saisie du chemin
                if (g_host.edit) g_host.edit();
                return 0;
            }
            // Chevron et « « » s'ouvrent à l'appui, un segment au relâchement.
            if (g_hits[hit].part == Part::Chevron) { RequestChevron(hit); return 0; }
            if (g_hits[hit].part == Part::Overflow) { ShowOverflow(hit); return 0; }
            g_pressed = hit;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MBUTTONUP: {
            const int hit = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hit >= 0 && g_hits[hit].part == Part::Text && g_host.openNewTab)
                g_host.openNewTab(std::wstring(g_crumbs[g_hits[hit].index].path));
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            const int was = g_pressed;  // avant ReleaseCapture : son WM_CAPTURECHANGED l'efface
            if (GetCapture() == hwnd) ReleaseCapture();
            if (was < 0 || g_hits[was].part != Part::Text) return 0;
            g_pressed = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (hit == was && g_host.navigate) g_host.navigate(std::wstring(g_crumbs[g_hits[hit].index].path));
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (g_pressed >= 0 && g_pressed < (int)g_hits.size() && g_hits[g_pressed].part == Part::Text) {
                g_pressed = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CRUMB_MENU: ShowChevron(std::unique_ptr<MenuResult>((MenuResult*)lp)); return 0;
        case WM_CRUMB_PROBE: {
            if (wp >= g_hits.size()) return -1;
            const Hit& h = g_hits[wp];
            return ((LRESULT)h.part << 40) | ((LRESULT)(h.index & 0xFF) << 32) |
                   ((LRESULT)((h.rc.left + h.rc.right) / 2) << 16) | (LRESULT)((h.rc.top + h.rc.bottom) / 2);
        }
        case WM_DESTROY: g_wnd = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND Crumbs_Create(HWND parent, UINT id, CrumbHost host) {
    g_host = std::move(host);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = CrumbsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FastExplorerCrumbs";
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"FastExplorerCrumbs", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                            (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    Crumbs_SetPath(L"");
    return g_wnd;
}

void Crumbs_SetPath(const std::wstring& path) {
    g_path = path;
    Crumbs_Update();
}

void Crumbs_Update() {
    auto crumbs = Crumbs_Split(g_path, g_host.driveText);
    bool same = crumbs.size() == g_crumbs.size();
    for (size_t i = 0; same && i < crumbs.size(); ++i) same = crumbs[i].text == g_crumbs[i].text && crumbs[i].path == g_crumbs[i].path;
    if (same) return;
    g_crumbs = std::move(crumbs);
    ++g_menuSerial;  // un menu en attente ne correspond plus au fil
    g_hot = g_pressed = -1;
    LayoutParts();
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}
