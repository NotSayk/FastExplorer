#include "nt.h"

NtQueryDirectoryFile_t pNtQueryDirectoryFile;
RtlNtStatusToDosError_t pRtlNtStatusToDosError;

bool LoadNt() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    pNtQueryDirectoryFile = (NtQueryDirectoryFile_t)GetProcAddress(nt, "NtQueryDirectoryFile");
    pRtlNtStatusToDosError = (RtlNtStatusToDosError_t)GetProcAddress(nt, "RtlNtStatusToDosError");
    return pNtQueryDirectoryFile && pRtlNtStatusToDosError;
}
