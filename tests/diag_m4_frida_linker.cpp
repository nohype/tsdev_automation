// diag_m4_frida_linker.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: drive the orlib20 linker (FUN_101a3340, ordinal 85) from our own
// CLI process while a historical/transient Frida script
// (tmp/frida_m4_linker_trace.js; untracked and unavailable in a fresh checkout)
// traces the linker's internals (object-code buffer, decompressor, 0xBBC magic check,
// populators, commit). This answers WHY the linker returns a non-NULL record
// with EMPTY element arrays.
//
// Flow:
//   1. Print PID and "READY FOR FRIDA".
//   2. Wait for a Frida attach (sleep 8 seconds — Frida CLI attaches in
//      spawn mode, but we also support attach mode by sleeping here so the
//      operator can attach to the running PID).
//   3. Construct a TsDevLib (test-only ctor, real COM, no hooks) on a 64 MB
//      stack worker thread.
//   4. Resolve Ordinal_28 (partial copy), ordinal 85 (linker), Ordinal_70
//      (refcount-dec).
//   5. Insert + verify a Function "TS_AUTO_M4_FL_FUNC" with source
//      `TS_AUTO_M4_FL_FUNC = 1;`.
//   6. Get the partial record via Ordinal_28(4, funcProgId, 0, buf).
//   7. Call the linker: linker(4, funcProgId, 0, buf) — manual inline-asm
//      wrapper so the call is robust to either __stdcall (RET 0x10) or
//      __fastcall (RET 0x8) cleanup, AND loads ECX=type, EDX=id.
//   8. If non-NULL, read record fields (+0x00, +0x26, +0x29, +0x4f, +0x55)
//      and release via Ordinal_70.
//   9. Delete the Function.
//  10. Check for TSCrashReport.exe (pre/post).
//
// All orlib20 calls are SEH-protected. Read-only instrumentation; no
// production code is modified.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

using namespace tsdev;
using diag_m4_process::ProcessInspectionResult;

