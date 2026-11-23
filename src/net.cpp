#include "net.h"

#include "util.h"

#include <lm.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winnetwk.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "mpr.lib")

namespace {

constexpr wchar_t kServersKey[] = L"Software\\FastExplorer\\Servers";
constexpr DWORD kMaxRemembered = 64;

double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

int64_t Now() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime);
}

struct Known {
    std::wstring server;  // « \\NAS »
    int64_t last = 0;     // dernière visite (0 : jamais, lecteur connecté seulement)
    std::vector<std::wstring> drives;
};

std::mutex g_discoLock;
std::set<std::wstring> g_discovered;  // « \\PC » vus par la découverte du réseau (le temps de la session)
bool g_discovering = false;

// Serveurs mémorisés : une valeur REG_QWORD par serveur (nom = « \\serveur »).
void ReadRemembered(std::map<std::wstring, Known>& out) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kServersKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD n = _countof(name), type = 0, size = sizeof(int64_t);
        int64_t t = 0;
        const LSTATUS s = RegEnumValueW(k, i, name, &n, nullptr, &type, (BYTE*)&t, &size);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s != ERROR_SUCCESS || type != REG_QWORD || !Net_IsServerPath(name)) continue;
        Known& e = out[Lower(name)];
        e.server = name;
        e.last = std::max(e.last, t);
    }
    RegCloseKey(k);
}

// Lecteurs réseau (connectés ou mémorisés par Windows) → serveur.
void ReadDrives(std::map<std::wstring, Known>& out, DWORD scope) {
    HANDLE h = nullptr;
    if (WNetOpenEnumW(scope, RESOURCETYPE_DISK, 0, nullptr, &h) != NO_ERROR) return;
    std::vector<BYTE> buf(16384);
    for (;;) {
        DWORD count = (DWORD)-1, size = (DWORD)buf.size();
        const DWORD r = WNetEnumResourceW(h, &count, buf.data(), &size);
        if (r == ERROR_MORE_DATA && size > buf.size()) {
            buf.resize(size);
            continue;
        }
        if (r != NO_ERROR) break;
        const auto* res = (const NETRESOURCEW*)buf.data();
        for (DWORD i = 0; i < count; ++i) {
            if (!res[i].lpRemoteName) continue;
            const std::wstring server = Net_ServerOf(res[i].lpRemoteName);
            if (server.empty()) continue;
            Known& e = out[Lower(server)];
            if (e.server.empty()) e.server = server;
            if (res[i].lpLocalName && *res[i].lpLocalName) {
                const std::wstring drive = res[i].lpLocalName;
                if (std::find(e.drives.begin(), e.drives.end(), drive) == e.drives.end()) e.drives.push_back(drive);
            }
        }
    }
    WNetCloseEnum(h);
}

}  // namespace

bool Net_IsServerPath(const std::wstring& p) {
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.' && p[2] != L'\\' &&
           p.find(L'\\', 2) == std::wstring::npos;
}

std::wstring Net_ServerOf(const std::wstring& p) {
    if (p.size() < 3 || p[0] != L'\\' || p[1] != L'\\' || p[2] == L'?' || p[2] == L'.' || p[2] == L'\\') return L"";
    return p.substr(0, p.find(L'\\', 2));
}

