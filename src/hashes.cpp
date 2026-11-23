#include "hashes.h"

#include <bcrypt.h>
#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr const wchar_t* kAlgos[3] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM};

std::wstring Hex(const unsigned char* p, size_t n) {
    static const wchar_t* k = L"0123456789abcdef";
    std::wstring s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += k[p[i] >> 4];
        s += k[p[i] & 15];
    }
    return s;
}

// Les trois empreintes calculées ensemble.
struct Hasher {
    BCRYPT_ALG_HANDLE alg[3] = {};
    BCRYPT_HASH_HANDLE hash[3] = {};
    bool ok = true;
    Hasher() {
        for (int i = 0; i < 3; ++i) {
            ok = ok && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg[i], kAlgos[i], nullptr, 0)) &&
                 BCRYPT_SUCCESS(BCryptCreateHash(alg[i], &hash[i], nullptr, 0, nullptr, 0, 0));
        }
    }
    ~Hasher() {
        for (int i = 0; i < 3; ++i) {
            if (hash[i]) BCryptDestroyHash(hash[i]);
            if (alg[i]) BCryptCloseAlgorithmProvider(alg[i], 0);
        }
    }
    void Add(const void* p, ULONG n) {
        for (auto h : hash) BCryptHashData(h, (PUCHAR)p, n, 0);
    }
    void Finish(FileHashes& out) {
        std::wstring* dst[3] = {&out.md5, &out.sha1, &out.sha256};
        for (int i = 0; i < 3; ++i) {
            unsigned char buf[64];
            const ULONG len = i == 0 ? 16 : i == 1 ? 20 : 32;
            if (BCRYPT_SUCCESS(BCryptFinishHash(hash[i], buf, len, 0))) *dst[i] = Hex(buf, len);
        }
    }
};

// ───────────────────────────── Fenêtre ─────────────────────────────

constexpr UINT WM_HASH_PROGRESS = WM_APP + 1;
enum { ID_OUT = 101, ID_CMP, ID_CMPRES, ID_COPY, ID_COPYALL, ID_CLOSE, ID_PROG };

struct Work {
    std::vector<std::wstring> paths;
    std::mutex lock;
    std::vector<FileHashes> done;  // dans l'ordre de paths
    uint64_t bytes = 0, total = 0;
    std::atomic<bool> cancel{false};
};

struct Dialog {
    HWND wnd = nullptr, out = nullptr, cmp = nullptr, cmpRes = nullptr, prog = nullptr;
    std::shared_ptr<Work> work;
    size_t shownCount = (size_t)-1;
    bool done = false;
    HFONT mono = nullptr;
};
Dialog* g_dlg = nullptr;

