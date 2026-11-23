#include "vdisk.h"

#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <cwctype>

#pragma comment(lib, "virtdisk.lib")

namespace {

bool EndsWithI(const std::wstring& s, const wchar_t* ext) {
    const size_t n = wcslen(ext);
    return s.size() > n && _wcsicmp(s.c_str() + s.size() - n, ext) == 0;
}

// Numéro de périphérique d'un chemin physique (« \\.\CdRom1 », « \\.\PhysicalDrive3 ») ou d'un volume (« \\.\E: »).
bool DeviceNumber(const std::wstring& dev, STORAGE_DEVICE_NUMBER& out) {
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &out, sizeof out, &got, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

// Lettre d'un lecteur porté par ce périphérique ("" : pas encore).
std::wstring RootOf(const STORAGE_DEVICE_NUMBER& want) {
    const UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::wstring found;
    const DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26 && found.empty(); ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t vol[] = {L'\\', L'\\', L'.', L'\\', (wchar_t)(L'A' + i), L':', 0};
        STORAGE_DEVICE_NUMBER n{};
        if (DeviceNumber(vol, n) && n.DeviceType == want.DeviceType && n.DeviceNumber == want.DeviceNumber)
            found = {(wchar_t)(L'A' + i), L':', L'\\'};
    }
    SetErrorMode(old);
    return found;
}

}  // namespace

bool VDisk_IsImage(const std::wstring& name) {
    return EndsWithI(name, L".iso") || EndsWithI(name, L".img") || EndsWithI(name, L".vhd") || EndsWithI(name, L".vhdx");
}

DWORD VDisk_Mount(const std::wstring& image, std::wstring& root, DWORD timeoutMs) {
    root.clear();
    const bool iso = EndsWithI(image, L".iso") || EndsWithI(image, L".img");
    VIRTUAL_STORAGE_TYPE type{};
    type.DeviceId = iso ? VIRTUAL_STORAGE_TYPE_DEVICE_ISO : VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;
    if (iso) type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
    OPEN_VIRTUAL_DISK_PARAMETERS op{};
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    HANDLE h = nullptr;
    const VIRTUAL_DISK_ACCESS_MASK access =
        iso ? (VIRTUAL_DISK_ACCESS_MASK)(VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_GET_INFO)
            : VIRTUAL_DISK_ACCESS_ALL;
    DWORD r = OpenVirtualDisk(&type, image.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE, iso ? nullptr : &op, &h);
    if (r != ERROR_SUCCESS) return r;
    ATTACH_VIRTUAL_DISK_PARAMETERS ap{};
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    const ATTACH_VIRTUAL_DISK_FLAG flags = (ATTACH_VIRTUAL_DISK_FLAG)(
        ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME | (iso ? ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY : 0));
    r = AttachVirtualDisk(h, nullptr, flags, 0, &ap, nullptr);
    // Déjà montée : on retrouve simplement son lecteur.
    const bool attached = r == ERROR_SUCCESS || r == ERROR_SHARING_VIOLATION || r == ERROR_FILE_EXISTS ||
                          r == 0xC03A001C /* VIRTDISK déjà attaché */;
    wchar_t phys[MAX_PATH] = {};
    ULONG cb = sizeof phys;
    // Le chemin physique n'existe qu'une fois le disque attaché : on le redemande un moment.
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    DWORD pr = ERROR_NOT_READY;
    while ((pr = GetVirtualDiskPhysicalPath(h, &cb, phys)) != ERROR_SUCCESS && GetTickCount64() < until) {
        cb = sizeof phys;
        Sleep(100);
    }
    CloseHandle(h);
    if (pr != ERROR_SUCCESS) return attached ? ERROR_SUCCESS : r;
    STORAGE_DEVICE_NUMBER want{};
    bool known = false;
    while (GetTickCount64() < until) {
        if (!known) known = DeviceNumber(phys, want);
        if (known && !(root = RootOf(want)).empty()) break;
        Sleep(150);
    }
    return attached || !root.empty() ? ERROR_SUCCESS : r;
}
