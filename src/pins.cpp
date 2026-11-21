#include "pins.h"

#include <shlobj.h>

#include <mutex>

namespace {

std::mutex g_mutex;  // fichiers et g_dir
std::wstring g_dir;  // "" : %APPDATA%\FastExplorer

std::wstring DirLocked() {
    std::wstring dir = g_dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) {
            dir = p;
        } else {
            wchar_t buf[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) dir = buf;
        }
        CoTaskMemFree(p);
        if (dir.empty()) return dir;
        dir += L"\\FastExplorer";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::vector<SavedPin> Load(const wchar_t* file) {
    std::vector<SavedPin> out;
    const std::wstring dir = DirLocked();
    if (dir.empty()) return out;
    HANDLE h = CreateFileW((dir + L"\\" + file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    std::string bytes;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr)) got = 0;
        bytes.resize(got);
    }
    CloseHandle(h);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    std::wstring text(bytes.size(), L'\0');
    text.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), (int)text.size()));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        SavedPin p;
        const size_t tab = line.find(L'\t');
        p.path = line.substr(0, tab);
        if (tab != std::wstring::npos) p.text = line.substr(tab + 1);
        if (!p.path.empty()) out.push_back(std::move(p));
    }
    return out;
}

// Écriture dans un fichier temporaire puis remplacement : jamais de fichier à moitié écrit.
bool Save(const wchar_t* file, const std::vector<SavedPin>& pins) {
    const std::wstring dir = DirLocked();
    if (dir.empty()) return false;
    std::wstring text;
    for (const auto& p : pins) {
        text += p.path;
        if (!p.text.empty()) text += L"\t" + p.text;
        text += L"\r\n";
    }
    std::string bytes;
    if (!text.empty()) {
        bytes.resize((size_t)WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr));
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), bytes.data(), (int)bytes.size(), nullptr, nullptr);
    }
    const std::wstring final = dir + L"\\" + file, tmp = final + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = (bytes.empty() || WriteFile(h, bytes.data(), (DWORD)bytes.size(), &put, nullptr)) && put == bytes.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

std::wstring Pins_Dir() {
    std::lock_guard lock(g_mutex);
    return DirLocked();
}

void Pins_SetDir(const std::wstring& dir) {
    std::lock_guard lock(g_mutex);
    g_dir = dir;
}

std::vector<SavedPin> Pins_LoadOwn() {
    std::lock_guard lock(g_mutex);
    return Load(L"pins.txt");
}

bool Pins_IsOwn(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    for (const auto& p : Load(L"pins.txt"))
        if (SamePath(p.path, path)) return true;
    return false;
}

bool Pins_AddOwn(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    auto pins = Load(L"pins.txt");
    for (const auto& p : pins)
        if (SamePath(p.path, path)) return true;
    pins.push_back({path, {}});
    return Save(L"pins.txt", pins);
}

bool Pins_RemoveOwn(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    auto pins = Load(L"pins.txt");
    const size_t before = pins.size();
    std::erase_if(pins, [&](const SavedPin& p) { return SamePath(p.path, path); });
    return pins.size() == before || Save(L"pins.txt", pins);
}

std::vector<SavedPin> Pins_LoadSnapshot() {
    std::lock_guard lock(g_mutex);
    return Load(L"shellpins.txt");
}

bool Pins_SaveSnapshot(const std::vector<SavedPin>& pins) {
    std::lock_guard lock(g_mutex);
    const auto cur = Load(L"shellpins.txt");
    bool same = cur.size() == pins.size();
    for (size_t i = 0; same && i < pins.size(); ++i) same = cur[i].path == pins[i].path && cur[i].text == pins[i].text;
    return same || Save(L"shellpins.txt", pins);
}