std::wstring Leaf(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring ErrorText(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"erreur " + std::to_wstring(e);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s;
}

void SetClipboard(HWND h, const std::wstring& text) {
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    const size_t cb = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, cb)) {
        memcpy(GlobalLock(g), text.c_str(), cb);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

std::wstring Report(Dialog& d, bool sha256Only) {
    std::lock_guard<std::mutex> l(d.work->lock);
    std::wstring s;
    for (size_t i = 0; i < d.work->done.size(); ++i) {
        const FileHashes& h = d.work->done[i];
        const std::wstring name = Leaf(d.work->paths[i]);
        if (sha256Only) {
            if (!h.error) s += h.sha256 + (d.work->paths.size() > 1 ? L"  " + name : L"") + L"\r\n";
            continue;
        }
        s += name + L"\r\n";
        if (h.error) s += L"  " + ErrorText(h.error) + L"\r\n\r\n";
        else s += L"  MD5     " + h.md5 + L"\r\n  SHA-1   " + h.sha1 + L"\r\n  SHA-256 " + h.sha256 + L"\r\n\r\n";
    }
    if (sha256Only && d.work->paths.size() == 1)
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}

void Compare(Dialog& d) {
    wchar_t buf[512] = {};
    GetWindowTextW(d.cmp, buf, 512);
    const std::wstring want = Hash_Normalize(buf);
    if (want.empty()) {
        SetWindowTextW(d.cmpRes, L"");
        return;
    }
    std::wstring res = L"✗ Aucune empreinte ne correspond";
    std::lock_guard<std::mutex> l(d.work->lock);
    for (size_t i = 0; i < d.work->done.size(); ++i) {
        const FileHashes& h = d.work->done[i];
        const std::pair<const wchar_t*, const std::wstring*> all[] = {{L"MD5", &h.md5}, {L"SHA-1", &h.sha1}, {L"SHA-256", &h.sha256}};
        for (const auto& [name, v] : all)
            if (!v->empty() && *v == want) res = std::wstring(L"✓ Identique : ") + name + L" de « " + Leaf(d.work->paths[i]) + L" »";
    }
    if (d.work->done.size() < d.work->paths.size() && res[0] == L'✗') res = L"Calcul en cours…";
    SetWindowTextW(d.cmpRes, res.c_str());
}

void Refresh(Dialog& d) {
    size_t n;
    uint64_t bytes, total;
    {
        std::lock_guard<std::mutex> l(d.work->lock);
        n = d.work->done.size();
        bytes = d.work->bytes;
        total = d.work->total;
    }
    if (n != d.shownCount) {
        d.shownCount = n;
        SetWindowTextW(d.out, Report(d, false).c_str());
        Compare(d);
    }
    const bool finished = n >= d.work->paths.size();
    std::wstring p = finished ? L"Terminé"
                              : L"Calcul… " + std::to_wstring(total ? (int)(bytes * 100 / total) : 0) + L" % (" +
                                    std::to_wstring(n + 1) + L"/" + std::to_wstring(d.work->paths.size()) + L")";
    SetWindowTextW(d.prog, p.c_str());
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dialog* d = g_dlg;
    switch (msg) {
        case WM_HASH_PROGRESS:
            if (d) Refresh(*d);
            return 0;
        case WM_COMMAND:
            if (!d) return 0;
            if (LOWORD(wp) == ID_CMP && HIWORD(wp) == EN_CHANGE) Compare(*d);
            if (LOWORD(wp) == ID_COPY) SetClipboard(h, Report(*d, true));
            if (LOWORD(wp) == ID_COPYALL) SetClipboard(h, Report(*d, false));
            if (LOWORD(wp) == ID_CLOSE || LOWORD(wp) == IDCANCEL) d->done = true;
            return 0;
        case WM_CTLCOLORSTATIC:
            if (d && (HWND)lp == d->cmpRes) {
                wchar_t c[2] = {};
                GetWindowTextW(d->cmpRes, c, 2);
                SetBkMode((HDC)wp, TRANSPARENT);
                SetTextColor((HDC)wp, c[0] == L'✓' ? RGB(0, 128, 0) : c[0] == L'✗' ? RGB(190, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        case WM_CLOSE:
            if (d) d->done = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int Px(HWND h, int v) { return MulDiv(v, (int)GetDpiForWindow(h), 96); }

}  // namespace

FileHashes Hash_Bytes(const void* data, size_t len) {
    FileHashes out;
    Hasher h;
    if (!h.ok) {
        out.error = ERROR_NOT_SUPPORTED;
        return out;
    }
    h.Add(data, (ULONG)len);
    h.Finish(out);
    return out;
}

FileHashes Hash_File(const std::wstring& path, const std::atomic<bool>* cancel, const std::function<void(uint64_t)>& progress) {
    FileHashes out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        out.error = GetLastError();
        return out;
    }
    Hasher h;
    if (!h.ok) {
        CloseHandle(f);
        out.error = ERROR_NOT_SUPPORTED;
        return out;
    }
    std::vector<unsigned char> buf(1 << 20);
    uint64_t read = 0;
    DWORD got = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            out.error = ERROR_CANCELLED;
            break;
        }
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            out.error = GetLastError();
            break;
        }
        if (!got) break;
        h.Add(buf.data(), got);
        read += got;
        if (progress) progress(read);
    }
    CloseHandle(f);
    if (!out.error) h.Finish(out);
    return out;
}

std::wstring Hash_Normalize(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (iswspace(c) || c == L'-' || c == L':') continue;
        o += (wchar_t)towlower(c);
    }
    return o;
}

