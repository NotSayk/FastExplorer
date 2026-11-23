#include "term.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT WM_TERM_OUTPUT = WM_APP + 1;
constexpr wchar_t kMarkOpen = 1, kMarkClose = 2;  // « commande finie, dossier courant » envoyé par le script
constexpr size_t kMaxOutput = 400000;               // caractères gardés dans la sortie

// Ce que le thread de lecture partage avec l'interface (survit à la fenêtre si besoin).
struct Shared {
    std::mutex lock;
    std::wstring text;
    bool ended = false;
};

struct Terminal {
    HWND wnd = nullptr, out = nullptr, prompt = nullptr, input = nullptr;
    HFONT font = nullptr;
    HBRUSH bg = nullptr;
    HANDLE job = nullptr, process = nullptr, stdinW = nullptr;
    std::shared_ptr<Shared> shared;
    std::wstring pending;   // sortie reçue en partie (marqueur coupé)
    std::wstring location;  // dossier courant de PowerShell
    std::wstring wantDir;   // dossier de l'explorateur, à rejoindre dès que possible
    int busy = 0;           // commandes envoyées dont on attend la fin
    int silent = 0;         // dont l'écho est caché (Set-Location automatiques)
    std::vector<std::wstring> history;
    size_t histAt = 0;
    bool shown = false;
} g;

const COLORREF kBack = RGB(12, 12, 12), kFore = RGB(220, 220, 220), kPromptFore = RGB(110, 190, 255);

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool FileSystemDir(const std::wstring& d) {
    return (d.size() >= 3 && d[1] == L':' && d[2] == L'\\') || (d.size() > 2 && d[0] == L'\\' && d[1] == L'\\' && d[2] != L'?');
}

void Append(const std::wstring& raw) {
    if (raw.empty()) return;
    std::wstring t;  // fins de ligne de l'EDIT : \r\n
    t.reserve(raw.size() + raw.size() / 16);
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t c = raw[i];
        if (c == L'\r') continue;
        if (c == L'\n') t += L"\r\n";
        else if (c == L'\t' || c >= 32) t += c;
    }
    int len = GetWindowTextLengthW(g.out);
    if ((size_t)len + t.size() > kMaxOutput) {  // trop long : on retire le début
        SendMessageW(g.out, EM_SETSEL, 0, (LPARAM)(kMaxOutput / 3));
        SendMessageW(g.out, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g.out);
    }
    SendMessageW(g.out, EM_SETSEL, len, len);
    SendMessageW(g.out, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    SendMessageW(g.out, WM_VSCROLL, SB_BOTTOM, 0);
}

void SetPrompt() {
    const std::wstring p = L"PS " + g.location + L">";
    SetWindowTextW(g.prompt, p.c_str());
    SendMessageW(g.wnd, WM_SIZE, 0, 0);
}

void Send(const std::wstring& line, bool echo) {
    if (!g.stdinW) return;
    if (echo) Append(L"PS " + g.location + L"> " + line + L"\n");
    else ++g.silent;
    const std::string s = Utf8(line) + "\n";
    DWORD w = 0;
    WriteFile(g.stdinW, s.data(), (DWORD)s.size(), &w, nullptr);
    ++g.busy;
}

// Rejoint le dossier de l'explorateur si PowerShell est libre.
void FollowDir() {
    if (g.busy || !g.stdinW || g.wantDir.empty() || !FileSystemDir(g.wantDir)) return;
    if (_wcsicmp(g.wantDir.c_str(), g.location.c_str()) == 0) return;
    Send(Term_CdCommand(g.wantDir), false);
}

// Script lancé par -EncodedCommand : lit une ligne, l'exécute (point : variables et fonctions gardées, erreurs
// comprises dans la sortie), écrit 0x01 dossier 0x02.
const wchar_t kScript[] =
    L"$ProgressPreference='SilentlyContinue'\n"
    L"$u=New-Object Text.UTF8Encoding $false\n"
    L"[Console]::InputEncoding=$u;[Console]::OutputEncoding=$u;$OutputEncoding=$u\n"
    L"$__in=[Console]::In\n"
    L"function __fe_done{[Console]::Out.Write([string][char]1+(Get-Location).ProviderPath+[char]2);[Console]::Out.Flush()}\n"
    L"__fe_done\n"
    L"while(($__l=$__in.ReadLine()) -ne $null){\n"
    L" try{. ([scriptblock]::Create($__l)) 2>&1|Out-String -Stream -Width 200|ForEach-Object{[Console]::Out.WriteLine($_)}}\n"
    L" catch{[Console]::Out.WriteLine(($_|Out-String).TrimEnd())}\n"
    L" __fe_done\n"
    L"}\n";

