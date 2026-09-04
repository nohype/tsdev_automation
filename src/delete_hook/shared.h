#pragma once
#include <windows.h>

// Shared memory layout between the CLI (tsdevctl) and the injected DLL.
// The CLI creates the file mapping, fills in the target name and addresses,
// then injects the DLL. The DLL opens the mapping in DllMain, installs the
// inline hook, and writes status/crash_flag when the hook fires.
struct DeleteShared {
    wchar_t target_name[256];    // Item name to match (set by CLI)
    void*   fn_open;             // Address of FUN_004944b0 (set by CLI)
    void*   fn_delete;           // Address of FUN_0044c0f0 (set by CLI)
    int     modified_offset;     // 0x17c (set by CLI)
    int     vtable_close_offset; // 0x8c (set by CLI)
    volatile long status;        // 0=pending, 1=done, 2=error (set by DLL)
    volatile long crash_flag;    // 1 if an exception occurred in the hook (set by DLL)
};

enum DeleteStatus : long {
    DeletePending = 0,
    DeleteDone    = 1,
    DeleteError   = 2,
};
