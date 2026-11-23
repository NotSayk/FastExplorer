#include "git.h"

#include <algorithm>
#include <cwctype>
#include <thread>

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

const std::wstring& GitExe() {
    static const std::wstring exe = [] {
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr)) return std::wstring(buf);
        for (const wchar_t* env : {L"ProgramFiles", L"ProgramW6432", L"LOCALAPPDATA"}) {
            wchar_t base[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(env, base, MAX_PATH)) continue;
            for (const wchar_t* sub : {L"\\Git\\cmd\\git.exe", L"\\Programs\\Git\\cmd\\git.exe"}) {
                const std::wstring p = std::wstring(base) + sub;
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
            }
        }
        return std::wstring();
    }();
    return exe;
}

void Drain(HANDLE h, std::string* out) {
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
}

wchar_t CodeOf(char x, char y) {
    if (x == '?' && y == '?') return L'?';
    if (x == '!' && y == '!') return L'!';
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return L'U';
    const char c = y != ' ' && y != '.' ? y : x;
    switch (c) {
        case 'M': case 'A': case 'D': case 'R': case 'C': case 'T': return (wchar_t)c;
    }
    return L'M';
}

}  // namespace

bool Git_Available() { return !GitExe().empty(); }

std::wstring Git_FindRoot(const std::wstring& dir) {
    if (dir.size() < 3 || (dir[1] != L':' && !(dir[0] == L'\\' && dir[1] == L'\\'))) return L"";
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/') && d.size() > 3) d.pop_back();
    for (int depth = 0; depth < 64; ++depth) {
        const std::wstring g = d + (d.back() == L'\\' ? L"" : L"\\") + L".git";
        if (GetFileAttributesW(g.c_str()) != INVALID_FILE_ATTRIBUTES) return d;
        const size_t s = d.find_last_of(L'\\');
        if (s == std::wstring::npos) break;
        if (s < 3) {  // « C:\ » : déjà essayé ?
            if (d.size() <= 3) break;
            d = d.substr(0, 3);
            continue;
        }
        if (d[0] == L'\\' && std::count(d.begin(), d.end(), L'\\') <= 3) break;  // « \\serveur\partage »
        d.resize(s);
    }
    return L"";
}