void Net_EnumShares(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    d.server = true;
    DWORD resume = 0;
    NET_API_STATUS st;
    do {
        BYTE* buf = nullptr;
        DWORD read = 0, total = 0;
        st = NetShareEnum((LPWSTR)d.path.c_str(), 1, &buf, MAX_PREFERRED_LENGTH, &read, &total, &resume);
        if ((st == NERR_Success || st == ERROR_MORE_DATA) && buf) {
            const auto* s = (const SHARE_INFO_1*)buf;
            for (DWORD i = 0; i < read; ++i) {
                if ((s[i].shi1_type & STYPE_MASK) != STYPE_DISKTREE || !s[i].shi1_netname) continue;
                const std::wstring name = s[i].shi1_netname;
                const bool admin = (s[i].shi1_type & STYPE_SPECIAL) || (!name.empty() && name.back() == L'$');
                DirData_Add(d, name.c_str(), (uint32_t)name.size(),
                            FILE_ATTRIBUTE_DIRECTORY | (admin ? FILE_ATTRIBUTE_HIDDEN : 0), 0, 0);
                d.origin.push_back(s[i].shi1_remark ? s[i].shi1_remark : L"");
            }
        }
        if (buf) NetApiBufferFree(buf);
        if (ct.cancelled()) return;
    } while (st == ERROR_MORE_DATA);
    if (st != NERR_Success) {
        d.error = st;
        d.entries.clear();
        d.pool.clear();
        d.origin.clear();
        return;
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

void Net_EnumServers(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    d.path = kNetPath;
    std::map<std::wstring, Known> known;
    ReadRemembered(known);
    ReadDrives(known, RESOURCE_CONNECTED);
    if (ct.cancelled()) return;
    ReadDrives(known, RESOURCE_REMEMBERED);
    std::set<std::wstring> seen;  // découverts sur le réseau, pas encore visités
    {
        std::lock_guard<std::mutex> l(g_discoLock);
        for (const auto& srv : g_discovered)
            if (!known.count(Lower(srv))) seen.insert(Lower(srv)), known[Lower(srv)].server = srv;
    }
    for (auto& [key, k] : known) {
        std::sort(k.drives.begin(), k.drives.end());
        std::wstring drives;
        for (const auto& dr : k.drives) drives += (drives.empty() ? L"" : L", ") + dr;
        const std::wstring name = k.server.substr(2);
        DirData_Add(d, name.c_str(), (uint32_t)name.size(), FILE_ATTRIBUTE_DIRECTORY, 0, k.last);
        d.target.push_back(k.server);
        d.origin.push_back(!drives.empty() ? (k.drives.size() > 1 ? L"Lecteurs " : L"Lecteur ") + drives
                           : seen.count(key)  ? L"Découvert sur le réseau"
                                              : std::wstring());
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

void Net_Remember(const std::wstring& path) {
    const std::wstring server = Net_ServerOf(path);
    if (server.empty()) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kServersKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return;
    // Même serveur écrit autrement (« \\nas » / « \\NAS ») : on garde la première écriture.
    std::wstring name = server;
    std::vector<std::pair<int64_t, std::wstring>> all;
    for (DWORD i = 0;; ++i) {
        wchar_t n[512];
        DWORD nn = _countof(n), type = 0, size = sizeof(int64_t);
        int64_t t = 0;
        const LSTATUS s = RegEnumValueW(k, i, n, &nn, nullptr, &type, (BYTE*)&t, &size);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s != ERROR_SUCCESS) continue;
        if (_wcsicmp(n, server.c_str()) == 0) name = n;
        else all.push_back({t, n});
    }
    const int64_t now = Now();
    RegSetValueExW(k, name.c_str(), 0, REG_QWORD, (const BYTE*)&now, sizeof now);
    if (all.size() >= kMaxRemembered) {  // les plus anciens s'en vont
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i + kMaxRemembered <= all.size(); ++i) RegDeleteValueW(k, all[i].second.c_str());
    }
    RegCloseKey(k);
}

void Net_Forget(const std::wstring& server) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kServersKey, 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {  // la casse peut différer
        wchar_t n[512];
        DWORD nn = _countof(n);
        const LSTATUS s = RegEnumValueW(k, i, n, &nn, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s == ERROR_SUCCESS && _wcsicmp(n, server.c_str()) == 0) {
            RegDeleteValueW(k, n);
            break;
        }
    }
    RegCloseKey(k);
}

bool Net_IsAuthError(DWORD e) {
    return e == ERROR_ACCESS_DENIED || e == ERROR_LOGON_FAILURE || e == ERROR_NOT_AUTHENTICATED ||
           e == ERROR_INVALID_PASSWORD || e == ERROR_BAD_USERNAME || e == ERROR_ACCOUNT_RESTRICTION;
}

DWORD Net_Connect(HWND owner, const std::wstring& path) {
    const std::wstring server = Net_ServerOf(path);
    if (server.empty()) return ERROR_BAD_NETPATH;
    std::wstring remote;
    if (Net_IsServerPath(path)) {
        remote = server + L"\\IPC$";  // le serveur lui-même : session sans partage
    } else {
        const size_t share = path.find(L'\\', server.size() + 1);
        remote = path.substr(0, share);
    }
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_ANY;
    nr.lpRemoteName = remote.data();
    return WNetAddConnection3W(owner, &nr, nullptr, nullptr, CONNECT_INTERACTIVE | CONNECT_PROMPT);
}

std::wstring Net_ErrorText(DWORD e) {
    if (e >= NERR_BASE && e <= MAX_NERR) {
        switch (e) {
            case NERR_NetNameNotFound: return L"Ce partage n'existe pas sur le serveur.";
            default: break;
        }
        HMODULE m = LoadLibraryExW(L"netmsg.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
        wchar_t* msg = nullptr;
        if (m)
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, m,
                           e, 0, (LPWSTR)&msg, 0, nullptr);
        std::wstring r = msg ? msg : L"Erreur réseau " + std::to_wstring(e);
        if (msg) LocalFree(msg);
        if (m) FreeLibrary(m);
        while (!r.empty() && (r.back() == L'\n' || r.back() == L'\r' || r.back() == L' ')) r.pop_back();
        return r;
    }
    return ErrorText(e);
}

bool Net_Discover(std::function<void(size_t)> done) {
    {
        std::lock_guard<std::mutex> l(g_discoLock);
        if (g_discovering) return false;
        g_discovering = true;
    }
    std::thread([done = std::move(done)] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::set<std::wstring> found;
        IShellItem* net = nullptr;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&net)))) {
            IEnumShellItems* en = nullptr;
            if (SUCCEEDED(net->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
                IShellItem* it = nullptr;
                while (en->Next(1, &it, nullptr) == S_OK) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p)) && p && Net_IsServerPath(p)) found.insert(p);
                    CoTaskMemFree(p);
                    it->Release();
                }
                en->Release();
            }
            net->Release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
        size_t added = 0;
        {
            std::lock_guard<std::mutex> l(g_discoLock);
            for (const auto& f : found) added += g_discovered.insert(f).second;
            g_discovering = false;
        }
        if (done) done(added);
    }).detach();
    return true;
}

std::vector<std::wstring> Net_Discovered() {
    std::lock_guard<std::mutex> l(g_discoLock);
    return {g_discovered.begin(), g_discovered.end()};
}