void Stop() {
    if (g.job) {
        TerminateJobObject(g.job, 1);
        CloseHandle(g.job);
        g.job = nullptr;
    }
    if (g.process) CloseHandle(g.process), g.process = nullptr;
    if (g.stdinW) CloseHandle(g.stdinW), g.stdinW = nullptr;
    if (g.shared) {
        std::lock_guard<std::mutex> l(g.shared->lock);
        g.shared->ended = true;  // le thread de lecture se termine avec le tuyau ; ses restes sont ignorés
    }
    g.shared.reset();
    g.busy = g.silent = 0;
    g.pending.clear();
}

bool Start(const std::wstring& dir) {
    Stop();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        CloseHandle(inR), CloseHandle(inW);
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) +
                       L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass "
                       L"-EncodedCommand " +
                       Term_Base64(kScript);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = outW;
    PROCESS_INFORMATION pi{};
    std::wstring cwd = FileSystemDir(dir) ? dir : L"";
    if (cwd.empty()) {
        wchar_t prof[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) cwd = prof;
    }
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW), CloseHandle(outR);
        Append(L"PowerShell n'a pas pu démarrer (erreur " + std::to_wstring(GetLastError()) + L").\n");
        return false;
    }
    // Tâche : fermer le terminal arrête aussi ce que PowerShell a lancé.
    g.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g.job) {
        SetInformationJobObject(g.job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(g.job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g.process = pi.hProcess;
    g.stdinW = inW;
    g.location = cwd;
    g.busy = 1;  // jusqu'au premier marqueur : le script démarre
    g.silent = 1;
    g.shared = std::make_shared<Shared>();
    std::shared_ptr<Shared> sh = g.shared;
    HWND wnd = g.wnd;
    std::thread([sh, outR, wnd] {
        std::string carry;  // octets d'un caractère UTF-8 coupé
        char buf[8192];
        DWORD got = 0;
        while (ReadFile(outR, buf, sizeof buf, &got, nullptr) && got) {
            carry.append(buf, got);
            size_t keep = 0;  // fin incomplète : gardée pour la lecture suivante
            for (size_t i = carry.size(); i > 0 && carry.size() - i < 4; --i) {
                const unsigned char c = (unsigned char)carry[i - 1];
                if ((c & 0xC0) == 0x80) continue;
                const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                if (carry.size() - (i - 1) < need) keep = carry.size() - (i - 1);
                break;
            }
            const int n = (int)(carry.size() - keep);
            std::wstring w;
            if (n > 0) {
                w.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, carry.data(), n, nullptr, 0));
                MultiByteToWideChar(CP_UTF8, 0, carry.data(), n, w.data(), (int)w.size());
            }
            carry.erase(0, (size_t)n);
            {
                std::lock_guard<std::mutex> l(sh->lock);
                if (sh->ended) break;
                sh->text += w;
            }
            PostMessageW(wnd, WM_TERM_OUTPUT, 0, 0);
        }
        CloseHandle(outR);
        {
            std::lock_guard<std::mutex> l(sh->lock);
            if (sh->ended) return;
            sh->ended = true;
        }
        PostMessageW(wnd, WM_TERM_OUTPUT, 1, 0);
    }).detach();
    SetPrompt();
    return true;
}

void OnOutput() {
    if (!g.shared) return;
    std::wstring t;
    bool ended = false;
    {
        std::lock_guard<std::mutex> l(g.shared->lock);
        t.swap(g.shared->text);
        ended = g.shared->ended;
    }
    g.pending += t;
    std::wstring show;
    for (;;) {
        const size_t a = g.pending.find(kMarkOpen);
        if (a == std::wstring::npos) {
            show += g.pending;
            g.pending.clear();
            break;
        }
        const size_t b = g.pending.find(kMarkClose, a);
        if (b == std::wstring::npos) {  // marqueur pas encore complet
            show += g.pending.substr(0, a);
            g.pending.erase(0, a);
            break;
        }
        show += g.pending.substr(0, a);
        g.location = g.pending.substr(a + 1, b - a - 1);
        g.pending.erase(0, b + 1);
        if (g.busy) --g.busy;
        if (g.silent) --g.silent;
    }
    Append(show);
    SetPrompt();
    if (ended) {
        Append(L"\n[PowerShell s'est arrêté. Entrée : le relancer]\n");
        Stop();
        return;
    }
    FollowDir();
}

