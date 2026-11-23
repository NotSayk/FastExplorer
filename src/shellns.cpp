#include "shellns.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// COM sur le thread appelant, s'il ne l'est pas déjà (les tâches de fond ne l'initialisent pas toutes).
struct Com {
    bool mine;
    Com() : mine(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
    ~Com() {
        if (mine) CoUninitialize();
    }
};

std::wstring NameOf(IShellItem* it, SIGDN how) {
    PWSTR s = nullptr;
    std::wstring r;
    if (SUCCEEDED(it->GetDisplayName(how, &s)) && s) r = s;
    CoTaskMemFree(s);
    return r;
}

ComPtr<IShellItem> ItemOf(const std::wstring& path) {
    ComPtr<IShellItem> it;
    if (path.empty()) SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&it));
    else SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&it));
    return it;
}

int IconOf(IShellItem* it) {
    PIDLIST_ABSOLUTE p = nullptr;
    int icon = -1;
    if (SUCCEEDED(SHGetIDListFromObject(it, &p))) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW((LPCWSTR)p, 0, &sfi, sizeof sfi, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) icon = sfi.iIcon;
        CoTaskMemFree(p);
    }
    return icon;
}

int64_t Ft(const FILETIME& f) { return (int64_t)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime); }

bool IsComputer(PCIDLIST_ABSOLUTE p) {
    PIDLIST_ABSOLUTE pc = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pc))) return false;
    const bool same = ILIsEqual(p, pc) != FALSE;
    CoTaskMemFree(pc);
    return same;
}

// Opération du Shell (IFileOperation) sur un thread STA à part.
void RunOp(HWND owner, std::function<bool(IFileOperation*)> fill, std::function<void(bool)> done) {
    std::thread([owner, fill = std::move(fill), done = std::move(done)] {
        bool ok = false;
        {
            Com com;
            ComPtr<IFileOperation> op;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
                op->SetOwnerWindow(owner);
                op->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
                BOOL aborted = FALSE;
                ok = fill(op.Get()) && SUCCEEDED(op->PerformOperations()) &&
                     SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted;
            }
        }
        if (done) done(ok);
    }).detach();
}

ComPtr<IShellItemArray> ArrayOf(const std::vector<std::wstring>& items) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& s : items) {
        PIDLIST_ABSOLUTE p = nullptr;
        if (SUCCEEDED(SHParseDisplayName(s.c_str(), nullptr, &p, 0, nullptr)) && p) ids.push_back(p);
    }
    ComPtr<IShellItemArray> arr;
    if (!ids.empty()) SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr);
    for (auto p : ids) CoTaskMemFree(p);
    return arr;
}

}  // namespace

void ShellNs_Enumerate(DirData& d, const CancelToken& ct) {
    const double t0 = Ms();
    Com com;
    ComPtr<IShellItem> folder = ItemOf(d.path);
    ComPtr<IEnumShellItems> en;
    if (!folder || FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        d.error = ERROR_PATH_NOT_FOUND;  // appareil débranché ou verrouillé
        return;
    }
    for (;;) {
        if (ct.cancelled()) return;
        ComPtr<IShellItem> it;
        ULONG got = 0;
        if (en->Next(1, &it, &got) != S_OK || !got) break;
        SFGAOF a = 0;
        it->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_HIDDEN | SFGAO_READONLY, &a);
        std::wstring name = NameOf(it.Get(), SIGDN_PARENTRELATIVEEDITING);
        if (name.empty()) name = NameOf(it.Get(), SIGDN_NORMALDISPLAY);
        const std::wstring parsing = NameOf(it.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (name.empty() || parsing.empty()) continue;
        uint32_t attr = (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (a & SFGAO_HIDDEN) attr |= FILE_ATTRIBUTE_HIDDEN;
        if (a & SFGAO_READONLY) attr |= FILE_ATTRIBUTE_READONLY;
        uint64_t size = 0;
        int64_t mtime = 0, ctime = 0;
        if (ComPtr<IShellItem2> i2; SUCCEEDED(it.As(&i2))) {
            ULONGLONG s = 0;
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && SUCCEEDED(i2->GetUInt64(PKEY_Size, &s))) size = s;
            FILETIME f{};
            if (SUCCEEDED(i2->GetFileTime(PKEY_DateModified, &f))) mtime = Ft(f);
            if (SUCCEEDED(i2->GetFileTime(PKEY_DateCreated, &f))) ctime = Ft(f);
        }
        DirData_Add(d, name.c_str(), (uint32_t)name.size(), attr, size, mtime, ctime, 0);
        d.entries.back().icon = IconOf(it.Get());
        d.target.push_back(parsing);
        d.origin.emplace_back();
    }
    const double t1 = Ms();
    DirData_BuildKeys(d);
    d.enumMs = t1 - t0;
    d.keyMs = Ms() - t1;
}

