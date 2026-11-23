#include "defexp.h"

#include <shlobj.h>

namespace {

constexpr wchar_t kVerb[] = L"FastExplorer";
constexpr wchar_t kWinE[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";
constexpr wchar_t kPrev[] = L"Software\\FastExplorer\\DefaultPrev";
const wchar_t* const kKinds[] = {L"Directory", L"Drive"};

bool ReadString(HKEY root, const std::wstring& key, const wchar_t* value, std::wstring& out) {
    wchar_t buf[2048] = {};
    DWORD cb = sizeof buf;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &cb) !=
        ERROR_SUCCESS)
        return false;
    out = buf;
    return true;
}

bool KeyExists(const std::wstring& key) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

// Clé sans valeur, dont les sous-clés sont elles aussi vides.
bool EmptyTree(const std::wstring& key) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD subs = 0, values = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    bool empty = values == 0;
    for (DWORD i = 0; empty && i < subs; ++i) {
        wchar_t name[256];
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        empty = EmptyTree(key + L"\\" + name);
    }
    RegCloseKey(k);
    return empty;
}

LSTATUS WriteString(const std::wstring& key, const wchar_t* value, const std::wstring& data) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), value, REG_SZ, data.c_str(),
                           (DWORD)((data.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

std::wstring DefExp_Command(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }

bool DefExp_IsOn() {
    std::wstring v;
    return ReadString(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell", nullptr, v) && v == kVerb &&
           ReadString(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FastExplorer\\command", nullptr, v);
}

LSTATUS DefExp_Set(bool on, const std::wstring& exe) {
    LSTATUS r = ERROR_SUCCESS;
    if (on) {
        for (const wchar_t* kind : kKinds) {
            const std::wstring shell = std::wstring(L"Software\\Classes\\") + kind + L"\\shell";
            std::wstring prev;
            const bool had = ReadString(HKEY_CURRENT_USER, shell, nullptr, prev);
            if (!(had && prev == kVerb)) {  // déjà en place : l'ancien est déjà gardé
                if (had) WriteString(kPrev, kind, prev);
                else RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrev, kind);
                // Clé que nous allons créer (« Classes\Drive » ou son « shell ») : retirée à l'arrêt si elle est vide.
                const std::wstring cls = std::wstring(L"Software\\Classes\\") + kind;
                const std::wstring created = !KeyExists(cls) ? cls : !KeyExists(shell) ? shell : L"";
                const std::wstring createdName = std::wstring(kind) + L".created";
                if (!created.empty()) WriteString(kPrev, createdName.c_str(), created);
                else RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrev, createdName.c_str());
            }
            if ((r = WriteString(shell + L"\\FastExplorer", nullptr, L"Ouvrir avec FastExplorer"))) return r;
            if ((r = WriteString(shell + L"\\FastExplorer\\command", nullptr, DefExp_Command(exe)))) return r;
            if ((r = WriteString(shell, nullptr, kVerb))) return r;
        }
        // Win+E (et « Ce PC » ouvert par le Shell) : DelegateExecute vide pour que la commande soit prise.
        const std::wstring cmd = std::wstring(kWinE) + L"\\shell\\opennewwindow\\command";
        if ((r = WriteString(cmd, nullptr, L"\"" + exe + L"\""))) return r;
        if ((r = WriteString(cmd, L"DelegateExecute", L""))) return r;
    } else {
        for (const wchar_t* kind : kKinds) {
            const std::wstring shell = std::wstring(L"Software\\Classes\\") + kind + L"\\shell";
            RegDeleteTreeW(HKEY_CURRENT_USER, (shell + L"\\FastExplorer").c_str());
            std::wstring cur, prev;
            if (ReadString(HKEY_CURRENT_USER, shell, nullptr, cur) && cur == kVerb) {
                if (ReadString(HKEY_CURRENT_USER, kPrev, kind, prev)) WriteString(shell, nullptr, prev);
                else RegDeleteKeyValueW(HKEY_CURRENT_USER, shell.c_str(), nullptr);
            }
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrev, kind);
            // Clé créée par nous et restée vide : retirée (sinon gardée : d'autres verbes peuvent y être).
            const std::wstring createdName = std::wstring(kind) + L".created";
            std::wstring created;
            if (ReadString(HKEY_CURRENT_USER, kPrev, createdName.c_str(), created) &&
                created.rfind(L"Software\\Classes\\", 0) == 0 && EmptyTree(created))
                RegDeleteTreeW(HKEY_CURRENT_USER, created.c_str()), RegDeleteKeyW(HKEY_CURRENT_USER, created.c_str());
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrev, createdName.c_str());
        }
        RegDeleteKeyW(HKEY_CURRENT_USER, kPrev);
        // Win+E : seulement si c'est bien la nôtre.
        std::wstring cmd;
        const std::wstring key = std::wstring(kWinE) + L"\\shell\\opennewwindow\\command";
        if (ReadString(HKEY_CURRENT_USER, key, nullptr, cmd) && cmd.find(L"FastExplorer") != std::wstring::npos)
            RegDeleteTreeW(HKEY_CURRENT_USER, kWinE), RegDeleteKeyW(HKEY_CURRENT_USER, kWinE);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return r;
}