void Submit() {
    const int n = GetWindowTextLengthW(g.input);
    std::wstring line((size_t)n, L'\0');
    GetWindowTextW(g.input, line.data(), n + 1);
    SetWindowTextW(g.input, L"");
    if (!g.stdinW) {  // arrêté : on relance
        Start(g.wantDir.empty() ? g.location : g.wantDir);
        if (line.empty()) return;
    }
    std::wstring trimmed = line;
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(0, 1);
    if (!trimmed.empty() && (g.history.empty() || g.history.back() != trimmed)) g.history.push_back(trimmed);
    g.histAt = g.history.size();
    if (_wcsicmp(trimmed.c_str(), L"cls") == 0 || _wcsicmp(trimmed.c_str(), L"clear") == 0) {
        SetWindowTextW(g.out, L"");
        return;
    }
    if (_wcsicmp(trimmed.c_str(), L"exit") == 0) {
        Term_Show(false, L"");
        SetFocus(GetParent(g.wnd));
        return;
    }
    Send(line, true);
}

LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
            case VK_RETURN: Submit(); return 0;
            case VK_ESCAPE: SetWindowTextW(h, L""); return 0;
            case VK_UP:
            case VK_DOWN: {
                if (g.history.empty()) return 0;
                if (wp == VK_UP && g.histAt > 0) --g.histAt;
                if (wp == VK_DOWN && g.histAt < g.history.size()) ++g.histAt;
                const std::wstring t = g.histAt < g.history.size() ? g.history[g.histAt] : L"";
                SetWindowTextW(h, t.c_str());
                SendMessageW(h, EM_SETSEL, t.size(), t.size());
                return 0;
            }
            case 'C': {
                DWORD s = 0, e = 0;
                SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
                if (ctrl && s == e && g.busy && g.stdinW) {  // Ctrl+C sans sélection : arrête la commande
                    Append(L"^C\n[Commande arrêtée]\n");
                    const std::wstring at = g.location;
                    Start(at);
                    return 0;
                }
                break;
            }
            case 'L':
                if (ctrl) {
                    SetWindowTextW(g.out, L"");
                    return 0;
                }
                break;
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == 12 /* Ctrl+L */)) return 0;
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK TermProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            RECT rc;
            GetClientRect(h, &rc);
            HDC dc = GetDC(h);
            HGDIOBJ old = SelectObject(dc, g.font ? g.font : GetStockObject(DEFAULT_GUI_FONT));
            TEXTMETRICW tm{};
            GetTextMetricsW(dc, &tm);
            wchar_t p[1024] = {};
            GetWindowTextW(g.prompt, p, 1024);
            SIZE ps{};
            GetTextExtentPoint32W(dc, p, (int)wcslen(p), &ps);
            SelectObject(dc, old);
            ReleaseDC(h, dc);
            const int line = tm.tmHeight + 8, pad = 4;
            const int promptW = std::min<int>(ps.cx + 6, (int)rc.right * 2 / 5);
            const int topBorder = 2;
            MoveWindow(g.out, pad, topBorder, std::max(0, (int)rc.right - 2 * pad), std::max(0, (int)rc.bottom - line - topBorder), TRUE);
            MoveWindow(g.prompt, pad, rc.bottom - line, promptW, line, TRUE);
            MoveWindow(g.input, pad + promptW, rc.bottom - line + 4, std::max(0, (int)rc.right - 2 * pad - promptW), line - 4, TRUE);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.bg);
            RECT top = rc;
            top.bottom = 1;
            FillRect((HDC)wp, &top, GetSysColorBrush(COLOR_3DSHADOW));  // trait de séparation
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetBkColor((HDC)wp, kBack);
            SetTextColor((HDC)wp, (HWND)lp == g.prompt ? kPromptFore : kFore);
            return (LRESULT)g.bg;
        case WM_TERM_OUTPUT: OnOutput(); return 0;
        case WM_SETFOCUS: SetFocus(g.input); return 0;
        case WM_LBUTTONDOWN: SetFocus(g.input); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