bool ShellNs_Parent(const std::wstring& path, std::wstring& parent) {
    if (!ShellNs_IsPath(path)) return false;
    Com com;
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr)) || !p) return false;
    parent.clear();
    if (ILRemoveLastID(p) && !ILIsEmpty(p) && !IsComputer(p)) {
        PWSTR s = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(p, SIGDN_DESKTOPABSOLUTEPARSING, &s)) && s) parent = s;
        CoTaskMemFree(s);
    }
    CoTaskMemFree(p);
    return true;
}

std::wstring ShellNs_Title(const std::wstring& path) {
    Com com;
    ComPtr<IShellItem> it = ItemOf(path);
    return it ? NameOf(it.Get(), SIGDN_NORMALDISPLAY) : std::wstring();
}

std::vector<ShellNsCrumb> ShellNs_Chain(const std::wstring& path) {
    std::vector<ShellNsCrumb> out;
    Com com;
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr)) || !p) return out;
    bool underPc = false;
    while (!ILIsEmpty(p)) {
        if (IsComputer(p)) {
            underPc = true;
            break;
        }
        PWSTR name = nullptr, parsing = nullptr;
        SHGetNameFromIDList(p, SIGDN_NORMALDISPLAY, &name);
        SHGetNameFromIDList(p, SIGDN_DESKTOPABSOLUTEPARSING, &parsing);
        out.insert(out.begin(), {name ? name : L"", parsing ? parsing : L""});
        CoTaskMemFree(name);
        CoTaskMemFree(parsing);
        if (!ILRemoveLastID(p)) break;
    }
    CoTaskMemFree(p);
    if (underPc) out.insert(out.begin(), {L"Ce PC", L""});
    return out;
}

std::vector<ShellNsDevice> ShellNs_Devices() {
    std::vector<ShellNsDevice> out;
    Com com;
    ComPtr<IShellItem> pc = ItemOf(L"");
    ComPtr<IEnumShellItems> en;
    if (!pc || FAILED(pc->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) return out;
    for (;;) {
        ComPtr<IShellItem> it;
        ULONG got = 0;
        if (en->Next(1, &it, &got) != S_OK || !got) break;
        SFGAOF a = 0;
        it->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &a);
        if ((a & SFGAO_FILESYSTEM) || !(a & SFGAO_FOLDER)) continue;  // lecteurs (et dossiers de l'utilisateur)
        const std::wstring parsing = NameOf(it.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        // Appareils : « ::{20D04FE0-…}\\?\usb#… » (WPD). Le reste (dossiers « ::{GUID} » de Ce PC) est ignoré.
        if (parsing.find(L"\\\\?\\") == std::wstring::npos) continue;
        out.push_back({parsing, NameOf(it.Get(), SIGDN_NORMALDISPLAY), IconOf(it.Get())});
    }
    return out;
}

bool ShellNs_Open(HWND owner, const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr)) || !p) return false;
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
    sei.hwnd = owner;
    sei.lpIDList = p;
    sei.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&sei) != FALSE;
    CoTaskMemFree(p);
    return ok;
}

void ShellNs_Paste(HWND owner, const std::wstring& folder, std::function<void(bool)> done) {
    RunOp(owner, [folder](IFileOperation* op) {
        ComPtr<IDataObject> data;
        ComPtr<IShellItem> dest = ItemOf(folder);
        return SUCCEEDED(OleGetClipboard(&data)) && dest && SUCCEEDED(op->CopyItems(data.Get(), dest.Get()));
    }, std::move(done));
}

void ShellNs_CopyInto(HWND owner, const std::vector<std::wstring>& items, const std::wstring& folder,
                      std::function<void(bool)> done) {
    RunOp(owner, [items, folder](IFileOperation* op) {
        ComPtr<IShellItemArray> arr = ArrayOf(items);
        ComPtr<IShellItem> dest = ItemOf(folder);
        return arr && dest && SUCCEEDED(op->CopyItems(arr.Get(), dest.Get()));
    }, std::move(done));
}

void ShellNs_Delete(HWND owner, const std::vector<std::wstring>& items, std::function<void(bool)> done) {
    RunOp(owner, [items](IFileOperation* op) {
        ComPtr<IShellItemArray> arr = ArrayOf(items);
        return arr && SUCCEEDED(op->DeleteItems(arr.Get()));
    }, std::move(done));
}

void ShellNs_Rename(HWND owner, const std::wstring& item, const std::wstring& newName, std::function<void(bool)> done) {
    RunOp(owner, [item, newName](IFileOperation* op) {
        ComPtr<IShellItem> it = ItemOf(item);
        return it && SUCCEEDED(op->RenameItem(it.Get(), newName.c_str(), nullptr));
    }, std::move(done));
}
