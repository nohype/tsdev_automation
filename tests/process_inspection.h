#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>

namespace diag_m4_process {

enum class ProcessInspectionResult {
    Found,
    NotFound,
    InspectionFailure,
};

// Enumerate the current Windows process snapshot and compare executable names
// without invoking a shell. Process32First/Next failures other than the normal
// end-of-snapshot condition are inspection failures, not evidence of absence.
inline ProcessInspectionResult inspectProcessByName(const wchar_t* executableName) {
    if (executableName == nullptr || *executableName == L'\0') {
        return ProcessInspectionResult::InspectionFailure;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ProcessInspectionResult::InspectionFailure;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        const DWORD error = GetLastError();
        CloseHandle(snapshot);
        return error == ERROR_NO_MORE_FILES
                   ? ProcessInspectionResult::NotFound
                   : ProcessInspectionResult::InspectionFailure;
    }

    for (;;) {
        if (_wcsicmp(entry.szExeFile, executableName) == 0) {
            CloseHandle(snapshot);
            return ProcessInspectionResult::Found;
        }

        if (!Process32NextW(snapshot, &entry)) {
            const DWORD error = GetLastError();
            CloseHandle(snapshot);
            return error == ERROR_NO_MORE_FILES
                       ? ProcessInspectionResult::NotFound
                       : ProcessInspectionResult::InspectionFailure;
        }
    }
}

} // namespace diag_m4_process
