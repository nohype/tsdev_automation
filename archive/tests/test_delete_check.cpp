// test_delete_check.cpp — quick check if deleted entries persist across restarts
#include "tsdev_lib.h"
#include <cstdio>
using namespace tsdev;

DWORD WINAPI runCheck(LPVOID p) {
    int* exitCode = (int*)p;
    TsDevLib* lib = nullptr;
    try { lib = new TsDevLib(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "init failed: %s\n", e.what());
        *exitCode = 1; return 0;
    }
    int r1 = lib->findExistPos("zzz_delete_trace", ComponentType::Strategy);
    int r2 = lib->findExistPos("zzz_cli_delete_test", ComponentType::Strategy);
    int r3 = lib->findExistPos("zzz_delete_probe", ComponentType::Strategy);
    std::fprintf(stdout, "zzz_delete_trace (deleted via TSDev UI) = %d\n", r1);
    std::fprintf(stdout, "zzz_cli_delete_test (our test entry) = %d\n", r2);
    std::fprintf(stdout, "zzz_delete_probe (from crashed probe) = %d\n", r3);
    delete lib;
    *exitCode = 0;
    return 0;
}

int main() {
    int exitCode = 0;
    HANDLE h = CreateThread(nullptr, 64*1024*1024, runCheck, &exitCode, 0, nullptr);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return exitCode;
}
