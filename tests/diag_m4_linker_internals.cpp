// diag_m4_linker_internals.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: investigate why the linker FUN_101a3340 (ordinal 85) returns a
// non-NULL record (e.g. 0x39FC1EB0) but with EMPTY element arrays
// (+0x29=0, +0x4f=NULL, +0x26=NULL), even though the warehouse state 0x38
// succeeds (warehouse+0x4e=0, warehouse+0x26 is non-NULL with a SafeArray).
//
// Hypotheses to disambiguate:
//   H1. The record is CACHED at warehouse+0x23d from a previous partial link.
//   H2. The linker takes a different code path (BinaryCode path in
//       FUN_101a3a90) that bypasses the populators.
//   H3. The 0xBBC magic IS present in the compiled object code but the
//       populators (FUN_101a25a0, FUN_101a2430, FUN_101a22c0) return NULL.
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB-
//      stack worker thread.
//   2. Resolves Ordinal_10, Ordinal_69, Ordinal_70, Ordinal_28, and ordinal
//      85 (linker).
//   3. Inserts + verifies a Function "TS_AUTO_M4_LI_FUNC" with source
//      `TS_AUTO_M4_LI_FUNC = 1;`.
//   4. Records the Function's ProgId.
//   5. Reads warehouse+0x23d (cached linked record) BEFORE any linker call.
//   6. Calls the linker with errorFlag=1 (emit errors on failure) using
//      __stdcall convention; reads warehouse+0x23d again and the returned
//      record's fields.
//   7. Reads the raw compiled object code from the SafeArray at warehouse+0x26
//      (first 256 bytes hex dump, scans for 0xBBC and 0xBB9 magic).
//   8. Checks the .ell file contents (first 256 bytes hex dump, scans for
//      0xBBC and 0xBB9 magic).
//   9. Calls Ordinal_69 with param_3=0 (normal path) and reads all fields.
//  10. Calls Ordinal_69 with param_3=1 (short-circuit) and reads all fields.
//  11. Reads warehouse+0x23d AFTER all calls.
//  12. Cleans up: deletes the Function. Checks for TSCrashReport.exe.
//
// All orlib20 calls are SEH-protected. The warehouse pointer from
// Ordinal_10(0) is a LIVE pointer — it is NOT freed. The SafeArray at
// warehouse+0x26 is owned by the warehouse — we use SafeArrayAccessData /
// SafeArrayUnaccessData but NEVER SafeArrayDestroy.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>
#include <oleauto.h>        // SafeArray APIs

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
// Ordinal_10 (GetWarehouse, FUN at RVA 0x1A9650): __cdecl, 1 arg (path; 0
//   = default). Returns the warehouse singleton pointer (CELWHouse*).
typedef void* (__cdecl *GetWarehouseFn)(int pathParam);

// Ordinal_69 (FUN_101a3e00): __cdecl, 3 args (category:char, progId:int,
//   flag:int). Returns live record pointer (refcounted — release via
//   Ordinal_70).
typedef void* (__cdecl *Ordinal69Fn)(char category, int progId, int flag);

// Ordinal_70 (FUN_1015df30): __stdcall, 2 args (rec, flag). RET 0x8.
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);

// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args (category, progId, flag,
//   outBuf). RET 0x10. Writes a 0x10A-byte partial copy into outBuf.
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);

// Linker FUN_101a3340 (ordinal 85). The brief specifies __stdcall with 4
// stack args (type, id, errorFlag, partialBuf). We use the manual-ESP
// inline-asm wrapper from diag_m4_whserver.cpp so the call is robust to
// either RET 0x10 (stdcall) or RET 0x8 (fastcall) cleanup, AND we load
// ECX=type, EDX=id so the call is valid under BOTH conventions.
//
// __stdcall typedef (used only for the type cast in the manual wrapper).
typedef void* (__stdcall *LinkerStdcallFn)(int type, int id, int errorFlag,
                                           void* partialBuf);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct PtrSehResult { void* ptr; };

