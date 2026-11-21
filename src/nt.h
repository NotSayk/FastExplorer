#pragma once
#include "win.h"

#include <winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#ifndef STATUS_NO_MORE_FILES
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif

using NtQueryDirectoryFile_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
using RtlNtStatusToDosError_t = ULONG(NTAPI*)(NTSTATUS);

extern NtQueryDirectoryFile_t pNtQueryDirectoryFile;
extern RtlNtStatusToDosError_t pRtlNtStatusToDosError;

// FILE_DIRECTORY_INFORMATION (classe 1) — non exposée par winternl.h.
struct DirInfo {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;  // en octets
    WCHAR FileName[1];
};

bool LoadNt();
