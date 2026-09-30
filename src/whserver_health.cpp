// whserver_health.cpp
//
// Implementation of the WHServer.exe VAS-headroom helpers declared in
// whserver_health.h. Pure Win32 (kernel32 + toolhelp32); see the header for
// the contract.

#include "whserver_health.h"

#include <tlhelp32.h>

namespace tsdev {

std::vector<DWORD> findWhServerPids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"WHServer.exe") == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

bool measureVasFree(DWORD pid, WhVasStats& out) {
    out = WhVasStats{};

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProc == nullptr) return false;

    // User VAS ceiling for a non-LARGEADDRESSAWARE 32-bit process.
    const uintptr_t kUserVaLimit = 0x80000000ULL;

    bool ok = true;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < kUserVaLimit) {
        SIZE_T n = VirtualQueryEx(hProc, reinterpret_cast<LPCVOID>(addr),
                                  &mbi, sizeof(mbi));
        if (n == 0) {
            // End of the queryable address space (or a failure at the very
            // first probe). A first-probe failure is reported as false; a
            // mid-walk failure just ends the walk with what we have.
            ok = (addr != 0);
            break;
        }
        if (mbi.State == MEM_FREE) {
            uint64_t size = static_cast<uint64_t>(mbi.RegionSize);
            // Clip the last region at the 2GB boundary if it straddles it.
            if (addr + size > kUserVaLimit) {
                size = kUserVaLimit - addr;
            }
            out.totalFreeBytes += size;
            if (size > out.largestFreeBlockBytes) {
                out.largestFreeBlockBytes = size;
            }
        }
        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                         static_cast<uintptr_t>(mbi.RegionSize);
        if (next <= addr) break;  // defensive: never loop in place
        addr = next;
    }

    CloseHandle(hProc);
    return ok;
}

} // namespace tsdev
