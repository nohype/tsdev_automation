#pragma once
#include <windows.h>

#include <cstdio>

// Shared memory layout between the CLI (tsdevctl list) and the injected DLL
// (enum_hook.dll). The CLI creates the file mapping and the done event, then
// injects the DLL and calls its exported entry via CreateRemoteThread. The
// DLL opens the mapping, runs the ORELUI20.dll enumeration inside the remote
// thread, writes the records, and signals the done event.
struct EnumRecord {
    wchar_t name[256];     // verbatim stored name (always NUL-terminated)
    wchar_t progid[256];   // ProgId (always NUL-terminated; may be empty)
    unsigned short category;   // type low byte (CELTechInfo+0x08)
    unsigned short subtype;    // type high byte (CELTechInfo+0x0a)
    unsigned short record_id;  // library record id (CELTechInfo+0x0c)
};

enum EnumStatus : long {
    EnumPending = 0,
    EnumDone    = 1,
    EnumError   = 2,
};

struct EnumShared {
    volatile long count;          // number of valid records (set by DLL)
    volatile long status;         // EnumStatus (set by DLL)
    volatile long crash_flag;     // 1 if an exception occurred (set by DLL)
    volatile long truncated_flag; // 1 if capacity/field truncation occurred (set by DLL)
    EnumRecord records[8192];     // capacity 8192 (~8.4 MB; observed max 2958 items)
};

// Copies src into the fixed dst field of field_chars wchar_t (including the
// NUL). Always NUL-terminates. Returns true when src did NOT fully fit
// (truncation occurred) so the caller can set truncated_flag — user-visible
// names are never silently truncated.
inline bool copy_fixed_wstr(wchar_t* dst, size_t field_chars, const wchar_t* src) {
    size_t i = 0;
    for (; i + 1 < field_chars && src[i]; ++i) dst[i] = src[i];
    dst[i] = L'\0';
    return src[i] != L'\0';
}

// Mapping/event names. Both sides (CLI + DLL) include this header so the
// two-sided names cannot drift apart.
inline void enum_map_name(unsigned long pid, wchar_t (&buf)[128]) {
    swprintf_s(buf, L"Local\\TSDevAutomation.Enum.%lu", pid);
}

inline void enum_done_event_name(unsigned long pid, wchar_t (&buf)[128]) {
    swprintf_s(buf, L"Local\\TSDevAutomation.EnumDone.%lu", pid);
}