std::wstring Term_CdCommand(const std::wstring& dir) {
    std::wstring q;
    for (wchar_t c : dir) {
        q += c;
        if (c == L'\'' || c == L'‘' || c == L'’') q += c;  // PowerShell : guillemet simple doublé
    }
    return L"Set-Location -LiteralPath '" + q + L"'";
}

std::wstring Term_Base64(const std::wstring& utf16) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> b;
    b.reserve(utf16.size() * 2);
    for (wchar_t c : utf16) {
        b.push_back((unsigned char)(c & 0xFF));
        b.push_back((unsigned char)((c >> 8) & 0xFF));
    }
    std::wstring out;
    out.reserve((b.size() + 2) / 3 * 4);
    for (size_t i = 0; i < b.size(); i += 3) {
        const unsigned v = (unsigned)b[i] << 16 | (i + 1 < b.size() ? (unsigned)b[i + 1] << 8 : 0) |
                           (i + 2 < b.size() ? (unsigned)b[i + 2] : 0);
        out += (wchar_t)k[(v >> 18) & 63];
        out += (wchar_t)k[(v >> 12) & 63];
        out += i + 1 < b.size() ? (wchar_t)k[(v >> 6) & 63] : L'=';
        out += i + 2 < b.size() ? (wchar_t)k[v & 63] : L'=';
    }
    return out;
}

HWND Term_Create(HWND parent, int id) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = TermProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = L"FastExplorerTerminal";
    RegisterClassExW(&wc);
    g.bg = CreateSolidBrush(kBack);
    g.wnd = CreateWindowExW(0, wc.lpszClassName, L"Terminal", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                            (HMENU)(INT_PTR)id, inst, nullptr);
    g.out = CreateWindowExW(0, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
                            0, 0, 0, 0, g.wnd, nullptr, inst, nullptr);
    SendMessageW(g.out, EM_SETLIMITTEXT, 0, 0);
    g.prompt = CreateWindowExW(0, WC_STATICW, L"PS>", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                               0, 0, 0, 0, g.wnd, nullptr, inst, nullptr);
    g.input = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, g.wnd,
                              nullptr, inst, nullptr);
    SendMessageW(g.input, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"Commande PowerShell (Entrée) ; ↑ ↓ : historique ; Ctrl+C : arrêter ; exit : fermer");
    SetWindowSubclass(g.input, InputProc, 1, 0);
    return g.wnd;
}

void Term_SetFont(int heightPx) {
    HFONT f = CreateFontW(-std::max(10, heightPx * 4 / 5), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (_wcsicmp(face, L"Cascadia Mono") != 0) {  // police absente : Consolas
        DeleteObject(f);
        f = CreateFontW(-std::max(10, heightPx * 4 / 5), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    for (HWND w : {g.out, g.prompt, g.input}) SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE);
    if (g.font) DeleteObject(g.font);
    g.font = f;
    if (g.wnd) SendMessageW(g.wnd, WM_SIZE, 0, 0);
}

void Term_Show(bool on, const std::wstring& dir) {
    if (!g.wnd) return;
    g.shown = on;
    if (on) {
        if (!dir.empty()) g.wantDir = dir;
        if (!g.stdinW) {
            if (GetWindowTextLengthW(g.out) == 0)
                Append(L"PowerShell — suit le dossier affiché. Pas de programmes plein écran ici : « Ouvrir dans le Terminal » pour ceux-là.\n");
            Start(g.wantDir);
        } else {
            FollowDir();
        }
    }
    ShowWindow(g.wnd, on ? SW_SHOW : SW_HIDE);
}

bool Term_Shown() { return g.shown; }

void Term_SetDir(const std::wstring& dir) {
    g.wantDir = dir;
    if (g.shown) FollowDir();
}

void Term_Focus() {
    if (g.input) SetFocus(g.input);
}

bool Term_Owns(HWND h) { return h && g.wnd && (h == g.wnd || IsChild(g.wnd, h)); }

void Term_Shutdown() { Stop(); }
