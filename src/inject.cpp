// inject.cpp - Inject a DLL into a running process
// Usage: inject.exe <PID> <DLL_PATH>
// Build: cmake --build build --config Release --target inject

#include <cstdio>
#include <cstring>
#include <windows.h>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <PID> <DLL_PATH>\n", argv[0]);
        return 1;
    }

    DWORD pid = (DWORD)atoi(argv[1]);
    const char* dllPath = argv[2];

    printf("Injecting DLL '%s' into PID %lu\n", dllPath, pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return 2;
    }

    size_t pathLen = strlen(dllPath) + 1;
    void* remoteMem = VirtualAllocEx(hProcess, NULL, pathLen, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) {
        printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(hProcess);
        return 3;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathLen, NULL)) {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 4;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(hKernel32, "LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, NULL);
    if (!hThread) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 5;
    }

    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    printf("LoadLibraryA returned %lu (0x%p)\n", exitCode, (void*)exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (exitCode) {
        printf("DLL loaded successfully\n");
        return 0;
    } else {
        printf("DLL load failed\n");
        return 6;
    }
}