namespace {

ProcessInspectionResult reportCrashReporter(const char* phase) {
    std::printf("[%s] Checking for TSCrashReport.exe...\n", phase);
    const ProcessInspectionResult result =
        diag_m4_process::inspectProcessByName(L"TSCrashReport.exe");
    if (result == ProcessInspectionResult::Found) {
        std::printf("[%s] TSCrashReport.exe IS running (crash report detected)\n",
                    phase);
    } else if (result == ProcessInspectionResult::NotFound) {
        std::printf("[%s] TSCrashReport.exe NOT running (no crash report detected)\n",
                    phase);
    } else {
        std::fprintf(stderr,
                     "[%s] FATAL: process enumeration inspection failure for "
                     "TSCrashReport.exe\n",
                     phase);
    }
    return result;
}

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args (rec, flag). RET 0x8.
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args (category, progId, flag,
//   outBuf). RET 0x10. Writes a 0x10A-byte partial copy into outBuf.
// Linker FUN_101a3340 (ordinal 85): __fastcall — ECX=type, EDX=progId,
//   stack: errorFlag (int), partialBuf (void*). We use the manual inline-asm
//   wrapper from diag_m4_linker_internals.cpp so the call is robust to
//   either RET 0x10 (stdcall) or RET 0x8 (fastcall) cleanup, AND we load
//   ECX=type, EDX=id so the call is valid under BOTH conventions.
// -------------------------------------------------------------------------
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef void* (__stdcall *LinkerStdcallFn)(int type, int id, int errorFlag,
                                           void* partialBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct Ord70SehResult { int result; };
bool callOrd70(Ordinal70Fn fn, void* rec, int flag,
               Ord70SehResult* out) {
    out->result = -1;
    __try { out->result = fn(rec, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord28SehResult { unsigned int result; };
bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try { out->result = fn(category, progId, flag, outBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// callLinkerManual — calls the linker with ALL 4 args on the stack
// (as __stdcall would) AND loads ECX=type, EDX=id (as __fastcall would),
// using inline assembly so we can manually restore ESP regardless of whether
// the callee did RET 0x10 (stdcall, popped 16) or RET 0x8 (fastcall, popped
// 8). This makes the call robust to calling-convention mismatch.
//
// NOTE: This function is NOT itself SEH-protected (inline asm + SEH in the
// same function is rejected by MSVC C2712/C2181). The caller wraps it.
void* callLinkerManual(void* fn, int whType, int whId, int whErrFlag,
                       void* whPartialBuf) {
    void* retVal = nullptr;
    __asm {
        mov  esi, esp
        push whPartialBuf
        push whErrFlag
        push whId
        push whType
        mov  ecx, whType
        mov  edx, whId
        call fn
        mov  retVal, eax
        mov  esp, esi
    }
    return retVal;
}

struct LinkerSehResult { void* ptr; bool seh; };
bool callLinkerSafe(void* fn, int type, int id, int errorFlag,
                    void* partialBuf, LinkerSehResult* out) {
    out->ptr = nullptr;
    out->seh = false;
    __try {
        out->ptr = callLinkerManual(fn, type, id, errorFlag, partialBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->seh = true;
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// SEH-protected raw readers. POD-only so __try/__except is legal.
// -------------------------------------------------------------------------
bool sehReadInt(const void* p, int* out) {
    *out = 0;
    __try { *out = *(const int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadShort(const void* p, short* out) {
    *out = 0;
    __try { *out = *(const short*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadByte(const void* p, unsigned char* out) {
    *out = 0;
    __try { *out = *(const unsigned char*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehReadPtr(const void* p, void** out) {
    *out = nullptr;
    __try { *out = *(void* const*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// -------------------------------------------------------------------------
// Real ComApi — binds the actual CoInitializeEx / CoUninitialize so the
// test-only ctor enters a real STA apartment (required for orlib20 / OLE).
// -------------------------------------------------------------------------
ComApi realComApi() {
    return ComApi{&CoInitializeEx, &CoUninitialize};
}

// -------------------------------------------------------------------------
// EasyLanguage source builder.
// -------------------------------------------------------------------------
std::string makeFunctionSource(const std::string& name) {
    return name + " = 1;\r\n";
}

void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
}

void preCleanup(TsDevLib& lib, ComponentType type, const std::string& name) {
    int existingProgId = lib.findExistPos(name, type);
    if (existingProgId > 0) {
        std::printf("  Pre-cleanup: deleting existing %s (ProgId=%d)\n",
                    name.c_str(), existingProgId);
        lib.deleteEntry(type, name, /*flush=*/false);
    }
}

int insertAndVerify(TsDevLib& lib, ComponentType type,
                    const std::string& name, const std::string& source,
                    const char* label) {
    preCleanup(lib, type, name);

    int errCode = 0;
    int progId = lib.createEntry(type, name, errCode);
    std::printf("      createEntry(%s, \"%s\") = %d, errCode = 0x%X\n",
                label, name.c_str(), progId, errCode);
    if (progId <= 0) {
        fail("createEntry returned ProgId <= 0");
        return 0;
    }

    if (!lib.saveSource(type, progId, source)) {
        std::fprintf(stderr, "      saveSource failed for %s\n",
                     name.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("saveSource returned false");
        return 0;
    }

    std::string errors = lib.verifyModule(type, progId);
    if (!errors.empty()) {
        std::fprintf(stderr,
                     "      verifyModule errors for %s:\n%s\n",
                     name.c_str(), errors.c_str());
        lib.deleteEntry(type, name, /*flush=*/false);
        fail("verifyModule returned compilation errors");
        return 0;
    }
    std::printf("      verifyModule(%s, %d): OK\n", label, progId);
    return progId;
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName = "TS_AUTO_M4_FL_FUNC";

    std::printf("=== diag_m4_frida_linker: linker internal trace driver ===\n");
    std::printf("Purpose: drive the linker (ordinal 85) while Frida traces "
                "the object-code buffer,\n");
    std::printf("  decompressor, 0xBBC magic check, populators, and commit. "
                "Answers WHY the\n");
    std::printf("  linker returns a non-NULL record with empty element "
                "arrays.\n");

    // --- Step 1: construct a real TsDevLib (full init). No hooks. ---
    std::printf("\n[1] Constructing TsDevLib (test-only ctor, real ComApi, "
                "no hooks)...\n");
    std::unique_ptr<TsDevLib> lib;
    try {
        TsDevLibTestConfig cfg;
        cfg.comApi        = realComApi();
        cfg.throwAfterCom = false;
        lib = tsdev::makeTsDevLibForTest(/*tsPathOverride=*/nullptr, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: TsDevLib construction threw: %s\n",
                     e.what());
        st->exitCode = 100;
        return 0;
    }
    if (!lib) {
        std::fprintf(stderr, "FATAL: makeTsDevLibForTest returned null\n");
        st->exitCode = 101;
        return 0;
    }
    std::printf("    TsDevLib constructed OK.\n");

    // --- Step 2: resolve exports. ---
    std::printf("\n[2] Resolving Ordinal_28 (28), Ordinal_70 (70), and "
                "ordinal 85 (linker FUN_101a3340)...\n");
    Ordinal28Fn ord28 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    void* linkerRaw = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) linkerRaw = (void*)ord85;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    if (!ord28) std::fprintf(stderr, "WARN: Ordinal_28 unresolved\n");
    if (!ord70) std::fprintf(stderr, "WARN: Ordinal_70 unresolved\n");
    if (!linkerRaw) {
        std::fprintf(stderr, "FATAL: ordinal 85 (linker) unresolved\n");
        st->exitCode = 103;
        return 0;
    }
    std::printf("    Resolved: Ord28=%p Ord70=%p Linker(85)=%p\n",
                (void*)ord28, (void*)ord70, linkerRaw);

    // --- Step 3: insert + verify the Function. ---
    std::printf("\n[3] Insert + verify Function \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) { st->exitCode = 200; return 0; }
    std::printf("    Function ProgId = %d\n", funcProgId);

    // --- Step 4: get the partial record via Ordinal_28. ---
    std::printf("\n[4] Getting partial record via "
                "Ordinal_28(4, %d, 0, buf)...\n", funcProgId);
    unsigned char partialBuf[0x10A] = {};
    bool havePartial = false;
    if (ord28) {
        Ord28SehResult r;
        bool ok = callOrd28(ord28, /*category=*/4, funcProgId,
                            /*flag=*/0, partialBuf, &r);
        std::printf("    Ordinal_28(4, %d, 0, buf): %s, ret=%u\n",
                    funcProgId, ok ? "OK" : "SEH", r.result);
        if (ok && r.result == 1) {
            havePartial = true;
            void* sa = nullptr;  sehReadPtr(partialBuf + 0x26, &sa);
            int ec = 0;          sehReadInt (partialBuf + 0x29, &ec);
            void* eap = nullptr; sehReadPtr(partialBuf + 0x4f, &eap);
            std::printf("    Partial copy: +0x26 SafeArray=%p  +0x29 "
                        "elemCount=%d  +0x4f elemArray=%p\n",
                        sa, ec, eap);
        }
    }

    // --- Step 5: call the linker (ordinal 85). ---
    std::printf("\n[5] Calling linker(4, %d, 0, partialBuf=%p)...\n",
                funcProgId, havePartial ? (void*)partialBuf : nullptr);
    LinkerSehResult r;
    bool ok = callLinkerSafe(linkerRaw, /*type=*/4, funcProgId,
                             /*errorFlag=*/0,
                             havePartial ? partialBuf : nullptr, &r);
    std::printf("    linker call: %s, rec=%p%s\n",
                ok ? "OK (no SEH)" : "SEH raised", r.ptr,
                r.seh ? " [SEH]" : "");

    // --- Step 6: read record fields and release. ---
    if (ok && r.ptr) {
        const unsigned char* rec = (const unsigned char*)r.ptr;
        short   firstShort = 0; sehReadShort(rec + 0x00, &firstShort);
        void*   sa = nullptr;   sehReadPtr  (rec + 0x26, &sa);
        int     ec = 0;         sehReadInt  (rec + 0x29, &ec);
        void*   eap = nullptr;  sehReadPtr  (rec + 0x4f, &eap);
        unsigned char vf = 0;   sehReadByte (rec + 0x55, &vf);
        int     rc = 0;         sehReadInt  (rec + 0xCA, &rc);
        std::printf("\n  --- LINKED RECORD FIELDS ---\n");
        std::printf("    +0x00 first short (ProgId?): %d (0x%04X)  "
                    "[expected %d]\n",
                    firstShort, (unsigned short)firstShort, funcProgId);
        std::printf("    +0x26 SafeArray ptr: %p  [%s]\n",
                    sa, sa ? "NON-NULL" : "NULL");
        std::printf("    +0x29 element count: %d (0x%X)  [%s]\n",
                    ec, (unsigned)ec, ec == 0 ? "ZERO" : "NONZERO");
        std::printf("    +0x4f element array ptr: %p  [%s]\n",
                    eap, eap ? "NON-NULL" : "NULL");
        std::printf("    +0x55 verified flag: 0x%02X (%u)\n",
                    (unsigned)vf, (unsigned)vf);
        std::printf("    +0xCA refcount: %d (0x%X)\n",
                    rc, (unsigned)rc);
        std::printf("    >> element arrays %s\n",
                    (ec > 0 && eap) ? "POPULATED" : "EMPTY (NULL/zero)");
        if (ord70) {
            Ord70SehResult r70;
            bool ok70 = callOrd70(ord70, r.ptr, 0, &r70);
            std::printf("    [release] Ordinal_70(rec, 0): %s, ret=%d\n",
                        ok70 ? "OK" : "SEH", r70.result);
        }
    } else {
        std::printf("    [linker returned no record — nothing to read]\n");
    }

    // --- Step 7: cleanup. ---
    std::printf("\n[7] Cleanup: deleting Function \"%s\"...\n",
                funcName.c_str());
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    std::printf("\n=== DIAG COMPLETE ===\n");
    st->exitCode = 0;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Print PID + READY banner so Frida (attach mode) can target us.
    std::printf("READY FOR FRIDA\n");
    std::printf("PID=%lu\n", GetCurrentProcessId());

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was "
                     "terminated.\n");
        return 97;
    }

    // Give Frida time to attach (attach mode). In spawn mode this sleep is
    // harmless — Frida injects before main runs anyway.
    std::printf("Sleeping 8 seconds to allow Frida attach...\n");
    Sleep(8000);
    std::printf("Proceeding with orlib20 calls.\n");

    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 120-second hard timeout — report a hang without terminating the worker.
    DWORD wait = WaitForSingleObject(hThread, 120000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 120s; worker was "
                     "not forcibly terminated and no process cleanup was "
                     "attempted. Manual exact-PID recovery is required.\n");
        CloseHandle(hThread);
        return 98;
    }
    if (wait == WAIT_FAILED) {
        std::fprintf(stderr,
                     "\nFATAL: waiting for diagnostic worker failed: %lu; "
                     "no thread or process was forcibly terminated.\n",
                     GetLastError());
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn?
    if (reportCrashReporter("POST") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected after the diagnostic; no process cleanup was "
                     "attempted.\n");
        return 97;
    }

    return st.exitCode;
}