void Hash_Show(HWND owner, HFONT font, std::vector<std::wstring> paths) {
    if (g_dlg || paths.empty()) return;
    WNDCLASSEXW wc{sizeof wc};
    if (!GetClassInfoExW(GetModuleHandleW(nullptr), L"FastExplorerHashes", &wc)) {
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"FastExplorerHashes";
        RegisterClassExW(&wc);
    }
    Dialog d;
    d.work = std::make_shared<Work>();
    d.work->paths = std::move(paths);
    for (const auto& p : d.work->paths) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
            d.work->total += (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    }
    g_dlg = &d;
    RECT ow;
    GetWindowRect(owner, &ow);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const int w = std::min<int>(Px(owner, 820), mi.rcWork.right - mi.rcWork.left);
    const int hgt = std::min<int>(Px(owner, 460), mi.rcWork.bottom - mi.rcWork.top);
    const int x = std::clamp<int>((ow.left + ow.right - w) / 2, mi.rcWork.left, mi.rcWork.right - w);
    const int y = std::clamp<int>((ow.top + ow.bottom - hgt) / 2, mi.rcWork.top, mi.rcWork.bottom - hgt);
    d.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FastExplorerHashes", L"Empreintes des fichiers",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, hgt, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT rc;
    GetClientRect(d.wnd, &rc);
    const int m = Px(owner, 10), line = Px(owner, 26), btnW = Px(owner, 130);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int px, int py, int pw, int ph, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px, py, pw, ph, d.wnd, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    const int bottom = rc.bottom - m - line;
    const int cmpY = bottom - m - line;
    d.out = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                 ID_OUT, m, m, rc.right - 2 * m, cmpY - 2 * m);
    LOGFONTW lf{};
    GetObjectW(font, sizeof lf, &lf);
    wcscpy_s(lf.lfFaceName, L"Consolas");
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    d.mono = CreateFontIndirectW(&lf);
    SendMessageW(d.out, WM_SETFONT, (WPARAM)d.mono, TRUE);
    make(L"STATIC", L"Comparer avec :", SS_CENTERIMAGE, 0, m, cmpY, Px(owner, 120), line);
    d.cmp = make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, ID_CMP, m + Px(owner, 125), cmpY, Px(owner, 380), line);
    SendMessageW(d.cmp, EM_SETCUEBANNER, TRUE, (LPARAM)L"Collez l'empreinte publiée (MD5, SHA-1 ou SHA-256)");
    d.cmpRes = make(L"STATIC", L"", SS_CENTERIMAGE | SS_ENDELLIPSIS, ID_CMPRES, m + Px(owner, 515), cmpY,
                    rc.right - m - (m + Px(owner, 515)), line);
    d.prog = make(L"STATIC", L"", SS_CENTERIMAGE, ID_PROG, m, bottom, Px(owner, 260), line);
    make(L"BUTTON", L"Copier SHA-256", WS_TABSTOP | BS_PUSHBUTTON, ID_COPY, rc.right - 3 * (btnW + m), bottom, btnW, line);
    make(L"BUTTON", L"Copier tout", WS_TABSTOP | BS_PUSHBUTTON, ID_COPYALL, rc.right - 2 * (btnW + m), bottom, btnW, line);
    make(L"BUTTON", L"Fermer", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_CLOSE, rc.right - (btnW + m), bottom, btnW, line);

    std::shared_ptr<Work> work = d.work;
    HWND wnd = d.wnd;
    std::thread([work, wnd] {
        uint64_t before = 0;
        ULONGLONG lastPost = 0;
        for (const auto& p : work->paths) {
            FileHashes h = Hash_File(p, &work->cancel, [&](uint64_t n) {
                {
                    std::lock_guard<std::mutex> l(work->lock);
                    work->bytes = before + n;
                }
                if (GetTickCount64() - lastPost > 150) {
                    lastPost = GetTickCount64();
                    PostMessageW(wnd, WM_HASH_PROGRESS, 0, 0);
                }
            });
            if (work->cancel) return;
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) before += (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
            {
                std::lock_guard<std::mutex> l(work->lock);
                work->done.push_back(std::move(h));
                work->bytes = before;
            }
            PostMessageW(wnd, WM_HASH_PROGRESS, 0, 0);
        }
    }).detach();

    Refresh(d);
    EnableWindow(owner, FALSE);
    ShowWindow(d.wnd, SW_SHOW);
    SetFocus(d.cmp);
    MSG msg{};
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            d.done = true;
            continue;
        }
        if (IsDialogMessageW(d.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage((int)msg.wParam);
    d.work->cancel = true;  // le thread s'arrête au prochain bloc lu
    EnableWindow(owner, TRUE);
    DestroyWindow(d.wnd);
    if (d.mono) DeleteObject(d.mono);
    SetForegroundWindow(owner);
    g_dlg = nullptr;
}