bool callGetWarehouse(GetWarehouseFn fn, int pathParam, PtrSehResult* out) {
    out->ptr = nullptr;
    __try { out->ptr = fn(pathParam); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool callOrd69(Ordinal69Fn fn, char category, int progId, int flag,
               PtrSehResult* out) {
    out->ptr = nullptr;
    __try { out->ptr = fn(category, progId, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct IntSehResult { int result; };

bool callOrd70(Ordinal70Fn fn, void* rec, int flag, IntSehResult* out) {
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
bool sehReadDword(const void* p, unsigned int* out) {
    *out = 0;
    __try { *out = *(const unsigned int*)p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool sehMemcpy(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); }
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
// Field snapshot of a live record returned by Ordinal_69 / linker.
// Captures the key offsets requested by the brief.
// -------------------------------------------------------------------------
struct RecFields {
    short          firstShort;     // +0x00
    unsigned char  flags15;        // +0x15
    void*          safeArrayPtr;   // +0x26
    int            elemCount;      // +0x29
    void*          elemArrayPtr4f; // +0x4f
    void*          elemArrayPtr53; // +0x53
    void*          elemArrayPtr57; // +0x57
    unsigned char  verified55;     // +0x55
    void*          recDepListPtr;  // +0x7b
    int            refcount;       // +0xCA
    bool           anySeh;
};

RecFields readFields(const void* recPtr) {
    RecFields f = {};
    f.anySeh = false;
    if (!recPtr) return f;
    const unsigned char* rec = (const unsigned char*)recPtr;
    if (!sehReadShort(rec + 0x00, &f.firstShort))      f.anySeh = true;
    if (!sehReadByte (rec + 0x15, &f.flags15))         f.anySeh = true;
    if (!sehReadPtr  (rec + 0x26, &f.safeArrayPtr))    f.anySeh = true;
    if (!sehReadInt  (rec + 0x29, &f.elemCount))       f.anySeh = true;
    if (!sehReadPtr  (rec + 0x4f, &f.elemArrayPtr4f))  f.anySeh = true;
    if (!sehReadPtr  (rec + 0x53, &f.elemArrayPtr53))  f.anySeh = true;
    if (!sehReadPtr  (rec + 0x57, &f.elemArrayPtr57))  f.anySeh = true;
    if (!sehReadByte (rec + 0x55, &f.verified55))      f.anySeh = true;
    if (!sehReadPtr  (rec + 0x7b, &f.recDepListPtr))   f.anySeh = true;
    if (!sehReadInt  (rec + 0xCA, &f.refcount))        f.anySeh = true;
    return f;
}

void printFields(const char* label, const RecFields& f, int expectedProgId) {
    std::printf("\n  --- FIELD READS: %s ---\n", label);
    std::printf("    +0x00 first field (short):     %d (0x%04X)  "
                "[expected ProgId=%d]\n",
                f.firstShort, (unsigned short)f.firstShort, expectedProgId);
    std::printf("    +0x15 flags (byte):            0x%02X\n", f.flags15);
    std::printf("    +0x26 SafeArray ptr:           %p  [%s]\n",
                f.safeArrayPtr, f.safeArrayPtr ? "NON-NULL" : "NULL");
    std::printf("    +0x29 element count (int):     %d (0x%X)  [%s]\n",
                f.elemCount, (unsigned)f.elemCount,
                f.elemCount == 0 ? "ZERO" : "NONZERO");
    std::printf("    +0x4f element array ptr:       %p  [%s]\n",
                f.elemArrayPtr4f, f.elemArrayPtr4f ? "NON-NULL" : "NULL");
    std::printf("    +0x53 element array ptr:       %p  [%s]\n",
                f.elemArrayPtr53, f.elemArrayPtr53 ? "NON-NULL" : "NULL");
    std::printf("    +0x57 element array ptr:       %p  [%s]\n",
                f.elemArrayPtr57, f.elemArrayPtr57 ? "NON-NULL" : "NULL");
    std::printf("    +0x55 verified flag (byte):    0x%02X (%u)\n",
                f.verified55, f.verified55);
    std::printf("    +0x7b record-level dep-list:   %p  [%s]\n",
                f.recDepListPtr, f.recDepListPtr ? "NON-NULL" : "NULL");
    std::printf("    +0xCA refcount (int):          %d (0x%X)\n",
                f.refcount, (unsigned)f.refcount);
    if (f.anySeh) std::printf("    [one or more field reads raised SEH]\n");
}

bool fieldsPopulated(const RecFields& f) {
    return f.elemCount > 0 && f.elemArrayPtr4f != nullptr;
}

void releaseRec(Ordinal70Fn ord70, const char* label, void* rec) {
    if (!rec) return;
    if (ord70) {
        IntSehResult r;
        bool ok = callOrd70(ord70, rec, 0, &r);
        std::printf("    [release %s] Ordinal_70(rec=%p, 0): %s, ret=%d\n",
                    label, rec, ok ? "OK" : "SEH", r.result);
    } else {
        std::printf("    [release %s] Ordinal_70 NOT resolved — LEAKING "
                    "rec=%p\n", label, rec);
    }
}

// -------------------------------------------------------------------------
// Hex dump helper.
// -------------------------------------------------------------------------
void hexDump(const char* label, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    std::printf("  --- HEX DUMP: %s (%zu bytes) ---\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        std::printf("  %04zx: ", i);
        for (size_t j = 0; j < 16 && (i + j) < n; j++) {
            std::printf("%02x ", p[i + j]);
        }
        // Pad to align ASCII column.
        for (size_t j = (n - i < 16 ? n - i : 16); j < 16; j++) {
            std::printf("   ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16 && (i + j) < n; j++) {
            unsigned char c = p[i + j];
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        std::printf("|\n");
    }
}

// Scan a buffer for the 4-byte little-endian magic 0xBBC and 0xBB9 (each is
// a 4-byte int with the high bytes zero). Report all offsets where found.
void scanMagic(const char* label, const void* data, size_t n,
               unsigned int magic, const char* magicName) {
    const unsigned char* p = (const unsigned char*)data;
    std::printf("  [scan %s for %s (0x%X)]: ", label, magicName, magic);
    int hits = 0;
    if (n >= 4) {
        for (size_t i = 0; i + 4 <= n; i++) {
            unsigned int v = (unsigned int)p[i]
                           | ((unsigned int)p[i + 1] << 8)
                           | ((unsigned int)p[i + 2] << 16)
                           | ((unsigned int)p[i + 3] << 24);
            if (v == magic) {
                if (hits > 0) std::printf(", ");
                std::printf("offset 0x%zx", i);
                hits++;
                if (hits >= 16) { std::printf(" ... (truncated)"); break; }
            }
        }
    }
    if (hits == 0) std::printf("NOT FOUND");
    std::printf("  [%d hit(s)]\n", hits);
}

// -------------------------------------------------------------------------
// .ell file inspection (first 256 bytes hex dump + magic scan + size).
// -------------------------------------------------------------------------
void inspectEllFile(const std::string& binDir, int whType, int progId) {
    char ellPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  binDir.c_str(), whType, progId);
    std::printf("  .ell file: %s\n", ellPath);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(ellPath, GetFileExInfoStandard, &fad)) {
        std::printf("    DOES NOT EXIST (GetFileAttributes failed: %lu)\n",
                    GetLastError());
        return;
    }
    __int64 size = ((__int64)fad.nFileSizeHigh << 32) |
                   (__int64)fad.nFileSizeLow;
    std::printf("    EXISTS, size = %lld bytes\n", (long long)size);
    SYSTEMTIME st;
    if (FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
        std::printf("    mtime = %04d-%02d-%02d %02d:%02d:%02d\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
    }

    HANDLE h = CreateFileA(ellPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("    CreateFileA failed: %lu\n", GetLastError());
        return;
    }
    unsigned char buf[256] = {};
    DWORD toRead = (size < (__int64)sizeof(buf)) ? (DWORD)size
                                                 : (DWORD)sizeof(buf);
    DWORD read = 0;
    if (!ReadFile(h, buf, toRead, &read, nullptr) || read == 0) {
        std::printf("    ReadFile failed: %lu (read=%lu)\n", GetLastError(),
                    read);
        CloseHandle(h);
        return;
    }
    CloseHandle(h);

    char label[64];
    std::snprintf(label, sizeof(label), ".ell first %lu bytes", read);
    hexDump(label, buf, read);
    scanMagic(".ell", buf, read, 0x00000BBCu, "0xBBC");
    scanMagic(".ell", buf, read, 0x00000BB9u, "0xBB9");
}

// -------------------------------------------------------------------------
// SafeArray inspection at warehouse+0x26 (first 256 bytes hex dump +
// magic scan). Does NOT destroy the SafeArray (owned by the warehouse).
// -------------------------------------------------------------------------
void inspectWarehouseSafeArray(const char* tag, void* whPtr) {
    if (!whPtr) {
        std::printf("  [%s SafeArray] warehouse ptr is NULL — skipping\n",
                    tag);
        return;
    }
    void* saPtr = nullptr;
    if (!sehReadPtr((const char*)whPtr + 0x26, &saPtr)) {
        std::printf("  [%s SafeArray] read warehouse+0x26 raised SEH\n", tag);
        return;
    }
    std::printf("  [%s SafeArray] warehouse+0x26 = %p  [%s]\n",
                tag, saPtr, saPtr ? "NON-NULL" : "NULL");
    if (!saPtr) return;

    SAFEARRAY* psa = (SAFEARRAY*)saPtr;
    // Read SafeArray descriptor fields (SEH-protected).
    VARTYPE vt = 0;
    unsigned short dims = 0;
    long lb = 0, ub = 0;
    bool ok = true;
    __try {
        dims = SafeArrayGetDim(psa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    SafeArrayGetDim raised SEH\n");
        ok = false;
    }
    if (!ok) return;
    std::printf("    SafeArrayGetDim = %u\n", (unsigned)dims);
    if (dims == 0) {
        std::printf("    [zero-dim SafeArray — no data]\n");
        return;
    }
    __try {
        if (SafeArrayGetLBound(psa, 1, &lb) != S_OK) lb = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { lb = 0; }
    __try {
        if (SafeArrayGetUBound(psa, 1, &ub) != S_OK) ub = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ub = -1; }
    long elemCount = (ub >= lb) ? (ub - lb + 1) : 0;
    std::printf("    LBound=%ld UBound=%ld elemCount=%ld\n", lb, ub, elemCount);

    // Try to read the element size via the descriptor (cbElements).
    unsigned short cbElements = 0;
    __try { cbElements = psa->cbElements; }
    __except (EXCEPTION_EXECUTE_HANDLER) { cbElements = 0; }
    std::printf("    cbElements=%u  cLocks=%ld  pvData=%p\n",
                (unsigned)cbElements, psa->cLocks, psa->pvData);

    if (elemCount <= 0 || cbElements == 0) {
        std::printf("    [no elements to dump]\n");
        return;
    }

    // Access the data. We dump the first 256 bytes of the data buffer.
    void* pData = nullptr;
    HRESULT hr = S_OK;
    __try { hr = SafeArrayAccessData(psa, &pData); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    SafeArrayAccessData raised SEH\n");
        pData = nullptr;
    }
    if (FAILED(hr) || !pData) {
        std::printf("    SafeArrayAccessData failed: hr=0x%08X\n",
                    (unsigned)hr);
        return;
    }
    size_t totalBytes = (size_t)elemCount * (size_t)cbElements;
    size_t dumpBytes = totalBytes < 256 ? totalBytes : 256;
    char label[64];
    std::snprintf(label, sizeof(label), "SafeArray data (first %zu of %zu bytes)",
                  dumpBytes, totalBytes);
    hexDump(label, pData, dumpBytes);
    // Scan the FULL data buffer (not just first 256) for magic, capped at 64KB.
    size_t scanBytes = totalBytes < 65536 ? totalBytes : 65536;
    scanMagic("SafeArray", pData, scanBytes, 0x00000BBCu, "0xBBC");
    scanMagic("SafeArray", pData, scanBytes, 0x00000BB9u, "0xBB9");

    __try { SafeArrayUnaccessData(psa); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    SafeArrayUnaccessData raised SEH\n");
    }
    std::printf("    [SafeArray NOT destroyed — owned by warehouse]\n");
}

// -------------------------------------------------------------------------
// Read warehouse+0x23d (cached linked record) and dump its fields if
// non-NULL. Returns the raw pointer value.
// -------------------------------------------------------------------------
void* readCachedLinkedRecord(const char* tag, void* whPtr,
                             int expectedProgId) {
    if (!whPtr) {
        std::printf("  [%s cached+0x23d] warehouse ptr is NULL\n", tag);
        return nullptr;
    }
    void* cached = nullptr;
    if (!sehReadPtr((const char*)whPtr + 0x23d, &cached)) {
        std::printf("  [%s cached+0x23d] read raised SEH\n", tag);
        return nullptr;
    }
    std::printf("  [%s cached+0x23d] = %p  [%s]\n",
                tag, cached, cached ? "NON-NULL (cached record exists)" :
                                     "NULL (no cached record)");
    if (cached) {
        RecFields f = readFields(cached);
        char label[96];
        std::snprintf(label, sizeof(label),
                      "%s — cached record at warehouse+0x23d", tag);
        printFields(label, f, expectedProgId);
    }
    return cached;
}

// Helper to snapshot warehouse +0x4e and +0x26.
void snapWh(const char* tag, void* whPtr) {
    if (!whPtr) return;
    int hr = 0;
    void* sa = nullptr;
    sehReadInt((const char*)whPtr + 0x4e, &hr);
    sehReadPtr((const char*)whPtr + 0x26, &sa);
    std::printf("    [snap %s] warehouse+0x4e=%d (0x%X)  "
                "warehouse+0x26=%p\n",
                tag, hr, (unsigned)hr, sa);
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName = "TS_AUTO_M4_LI_FUNC";
    const std::string binDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

    std::printf("=== diag_m4_linker_internals: linker non-NULL / empty-arrays "
                "diagnostic ===\n");
    std::printf("Purpose: investigate why the linker (ordinal 85) returns a "
                "non-NULL record\n");
    std::printf("  with empty element arrays (+0x29=0, +0x4f=NULL, "
                "+0x26=NULL), even though\n");
    std::printf("  warehouse state 0x38 succeeds (warehouse+0x4e=0, "
                "warehouse+0x26 non-NULL).\n");
    std::printf("Hypotheses: H1 cached at warehouse+0x23d; H2 BinaryCode "
                "path; H3 0xBBC magic\n");
    std::printf("  present but populators return NULL.\n");

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

    // --- Step 2: resolve exports.
    std::printf("\n[2] Resolving Ordinal_10, Ordinal_69, Ordinal_70, "
                "Ordinal_28, and ordinal 85 (linker)...\n");
    GetWarehouseFn getWarehouse = nullptr;
    Ordinal69Fn ord69 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    Ordinal28Fn ord28 = nullptr;
    void* ord85Raw = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        FARPROC ord10 = resolver.resolveExportByOrdinal(10);
        if (ord10) getWarehouse = reinterpret_cast<GetWarehouseFn>(ord10);
        ord69 = reinterpret_cast<Ordinal69Fn>(
            resolver.resolveExportByOrdinal(69));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) ord85Raw = (void*)ord85;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    Resolved: Ord10=%p Ord69=%p Ord70=%p Ord28=%p "
                "Linker(85)=%p\n",
                (void*)getWarehouse, (void*)ord69, (void*)ord70,
                (void*)ord28, ord85Raw);
    if (!getWarehouse) std::fprintf(stderr, "WARN: Ordinal_10 unresolved\n");
    if (!ord69) std::fprintf(stderr, "WARN: Ordinal_69 unresolved\n");
    if (!ord70) std::fprintf(stderr, "WARN: Ordinal_70 unresolved\n");
    if (!ord28) std::fprintf(stderr, "WARN: Ordinal_28 unresolved\n");
    if (!ord85Raw) std::fprintf(stderr, "WARN: ordinal 85 (linker) "
                                "unresolved — direct linker call skipped\n");

    // --- Step 3: insert + verify the Function. ---
    std::printf("\n[3] Insert + verify Function \"%s\"...\n",
                funcName.c_str());
    int funcProgId = insertAndVerify(*lib, ComponentType::Function,
                                     funcName,
                                     makeFunctionSource(funcName),
                                     "Function");
    if (funcProgId <= 0) { st->exitCode = 200; return 0; }

    // --- Step 4: record the Function's ProgId. ---
    std::printf("\n[4] Recorded Function ProgId = %d\n", funcProgId);

    // --- Step 5: read warehouse+0x23d BEFORE any linker call. ---
    std::printf("\n[5] Reading warehouse+0x23d (cached linked record) "
                "BEFORE any linker call...\n");
    void* whPtr = nullptr;
    if (getWarehouse) {
        PtrSehResult whSeh;
        bool ok = callGetWarehouse(getWarehouse, 0, &whSeh);
        whPtr = whSeh.ptr;
        std::printf("    Ordinal_10(0): %s, warehouse=%p\n",
                    ok ? "OK (no SEH)" : "SEH raised", whPtr);
    } else {
        std::printf("    [SKIPPED: Ordinal_10 not resolved]\n");
    }

    if (whPtr) {
        snapWh("post-verify / pre-linker", whPtr);
        readCachedLinkedRecord("BEFORE linker call", whPtr, funcProgId);
    }

    // --- Step 6: call the linker with errorFlag=1 (emit errors on failure). ---
    std::printf("\n[6] Calling linker (ordinal 85, __stdcall/manual) with "
                "errorFlag=1...\n");
    // First, get a partial record buffer via Ordinal_28 to pass as
    // partialBuf.
    unsigned char partialBuf[0x10A] = {};
    bool havePartial = false;
    if (ord28) {
        Ord28SehResult ord28Seh;
        bool ord28Ok = callOrd28(ord28, /*category=*/4, funcProgId,
                                 /*flag=*/0, partialBuf, &ord28Seh);
        std::printf("  Ordinal_28(4, %d, 0, buf): %s, ret=%u\n",
                    funcProgId, ord28Ok ? "OK" : "SEH", ord28Seh.result);
        if (ord28Ok && ord28Seh.result == 1) {
            havePartial = true;
            void* sa = nullptr;
            sehReadPtr(partialBuf + 0x26, &sa);
            int ec = 0;
            sehReadInt(partialBuf + 0x29, &ec);
            std::printf("    Partial copy +0x26 SafeArrayPtr=%p, "
                        "+0x29 elemCount=%d\n", sa, ec);
        }
    } else {
        std::printf("  [Ordinal_28 not resolved — partialBuf will be "
                    "NULL]\n");
    }

    RecFields fLinker = {};
    bool haveLinkerRec = false;
    void* linkerRec = nullptr;
    if (ord85Raw) {
        snapWh("before linker call", whPtr);
        LinkerSehResult r;
        bool ok = callLinkerSafe(ord85Raw, /*type=*/4, funcProgId,
                                 /*errorFlag=*/1,
                                 havePartial ? partialBuf : nullptr, &r);
        std::printf("    linker(4, %d, 1, partialBuf=%p): %s, rec=%p\n",
                    funcProgId, havePartial ? (void*)partialBuf : nullptr,
                    ok ? "OK (no SEH)" : "SEH raised", r.ptr);
        snapWh("after linker call", whPtr);
        linkerRec = r.ptr;
        if (ok && r.ptr) {
            fLinker = readFields(r.ptr);
            printFields("Linker return (errorFlag=1)", fLinker, funcProgId);
            haveLinkerRec = true;
        } else {
            std::printf("    [linker returned NULL — no record to "
                        "inspect]\n");
        }
        // Read warehouse+0x23d again — did it change?
        std::printf("\n  [6b] Reading warehouse+0x23d AFTER linker call...\n");
        readCachedLinkedRecord("AFTER linker call", whPtr, funcProgId);

        // Release the linker's record via Ordinal_70 (refcount decrement).
        // NOTE: we read fields BEFORE releasing because Ordinal_70 may free
        // the record if refcount drops to 0.
        if (haveLinkerRec) {
            releaseRec(ord70, "linker rec", linkerRec);
        }
    } else {
        std::printf("  [SKIPPED: ordinal 85 (linker) not resolved]\n");
    }

    // --- Step 7: read the raw compiled object code from the SafeArray at
    //     warehouse+0x26. ---
    std::printf("\n[7] Reading raw compiled object code from SafeArray at "
                "warehouse+0x26 (AFTER linker call)...\n");
    inspectWarehouseSafeArray("post-linker", whPtr);

    // --- Step 8: check the .ell file contents. ---
    std::printf("\n[8] Checking .ell file contents (Function whType=4, "
                "progId=%d)...\n", funcProgId);
    inspectEllFile(binDir, /*whType=*/4, funcProgId);

    // --- Step 9: try Ordinal_69 with param_3=0 (normal path). ---
    std::printf("\n[9] Calling Ordinal_69(4, %d, 0) — normal path...\n",
                funcProgId);
    RecFields fOrd69_0 = {};
    bool haveOrd69_0 = false;
    if (ord69) {
        snapWh("before Ord69(.,.,0)", whPtr);
        PtrSehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, 0, &r);
        std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
        snapWh("after Ord69(.,.,0)", whPtr);
        if (ok && r.ptr) {
            fOrd69_0 = readFields(r.ptr);
            printFields("Ordinal_69(4, progId, 0) — normal path",
                        fOrd69_0, funcProgId);
            haveOrd69_0 = true;
            releaseRec(ord70, "Ord69(.,.,0) rec", r.ptr);
        } else {
            std::printf("    [no record returned]\n");
        }
    } else {
        std::printf("    [SKIPPED: Ordinal_69 not resolved]\n");
    }

    // --- Step 10: try Ordinal_69 with param_3=1 (short-circuit). ---
    std::printf("\n[10] Calling Ordinal_69(4, %d, 1) — short-circuit "
                "(linker path)...\n", funcProgId);
    RecFields fOrd69_1 = {};
    bool haveOrd69_1 = false;
    if (ord69) {
        snapWh("before Ord69(.,.,1)", whPtr);
        PtrSehResult r;
        bool ok = callOrd69(ord69, (char)4, funcProgId, 1, &r);
        std::printf("    call: %s, rec=%p\n", ok ? "OK" : "SEH", r.ptr);
        snapWh("after Ord69(.,.,1)", whPtr);
        if (ok && r.ptr) {
            fOrd69_1 = readFields(r.ptr);
            printFields("Ordinal_69(4, progId, 1) — short-circuit",
                        fOrd69_1, funcProgId);
            haveOrd69_1 = true;
            releaseRec(ord70, "Ord69(.,.,1) rec", r.ptr);
        } else {
            std::printf("    [no record returned]\n");
        }
    } else {
        std::printf("    [SKIPPED: Ordinal_69 not resolved]\n");
    }

    // --- Step 11: read warehouse+0x23d AFTER all calls. ---
    std::printf("\n[11] Reading warehouse+0x23d AFTER all calls...\n");
    void* cachedFinal = readCachedLinkedRecord("AFTER all calls", whPtr,
                                               funcProgId);
    (void)cachedFinal;

    // --- Step 12: cleanup. ---
    std::printf("\n[12] Cleanup: delete Function...\n");
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId = %d\n", funcProgId);
    std::printf("Warehouse pointer = %p\n", whPtr);
    std::printf("\n1. warehouse+0x23d BEFORE any linker call: see [5] above "
                "(cached record?)\n");
    std::printf("2. Linker return value and record fields: see [6] above\n");
    std::printf("3. warehouse+0x23d AFTER the linker call: see [6b] above "
                "(did it change?)\n");
    std::printf("4. Raw hex dump of SafeArray data at warehouse+0x26: see "
                "[7] above\n");
    std::printf("5. Does the SafeArray data contain 0xBBC or 0xBB9 magic? "
                "see [7] scan lines\n");
    std::printf("6. Raw hex dump of the .ell file (first 256 bytes): see "
                "[8] above\n");
    std::printf("7. Does the .ell file contain 0xBBC or 0xBB9 magic? see "
                "[8] scan lines\n");
    std::printf("8. Results of Ordinal_69 with param_3=0 and param_3=1: see "
                "[9] and [10] above\n");
    std::printf("9. warehouse+0x23d value AFTER all calls: see [11] "
                "above\n");
    std::printf("10. Crash check: see [PRE]/[POST] TSCrashReport lines in "
                "stderr\n");
    std::printf("11. Cleanup status: see [12] above\n");

    std::printf("\n=== COMPARISON TABLE ===\n");
    std::printf("  Source                  elemCount  +0x4f       +0x26(SA)   "
                "  populated\n");
    if (haveLinkerRec) {
        std::printf("  Linker(errorFlag=1)     %-9d  %-11p %-11p  %s\n",
                    fLinker.elemCount, fLinker.elemArrayPtr4f,
                    fLinker.safeArrayPtr,
                    fieldsPopulated(fLinker) ? "YES" : "NO");
    } else {
        std::printf("  Linker(errorFlag=1)     (no record)\n");
    }
    if (haveOrd69_0) {
        std::printf("  Ord69(.,.,0) normal     %-9d  %-11p %-11p  %s\n",
                    fOrd69_0.elemCount, fOrd69_0.elemArrayPtr4f,
                    fOrd69_0.safeArrayPtr,
                    fieldsPopulated(fOrd69_0) ? "YES" : "NO");
    } else {
        std::printf("  Ord69(.,.,0) normal     (no record)\n");
    }
    if (haveOrd69_1) {
        std::printf("  Ord69(.,.,1) short-ckt  %-9d  %-11p %-11p  %s\n",
                    fOrd69_1.elemCount, fOrd69_1.elemArrayPtr4f,
                    fOrd69_1.safeArrayPtr,
                    fieldsPopulated(fOrd69_1) ? "YES" : "NO");
    } else {
        std::printf("  Ord69(.,.,1) short-ckt  (no record)\n");
    }

    st->exitCode = 0;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: inspect TSCrashReport.exe without terminating it.
    if (reportCrashReporter("PRE") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected before the diagnostic; no process was "
                     "terminated.\n");
        return 97;
    }

    // Run on a 64 MB-stack worker thread (EasyLanguage compiler / hidden
    // EDIT control need this, and TsDevLib is thread-affine).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 120-second hard timeout — report a hang without terminating the worker.
    // Insert+verify + linker + Ord69 x2 + SafeArray/ELL inspection may take
    // longer.
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

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    if (reportCrashReporter("POST") != ProcessInspectionResult::NotFound) {
        std::fprintf(stderr,
                     "FATAL: TSCrashReport.exe was detected or could not be "
                     "inspected after the diagnostic; no process cleanup was "
                     "attempted.\n");
        return 97;
    }

    return st.exitCode;
}