bool Git_Run(const std::wstring& root, const std::wstring& args, std::string& out, std::string& err, DWORD& exitCode,
             DWORD timeoutMs) {
    out.clear();
    err.clear();
    exitCode = (DWORD)-1;
    if (GitExe().empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE oR, oW, eR, eW;
    if (!CreatePipe(&oR, &oW, &sa, 0)) return false;
    if (!CreatePipe(&eR, &eW, &sa, 0)) {
        CloseHandle(oR), CloseHandle(oW);
        return false;
    }
    SetHandleInformation(oR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(eR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = oW;
    si.hStdError = eW;
    // Sans verrou facultatif : un « git status » de fond ne gêne pas celui qui travaille dans le dépôt.
    std::wstring cmd = L"\"" + GitExe() + L"\" --no-optional-locks -C \"" + root + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(oW);
    CloseHandle(eW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(oR), CloseHandle(eR);
        return false;
    }
    std::thread te(Drain, eR, &err);
    Drain(oR, &out);  // fini quand git ferme sa sortie
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    te.join();
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void Git_ParseStatus(const std::string& out, GitStatus& st) {
    size_t i = 0;
    bool renameNext = false;
    while (i < out.size()) {
        size_t e = out.find('\0', i);
        if (e == std::string::npos) e = out.size();
        const std::string rec = out.substr(i, e - i);
        i = e + 1;
        if (renameNext) {  // ancien nom d'un renommage : ignoré
            renameNext = false;
            continue;
        }
        if (rec.rfind("## ", 0) == 0) {
            std::string b = rec.substr(3);
            if (b.rfind("No commits yet on ", 0) == 0) b = b.substr(18);
            else if (b.rfind("Initial commit on ", 0) == 0) b = b.substr(18);
            if (b.rfind("HEAD (no branch)", 0) == 0) {
                st.branch = L"(détachée)";
                continue;
            }
            if (const size_t dots = b.find("..."); dots != std::string::npos) b.resize(dots);
            if (const size_t sp = b.find(' '); sp != std::string::npos) b.resize(sp);
            st.branch = Wide(b);
            continue;
        }
        if (rec.size() < 4) continue;
        const wchar_t code = CodeOf(rec[0], rec[1]);
        if (rec[0] == 'R' || rec[0] == 'C') renameNext = true;
        std::wstring path = Wide(rec.substr(3));
        for (auto& c : path)
            if (c == L'/') c = L'\\';
        st.files[Lower(path)] = code;
    }
}

GitStatus Git_Status(const std::wstring& root, DWORD timeoutMs) {
    GitStatus st;
    st.root = root;
    std::string out, err;
    DWORD code = 0;
    if (!Git_Run(root, L"status --porcelain=v1 -z -b --ignored=matching --untracked-files=normal", out, err, code,
                 timeoutMs)) {
        st.error = ERROR_FILE_NOT_FOUND;
        st.message = L"git introuvable";
        return st;
    }
    if (code != 0) {
        st.error = code;
        st.message = Wide(err);
        return st;
    }
    Git_ParseStatus(out, st);
    st.ok = true;
    return st;
}

std::map<std::wstring, wchar_t> Git_FolderStates(const GitStatus& st, const std::wstring& dir) {
    std::map<std::wstring, wchar_t> out;
    if (!st.ok || st.root.empty() || dir.size() < st.root.size() || _wcsnicmp(dir.c_str(), st.root.c_str(), st.root.size()) != 0)
        return out;
    std::wstring rel = Lower(dir.substr(st.root.size()));
    while (!rel.empty() && rel.front() == L'\\') rel.erase(0, 1);
    if (!rel.empty() && rel.back() != L'\\') rel += L'\\';
    // Dossier affiché dans un dossier ignoré / non suivi en entier : tout en hérite.
    for (size_t p = rel.find(L'\\'); p != std::wstring::npos; p = rel.find(L'\\', p + 1)) {
        const auto it = st.files.find(rel.substr(0, p + 1));
        if (it != st.files.end() && (it->second == L'!' || it->second == L'?')) {
            out[L"*"] = it->second;
            return out;
        }
    }
    for (auto it = st.files.lower_bound(rel); it != st.files.end() && it->first.compare(0, rel.size(), rel) == 0; ++it) {
        const std::wstring rest = it->first.substr(rel.size());
        const size_t slash = rest.find(L'\\');
        if (slash == std::wstring::npos) {
            out[rest] = it->second;  // fichier de ce dossier
        } else if (slash + 1 == rest.size()) {
            out[rest.substr(0, slash)] = it->second;  // sous-dossier entier non suivi / ignoré
        } else if (it->second != L'!') {
            auto& c = out[rest.substr(0, slash)];  // changement plus bas
            if (!c) c = L'm';
        }
    }
    return out;
}

const wchar_t* Git_StateText(wchar_t code) {
    switch (code) {
        case L'M': return L"Modifié";
        case L'A': return L"Ajouté";
        case L'D': return L"Supprimé";
        case L'R': return L"Renommé";
        case L'C': return L"Copié";
        case L'T': return L"Type modifié";
        case L'?': return L"Non suivi";
        case L'!': return L"Ignoré";
        case L'U': return L"Conflit";
        case L'm': return L"Modifié (contenu)";
    }
    return L"";
}

std::vector<std::wstring> Git_Branches(const std::wstring& root) {
    std::vector<std::wstring> v;
    std::string out, err;
    DWORD code = 0;
    if (!Git_Run(root, L"branch --format=%(refname:short)", out, err, code) || code) return v;
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(i, e - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line.rfind("(HEAD", 0) != 0) v.push_back(Wide(line));
        i = e + 1;
    }
    return v;
}

DWORD Git_Switch(const std::wstring& root, const std::wstring& branch, std::wstring& message) {
    std::string out, err;
    DWORD code = 0;
    std::wstring q;
    for (wchar_t c : branch)
        if (c != L'"') q += c;
    if (!Git_Run(root, L"switch \"" + q + L"\"", out, err, code, 30000)) {
        message = L"git introuvable";
        return ERROR_FILE_NOT_FOUND;
    }
    message = Wide(err.empty() ? out : err);
    while (!message.empty() && iswspace(message.back())) message.pop_back();
    return code;
}
