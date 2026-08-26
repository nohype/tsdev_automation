// diag_m4_objcode.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: dump the decompressed object code buffer that the linker uses, and
// resolve the warehouse indirection question raised by Ghidra analysis.
//
// Two critical issues with prior diagnostics:
//
//   1. Warehouse indirection: state handlers access *(param_1+0xc)+0x26, NOT
//      param_1+0x26. param_1 is a per-module wrapper; param_1+0xc is a
//      pointer to the CELWHouse. If Ordinal_10(0) returns the wrapper, then
//      wrapper+0x26 is NOT the SafeArray mailbox — *(wrapper+0xc)+0x26 IS.
//
//   2. Compressed vs decompressed: the .ell file contains COMPRESSED object
//      code. The decompression step (FUN_101dd260) strips a 0x44-byte header
//      and decompresses. The 0xBBC magic would be in the DECOMPRESSED data,
//      not the raw .ell file. Prior .ell scans were looking for 0xBBC in the
//      compressed file — wrong.
//
// The linker's data flow:
//   state 0x38 -> SafeArray at *(per_module+0xc)+0x26
//              -> FUN_101c2a30 copies to buffer
//              -> FUN_101dd260 decompresses
//              -> object code buffer at per_module+0x1c1
//              -> linker memcpy bytes 0xd-0xde to live record
//              -> record+0x26 = objcode+0x26
//
// In TSDev, record+0x26 = 0x2000000. In our CLI, record+0x26 = NULL. We need
// to see what the decompressed object code contains at offset 0x26.
//
// Steps:
//   1. Construct a TsDevLib (test-only ctor, real COM, no hooks) on a 64MB-
//      stack worker thread.
//   2. Resolve Ordinal_10, Ordinal_69, Ordinal_70, Ordinal_28, and ordinal
//      85 (linker).
//   3. Insert + verify a Function "TS_AUTO_M4_OBJ_FUNC" with source
//      `TS_AUTO_M4_OBJ_FUNC = 1;`.
//   4. Record the Function's ProgId.
//   5. Resolve the warehouse indirection. Call Ordinal_10(0) to get ptr.
//      Read ptr+0x04, ptr+0x0c, *(ptr+0x0c)+0x04, *(ptr+0x0c)+0x26,
//      *(ptr+0x0c)+0x4e, ptr+0x1c0, ptr+0x1c1, ptr+0x26 (old way).
//      Determine: is Ordinal_10(0) returning the per-module wrapper (with
//      +0x1c1) or the CELWHouse (with +0x26 as mailbox)?
//   6. Call the linker (ordinal 85, __stdcall/manual) to trigger state 0x38
//      and populate the object code buffer. Get a partial record via
//      Ordinal_28(4, progId, 0, buf). Call linker(4, progId, 0, buf).
//   7. Read the object code buffer AFTER the linker call. Using the correct
//      indirection: read wrapper+0x1c0 (cached flag), wrapper+0x1c1 (buffer
//      ptr), wrapper+0x1c5 (size?). Dump first 512 bytes. Check offset 0x26
//      (4 bytes) — is it 0x2000000 or 0? Check offset 0x24 (version field).
//      Scan for 0xBBC and 0xBB9 magic in the entire buffer.
//   8. Read the SafeArray at the correct indirection. Read *(wrapper+0xc)+
//      0x26 (the SafeArray mailbox). If valid (SafeArrayGetDim == 1): get
//      LBound, UBound, cbElements; AccessData and dump first 256 bytes;
//      UnaccessData; DO NOT destroy.
//   9. Compare the decompressed object code with the .ell file. Find the
//      .ell file: elmodule_4_<progId>.ell. Read first 512 bytes. Compare
//      with the decompressed object code buffer. Are they similar? Is the
//      .ell file just a wrapper around the compressed object code?
//  10. Check if the linker returned a populated record. If non-NULL: read
//      record+0x26 (should match objcode+0x26), record+0x29 (elemCount),
//      record+0x4f (elemArrayPtr), record+0x55 (verified flag). Call
//      Ordinal_70(rec, 0) to release.
//  11. Clean up: delete the Function. Check for TSCrashReport.exe.
//
// All orlib20 calls are SEH-protected. The warehouse pointer from
// Ordinal_10(0) is a LIVE pointer — it is NOT freed. The object code buffer
// at wrapper+0x1c1 is owned by the warehouse — it is NOT freed. The
// SafeArray is owned by the warehouse — we use SafeArrayAccessData /
// SafeArrayUnaccessData but NEVER SafeArrayDestroy.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>
#include <oleauto.h>        // SafeArray APIs

using namespace tsdev;

namespace {

// -------------------------------------------------------------------------
// Function pointer typedefs.
//
// Ordinal_10 (GetWarehouse, FUN at RVA 0x1A9650): __cdecl, 1 arg (path; 0
//   = default). Returns the warehouse singleton pointer (CELWHouse*) OR a
//   per-module wrapper — this diagnostic disambiguates.
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
//   stack args (type, id, errorFlag, partialBuf). We use the manual-ESP
//   inline-asm wrapper so the call is robust to either RET 0x10 (stdcall)
//   or RET 0x8 (fastcall) cleanup, AND we load ECX=type, EDX=id so the call
//   is valid under BOTH conventions.
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
// .ell file inspection (first 512 bytes hex dump + magic scan + size).
// Returns the raw bytes in `outBuf` (up to 512) and the file size.
// -------------------------------------------------------------------------
struct EllInspect {
    bool          exists;
    __int64       size;
    unsigned char head[512];
    DWORD         headLen;
};

EllInspect inspectEllFile(const std::string& binDir, int whType, int progId) {
    EllInspect r = {};
    char ellPath[512];
    std::snprintf(ellPath, sizeof(ellPath), "%s\\elmodule_%d_%d.ell",
                  binDir.c_str(), whType, progId);
    std::printf("  .ell file: %s\n", ellPath);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(ellPath, GetFileExInfoStandard, &fad)) {
        std::printf("    DOES NOT EXIST (GetFileAttributes failed: %lu)\n",
                    GetLastError());
        return r;
    }
    r.exists = true;
    r.size = ((__int64)fad.nFileSizeHigh << 32) | (__int64)fad.nFileSizeLow;
    std::printf("    EXISTS, size = %lld bytes\n", (long long)r.size);
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
        return r;
    }
    DWORD toRead = (r.size < (__int64)sizeof(r.head)) ? (DWORD)r.size
                                                      : (DWORD)sizeof(r.head);
    DWORD read = 0;
    if (!ReadFile(h, r.head, toRead, &read, nullptr) || read == 0) {
        std::printf("    ReadFile failed: %lu (read=%lu)\n", GetLastError(),
                    read);
        CloseHandle(h);
        return r;
    }
    r.headLen = read;
    CloseHandle(h);

    char label[64];
    std::snprintf(label, sizeof(label), ".ell first %lu bytes", read);
    hexDump(label, r.head, read);
    scanMagic(".ell head", r.head, read, 0x00000BBCu, "0xBBC");
    scanMagic(".ell head", r.head, read, 0x00000BB9u, "0xBB9");
    return r;
}

// -------------------------------------------------------------------------
// SafeArray inspection at the given SafeArray pointer (NOT destroyed).
// -------------------------------------------------------------------------
void inspectSafeArray(const char* tag, void* saPtr) {
    if (!saPtr) {
        std::printf("  [%s SafeArray] saPtr is NULL — skipping\n", tag);
        return;
    }
    SAFEARRAY* psa = (SAFEARRAY*)saPtr;
    unsigned short dims = 0;
    __try { dims = SafeArrayGetDim(psa); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("  [%s SafeArray] SafeArrayGetDim raised SEH — not a valid SafeArray?\n", tag);
        return;
    }
    std::printf("  [%s SafeArray] saPtr=%p  SafeArrayGetDim=%u\n",
                tag, saPtr, (unsigned)dims);
    if (dims != 1) {
        std::printf("    [dims != 1 — not the expected 1-D mailbox; skipping data dump]\n");
        return;
    }
    long lb = 0, ub = -1;
    __try { if (SafeArrayGetLBound(psa, 1, &lb) != S_OK) lb = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { lb = 0; }
    __try { if (SafeArrayGetUBound(psa, 1, &ub) != S_OK) ub = -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ub = -1; }
    long elemCount = (ub >= lb) ? (ub - lb + 1) : 0;
    unsigned short cbElements = 0;
    __try { cbElements = psa->cbElements; }
    __except (EXCEPTION_EXECUTE_HANDLER) { cbElements = 0; }
    std::printf("    LBound=%ld UBound=%ld elemCount=%ld cbElements=%u  "
                "cLocks=%ld pvData=%p\n",
                lb, ub, elemCount, (unsigned)cbElements, psa->cLocks,
                psa->pvData);
    if (elemCount <= 0 || cbElements == 0) {
        std::printf("    [no elements to dump]\n");
        return;
    }
    void* pData = nullptr;
    HRESULT hr = S_OK;
    __try { hr = SafeArrayAccessData(psa, &pData); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("    SafeArrayAccessData raised SEH\n");
        pData = nullptr;
        hr = E_FAIL;
    }
    if (FAILED(hr) || !pData) {
        std::printf("    SafeArrayAccessData failed: hr=0x%08X\n",
                    (unsigned)hr);
        return;
    }
    size_t totalBytes = (size_t)elemCount * (size_t)cbElements;
    size_t dumpBytes = totalBytes < 256 ? totalBytes : 256;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "%s SafeArray data (first %zu of %zu bytes)",
                  tag, dumpBytes, totalBytes);
    hexDump(label, pData, dumpBytes);
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
// Field snapshot of a live record returned by the linker.
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
// Warehouse indirection probe. Reads ptr+0x04, ptr+0x0c, *(ptr+0x0c)+0x04,
// *(ptr+0x0c)+0x26, *(ptr+0x0c)+0x4e, ptr+0x1c0, ptr+0x1c1, ptr+0x26.
// Prints all values. Returns the wrapper-vs-CELWHouse determination.
// -------------------------------------------------------------------------
struct WhIndir {
    void* ptr;            // Ordinal_10(0) result
    int   ptrState04;     // ptr+0x04
    void* ptr0c;          // ptr+0x0c (potential CELWHouse pointer)
    int   innerState04;   // *(ptr+0x0c)+0x04
    void* innerSa26;      // *(ptr+0x0c)+0x26 (potential SafeArray mailbox)
    int   innerHr4e;      // *(ptr+0x0c)+0x4e (potential HRESULT)
    unsigned char wrapper1c0; // ptr+0x1c0 (potential cached flag)
    void* wrapper1c1;     // ptr+0x1c1 (potential object code buffer ptr)
    void* ptr26_old;      // ptr+0x26 (old way — direct mailbox?)
    bool  looksLikeWrapper; // heuristic: ptr+0x1c1 non-NULL OR ptr+0x0c non-NULL
};

WhIndir probeWarehouseIndirection(void* ptr) {
    WhIndir w = {};
    w.ptr = ptr;
    if (!ptr) return w;
    sehReadInt((const char*)ptr + 0x04, &w.ptrState04);
    sehReadPtr((const char*)ptr + 0x0c, &w.ptr0c);
    if (w.ptr0c) {
        sehReadInt((const char*)w.ptr0c + 0x04, &w.innerState04);
        sehReadPtr((const char*)w.ptr0c + 0x26, &w.innerSa26);
        sehReadInt((const char*)w.ptr0c + 0x4e, &w.innerHr4e);
    }
    sehReadByte((const char*)ptr + 0x1c0, &w.wrapper1c0);
    sehReadPtr((const char*)ptr + 0x1c1, &w.wrapper1c1);
    sehReadPtr((const char*)ptr + 0x26, &w.ptr26_old);
    // Heuristic: if ptr+0x0c is non-NULL, ptr looks like the wrapper (it
    // holds a pointer to the CELWHouse at +0xc). If ptr+0x1c1 is non-NULL,
    // ptr is definitely the wrapper (object code buffer is a wrapper field).
    w.looksLikeWrapper = (w.ptr0c != nullptr) || (w.wrapper1c1 != nullptr);
    return w;
}

void printWhIndir(const WhIndir& w) {
    std::printf("    ptr (Ordinal_10(0))         = %p\n", w.ptr);
    std::printf("    ptr+0x04 (state?)           = 0x%08X (%d)\n",
                (unsigned)w.ptrState04, w.ptrState04);
    std::printf("    ptr+0x0c (CELWHouse ptr?)   = %p  [%s]\n",
                w.ptr0c, w.ptr0c ? "NON-NULL" : "NULL");
    if (w.ptr0c) {
        std::printf("      *(ptr+0x0c)+0x04 (state?)  = 0x%08X (%d)\n",
                    (unsigned)w.innerState04, w.innerState04);
        std::printf("      *(ptr+0x0c)+0x26 (SA mbx?) = %p  [%s]\n",
                    w.innerSa26, w.innerSa26 ? "NON-NULL" : "NULL");
        std::printf("      *(ptr+0x0c)+0x4e (HRESULT?)= 0x%08X (%d)\n",
                    (unsigned)w.innerHr4e, w.innerHr4e);
    }
    std::printf("    ptr+0x1c0 (cached flag?)    = 0x%02X (%u)\n",
                (unsigned)w.wrapper1c0, (unsigned)w.wrapper1c0);
    std::printf("    ptr+0x1c1 (objcode buf ptr?)= %p  [%s]\n",
                w.wrapper1c1, w.wrapper1c1 ? "NON-NULL" : "NULL");
    std::printf("    ptr+0x26 (old way, direct)  = %p  [%s]\n",
                w.ptr26_old, w.ptr26_old ? "NON-NULL" : "NULL");
    std::printf("    --> DETERMINATION: ptr looks like %s\n",
                w.looksLikeWrapper ? "the PER-MODULE WRAPPER"
                                   : "the CELWHouse (or unknown)");
}

// -------------------------------------------------------------------------
// Object code buffer inspection. Reads first 512 bytes, checks offset 0x26
// (4 bytes), offset 0x24 (version), scans for 0xBBC and 0xBB9 magic.
// Tries several size offsets near wrapper+0x1c5.
// -------------------------------------------------------------------------
void inspectObjCodeBuffer(const char* tag, void* bufPtr, void* wrapperPtr,
                          size_t assumeSize) {
    if (!bufPtr) {
        std::printf("  [%s objcode] buffer ptr is NULL — nothing to dump\n",
                    tag);
        return;
    }
    std::printf("  [%s objcode] buffer ptr = %p  (assumeSize = %zu)\n",
                tag, bufPtr, assumeSize);

    // Try several size offsets near wrapper+0x1c5 (FUN_101c2b90 returns the
    // size; the exact offset is uncertain). Read as int/uint at +0x1c5,
    // +0x1c4, +0x1c8, +0x1cc, +0x1d0, +0x1d4, +0x1d8.
    if (wrapperPtr) {
        const char* wp = (const char*)wrapperPtr;
        unsigned int sz = 0;
        for (int off : {0x1c4, 0x1c5, 0x1c8, 0x1cc, 0x1d0, 0x1d4, 0x1d8,
                        0x1dc, 0x1e0, 0x1e4, 0x1e8, 0x1ec, 0x1f0}) {
            if (sehReadDword(wp + off, &sz)) {
                std::printf("    wrapper+0x%X = 0x%08X (%u)\n",
                            off, sz, sz);
            } else {
                std::printf("    wrapper+0x%X = SEH\n", off);
            }
        }
    }

    // Dump first 512 bytes (SEH-protected memcpy into a local buffer).
    unsigned char local[512] = {};
    size_t dumpN = assumeSize < sizeof(local) ? assumeSize : sizeof(local);
    if (dumpN == 0) dumpN = sizeof(local);  // size unknown — try 512
    if (!sehMemcpy(bufPtr, local, dumpN)) {
        std::printf("    [memcpy of first %zu bytes raised SEH — trying "
                    "smaller]\n", dumpN);
        dumpN = 64;
        if (!sehMemcpy(bufPtr, local, dumpN)) {
            std::printf("    [memcpy of 64 bytes ALSO raised SEH — "
                        "aborting dump]\n");
            return;
        }
    }
    char label[64];
    std::snprintf(label, sizeof(label), "objcode first %zu bytes", dumpN);
    hexDump(label, local, dumpN);

    // Check offset 0x26 (4 bytes) — is it 0x2000000 or 0?
    int v26 = 0;
    if (dumpN >= 0x2a) sehMemcpy(local + 0x26, &v26, 4);
    std::printf("    objcode+0x26 (4 bytes) = 0x%08X (%d)  [%s]\n",
                (unsigned)v26, v26,
                (unsigned)v26 == 0x02000000u ? "MATCHES 0x2000000 (TSDev)"
                : v26 == 0 ? "ZERO (matches our CLI NULL)"
                : "OTHER VALUE");

    // Check offset 0x24 (version field the linker reads).
    int v24 = 0;
    if (dumpN >= 0x28) sehMemcpy(local + 0x24, &v24, 4);
    std::printf("    objcode+0x24 (4 bytes) = 0x%08X (%d)\n",
                (unsigned)v24, v24);

    // Scan for 0xBBC and 0xBB9 magic in the dumped region.
    scanMagic("objcode head", local, dumpN, 0x00000BBCu, "0xBBC");
    scanMagic("objcode head", local, dumpN, 0x00000BB9u, "0xBB9");

    // Also try a wider scan (up to 64KB) directly from the live buffer,
    // SEH-protected. We do this in 4KB chunks to limit SEH blast radius.
    if (assumeSize > 0 && assumeSize <= 0x100000) {
        size_t scanTotal = assumeSize < 0x10000 ? assumeSize : 0x10000;
        unsigned char chunk[4096];
        int hitsBbc = 0, hitsBb9 = 0;
        size_t firstBbc = (size_t)-1, firstBb9 = (size_t)-1;
        for (size_t off = 0; off < scanTotal; off += sizeof(chunk)) {
            size_t want = sizeof(chunk);
            if (off + want > scanTotal) want = scanTotal - off;
            if (!sehMemcpy((const char*)bufPtr + off, chunk, want)) break;
            for (size_t i = 0; i + 4 <= want; i++) {
                unsigned int v = (unsigned int)chunk[i]
                               | ((unsigned int)chunk[i + 1] << 8)
                               | ((unsigned int)chunk[i + 2] << 16)
                               | ((unsigned int)chunk[i + 3] << 24);
                if (v == 0x00000BBCu) {
                    if (firstBbc == (size_t)-1) firstBbc = off + i;
                    hitsBbc++;
                } else if (v == 0x00000BB9u) {
                    if (firstBb9 == (size_t)-1) firstBb9 = off + i;
                    hitsBb9++;
                }
            }
        }
        std::printf("    [wider scan, %zu bytes] 0xBBC: %d hit(s) "
                    "(first at 0x%zx)  0xBB9: %d hit(s) (first at 0x%zx)\n",
                    scanTotal, hitsBbc, firstBbc, hitsBb9, firstBb9);
    } else {
        std::printf("    [wider scan skipped — assumeSize unknown or > 1MB]\n");
    }
}

// -------------------------------------------------------------------------
// Compare two buffers byte-by-byte over the smaller of the two lengths.
// Reports the number of matching bytes and the first mismatch offset.
// -------------------------------------------------------------------------
void compareBuffers(const char* labelA, const unsigned char* a, size_t aLen,
                    const char* labelB, const unsigned char* b, size_t bLen) {
    size_t n = aLen < bLen ? aLen : bLen;
    size_t match = 0, firstDiff = (size_t)-1;
    for (size_t i = 0; i < n; i++) {
        if (a[i] == b[i]) match++;
        else if (firstDiff == (size_t)-1) firstDiff = i;
    }
    char diffBuf[32];
    if (firstDiff == (size_t)-1) {
        std::snprintf(diffBuf, sizeof(diffBuf), "(none)");
    } else {
        std::snprintf(diffBuf, sizeof(diffBuf), "0x%zx", firstDiff);
    }
    std::printf("  [compare %s (%zu B) vs %s (%zu B)]: common prefix = %zu B, "
                "matching = %zu B, first diff at offset %s\n",
                labelA, aLen, labelB, bLen, n, match, diffBuf);
    if (firstDiff != (size_t)-1) {
        std::printf("    first diff at 0x%zx: %s=0x%02X vs %s=0x%02X\n",
                    firstDiff, labelA, a[firstDiff], labelB, b[firstDiff]);
    }
    if (n > 0 && match == n) {
        std::printf("    --> Buffers are IDENTICAL over the common %zu-byte "
                    "prefix.\n", n);
    }
}

// -------------------------------------------------------------------------
// Diagnostic state.
// -------------------------------------------------------------------------
struct DiagState {
    int exitCode = 0;
};

DWORD WINAPI runDiag(LPVOID param) {
    DiagState* st = static_cast<DiagState*>(param);

    const std::string funcName = "TS_AUTO_M4_OBJ_FUNC";
    const std::string binDir =
        "C:\\Program Files (x86)\\TradeStation 9.5\\MyWork\\Bin";

    std::printf("=== diag_m4_objcode: decompressed object code buffer dump + "
                "warehouse indirection resolver ===\n");
    std::printf("Purpose: dump the DECOMPRESSED object code buffer the linker "
                "uses (at wrapper+0x1c1),\n");
    std::printf("  resolve whether Ordinal_10(0) returns the per-module "
                "wrapper or the CELWHouse,\n");
    std::printf("  and check offset 0x26 of the decompressed buffer "
                "(0x2000000 in TSDev, NULL in our CLI).\n");

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

    // --- Step 5: resolve the warehouse indirection ---
    std::printf("\n[5] Resolving warehouse indirection via Ordinal_10(0)...\n");
    WhIndir whPre = {};
    if (getWarehouse) {
        PtrSehResult whSeh;
        bool ok = callGetWarehouse(getWarehouse, 0, &whSeh);
        whPre.ptr = whSeh.ptr;
        std::printf("    Ordinal_10(0): %s, ptr=%p\n",
                    ok ? "OK (no SEH)" : "SEH raised", whPre.ptr);
        if (whPre.ptr) {
            whPre = probeWarehouseIndirection(whPre.ptr);
            printWhIndir(whPre);
        }
    } else {
        std::printf("    [SKIPPED: Ordinal_10 not resolved]\n");
    }

    // --- Step 6: call the linker to trigger state 0x38 and populate the
    //     object code buffer. ---
    std::printf("\n[6] Calling linker (ordinal 85, __stdcall/manual) with "
                "errorFlag=0 to populate the object code buffer...\n");
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
        LinkerSehResult r;
        bool ok = callLinkerSafe(ord85Raw, /*type=*/4, funcProgId,
                                 /*errorFlag=*/0,
                                 havePartial ? partialBuf : nullptr, &r);
        std::printf("    linker(4, %d, 0, partialBuf=%p): %s, rec=%p\n",
                    funcProgId, havePartial ? (void*)partialBuf : nullptr,
                    ok ? "OK (no SEH)" : "SEH raised", r.ptr);
        linkerRec = r.ptr;
        if (ok && r.ptr) {
            fLinker = readFields(r.ptr);
            printFields("Linker return (errorFlag=0)", fLinker, funcProgId);
            haveLinkerRec = true;
        } else {
            std::printf("    [linker returned NULL — no record to "
                        "inspect]\n");
        }
    } else {
        std::printf("  [SKIPPED: ordinal 85 (linker) not resolved]\n");
    }

    // --- Step 7: read the object code buffer AFTER the linker call. ---
    std::printf("\n[7] Reading the object code buffer AFTER the linker "
                "call...\n");
    // Re-probe the warehouse — the wrapper fields may have been populated
    // by the linker call.
    WhIndir whPost = {};
    if (getWarehouse) {
        PtrSehResult whSeh;
        bool ok = callGetWarehouse(getWarehouse, 0, &whSeh);
        whPost.ptr = whSeh.ptr;
        std::printf("    Ordinal_10(0) (post-linker): %s, ptr=%p\n",
                    ok ? "OK" : "SEH", whPost.ptr);
        if (whPost.ptr) {
            whPost = probeWarehouseIndirection(whPost.ptr);
            printWhIndir(whPost);
        }
    }

    // Try to find the object code buffer. Two candidates:
    //   (a) wrapper+0x1c1 (if ptr is the wrapper)
    //   (b) if ptr is the CELWHouse, we have no direct path to the wrapper
    //       — but we can still inspect the SafeArray mailbox.
    void* objCodeBuf = nullptr;
    void* wrapperPtr = nullptr;
    if (whPost.ptr) {
        if (whPost.looksLikeWrapper) {
            wrapperPtr = whPost.ptr;
            objCodeBuf = whPost.wrapper1c1;
            std::printf("    -> Using wrapper+0x1c1 as the object code "
                        "buffer ptr.\n");
        } else {
            std::printf("    -> ptr does NOT look like the wrapper. "
                        "wrapper+0x1c1 path unavailable.\n");
            // Fallback: maybe ptr+0x1c1 is still meaningful even if ptr is
            // the CELWHouse — read it for completeness.
            wrapperPtr = whPost.ptr;
            objCodeBuf = whPost.wrapper1c1;
        }
    }

    // For the size, try wrapper+0x1c5 first (the brief suggests
    // FUN_101c2b90 returns the size and the offset is near +0x1c5). We pass
    // 0 to inspectObjCodeBuffer and let it scan candidate offsets; we also
    // try reading wrapper+0x1c5 as a uint here.
    size_t assumeSize = 0;
    if (wrapperPtr) {
        unsigned int sz = 0;
        if (sehReadDword((const char*)wrapperPtr + 0x1c5, &sz) && sz > 0 &&
            sz < 0x100000) {
            assumeSize = sz;
            std::printf("    wrapper+0x1c5 (size candidate) = %u — using "
                        "as assumeSize\n", sz);
        } else if (sehReadDword((const char*)wrapperPtr + 0x1c8, &sz) &&
                   sz > 0 && sz < 0x100000) {
            assumeSize = sz;
            std::printf("    wrapper+0x1c8 (size candidate) = %u — using "
                        "as assumeSize\n", sz);
        } else {
            std::printf("    [size could not be determined from wrapper "
                        "fields — will dump 512 bytes blindly]\n");
        }
    }
    inspectObjCodeBuffer("post-linker", objCodeBuf, wrapperPtr, assumeSize);

    // --- Step 8: read the SafeArray at the correct indirection. ---
    std::printf("\n[8] Reading the SafeArray at the correct indirection...\n");
    if (whPost.ptr) {
        if (whPost.looksLikeWrapper && whPost.ptr0c) {
            std::printf("    ptr is the wrapper — SafeArray mailbox is at "
                        "*(ptr+0x0c)+0x26 = %p\n", whPost.innerSa26);
            inspectSafeArray("*(wrapper+0xc)+0x26", whPost.innerSa26);
        } else {
            std::printf("    ptr does NOT look like the wrapper — falling "
                        "back to ptr+0x26 (old way)\n");
            inspectSafeArray("ptr+0x26 (old way)", whPost.ptr26_old);
        }
        // Also dump the old-way ptr+0x26 for comparison regardless.
        std::printf("    [also inspecting ptr+0x26 (old way) for "
                    "comparison]\n");
        inspectSafeArray("ptr+0x26 (old way, comparison)", whPost.ptr26_old);
    } else {
        std::printf("    [SKIPPED: warehouse ptr is NULL]\n");
    }

    // --- Step 9: compare the decompressed object code with the .ell file. ---
    std::printf("\n[9] Comparing decompressed object code with the .ell "
                "file...\n");
    EllInspect ell = inspectEllFile(binDir, /*whType=*/4, funcProgId);

    if (objCodeBuf && ell.headLen > 0) {
        // Read the first ell.headLen bytes of the object code buffer.
        unsigned char objHead[512] = {};
        size_t n = ell.headLen < sizeof(objHead) ? ell.headLen
                                                 : sizeof(objHead);
        if (sehMemcpy(objCodeBuf, objHead, n)) {
            compareBuffers("objcode head", objHead, n,
                           ".ell head", ell.head, ell.headLen);
        } else {
            std::printf("    [memcpy of objcode head raised SEH — cannot "
                        "compare]\n");
        }
    } else if (!objCodeBuf) {
        std::printf("    [objcode buffer is NULL — cannot compare]\n");
    } else if (ell.headLen == 0) {
        std::printf("    [.ell file unreadable — cannot compare]\n");
    }

    // Also: is the .ell file just a wrapper around the compressed object
    // code? Check if the first 0x44 bytes look like a header (the
    // decompression step strips a 0x44-byte header).
    if (ell.headLen >= 0x44) {
        std::printf("    [.ell first 0x44 bytes (potential compression "
                    "header)]:\n");
        hexDump(".ell header (first 0x44 bytes)", ell.head, 0x44);
        // Check if bytes after 0x44 differ from bytes before (i.e., the
        // header is distinct from the payload).
        bool headerDistinct = false;
        for (size_t i = 0; i < 0x44 && i + 0x44 < ell.headLen; i++) {
            if (ell.head[i] != ell.head[i + 0x44]) { headerDistinct = true;
                                                     break; }
        }
        std::printf("    [header (0..0x44) %s from payload (0x44..0x88)]\n",
                    headerDistinct ? "DIFFERS" : "is IDENTICAL");
    }

    // --- Step 10: check if the linker returned a populated record. ---
    std::printf("\n[10] Checking if the linker returned a populated "
                "record...\n");
    if (haveLinkerRec) {
        std::printf("    linker returned rec=%p\n", linkerRec);
        std::printf("    rec+0x26 (SafeArray ptr)        = %p  [%s]\n",
                    fLinker.safeArrayPtr,
                    fLinker.safeArrayPtr ? "NON-NULL" : "NULL");
        std::printf("    rec+0x29 (elemCount)            = %d (0x%X)\n",
                    fLinker.elemCount, (unsigned)fLinker.elemCount);
        std::printf("    rec+0x4f (elemArrayPtr)         = %p  [%s]\n",
                    fLinker.elemArrayPtr4f,
                    fLinker.elemArrayPtr4f ? "NON-NULL" : "NULL");
        std::printf("    rec+0x55 (verified flag)        = 0x%02X (%u)\n",
                    fLinker.verified55, fLinker.verified55);
        // Compare rec+0x26 with objcode+0x26 (if we have objcode).
        if (objCodeBuf) {
            int v26 = 0;
            sehMemcpy((const char*)objCodeBuf + 0x26, &v26, 4);
            std::printf("    [compare] objcode+0x26 = 0x%08X, rec+0x26 ptr "
                        "= %p  [%s]\n",
                        (unsigned)v26, fLinker.safeArrayPtr,
                        ((unsigned)(intptr_t)fLinker.safeArrayPtr ==
                         (unsigned)v26)
                        ? "MATCH" : "MISMATCH");
        }
        // Release via Ordinal_70.
        releaseRec(ord70, "linker rec", linkerRec);
    } else {
        std::printf("    [linker returned NULL — no record to inspect]\n");
    }

    // --- Step 11: cleanup. ---
    std::printf("\n[11] Cleanup: delete Function...\n");
    lib->deleteEntry(ComponentType::Function, funcName, /*flush=*/true);
    std::printf("    Cleanup done.\n");

    // --- Final summary. ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    std::printf("Function ProgId = %d\n", funcProgId);
    std::printf("Warehouse ptr (Ordinal_10(0)) = %p\n", whPost.ptr);
    std::printf("\n1. Warehouse indirection:\n");
    std::printf("   Ordinal_10(0) returns: %s\n",
                whPost.looksLikeWrapper ? "the PER-MODULE WRAPPER"
                                        : "the CELWHouse (or unknown)");
    std::printf("   ptr+0x0c (CELWHouse ptr?)   = %p\n", whPost.ptr0c);
    std::printf("   *(ptr+0x0c)+0x26 (SA mbx?)  = %p\n", whPost.innerSa26);
    std::printf("   ptr+0x26 (old way, direct)  = %p\n", whPost.ptr26_old);
    std::printf("\n2. Object code buffer (wrapper+0x1c1):\n");
    std::printf("   wrapper+0x1c0 (cached flag) = 0x%02X\n",
                (unsigned)whPost.wrapper1c0);
    std::printf("   wrapper+0x1c1 (buffer ptr)  = %p  [%s]\n",
                whPost.wrapper1c1, whPost.wrapper1c1 ? "NON-NULL" : "NULL");
    std::printf("\n3. Hex dump of decompressed object code (first 512 B): "
                "see [7] above\n");
    std::printf("\n4. objcode+0x26 (4 bytes): see [7] 'objcode+0x26' line\n");
    std::printf("\n5. 0xBBC / 0xBB9 magic in decompressed objcode: see [7] "
                "scan lines\n");
    std::printf("\n6. SafeArray at correct indirection: see [8] above\n");
    std::printf("\n7. Comparison of decompressed objcode vs .ell file: see "
                "[9] above\n");
    std::printf("\n8. Linker return value and record fields: see [10] "
                "above\n");
    std::printf("\n9. Crash check: see [PRE]/[POST] TSCrashReport lines in "
                "stderr\n");
    std::printf("\n10. Cleanup status: see [11] above\n");

    st->exitCode = 0;
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Pre-check: is TSCrashReport.exe already running?
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [PRE] TSCrashReport.exe IS "
           "running || echo [PRE] TSCrashReport.exe NOT running");

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
    // 120-second hard timeout — insert+verify + linker + objcode dump +
    // SafeArray/ELL inspection.
    DWORD wait = WaitForSingleObject(hThread, 120000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr, "\nFATAL: diagnostic timed out after 120s, "
                     "terminating.\n");
        TerminateThread(hThread, 99);
        // Best-effort cleanup of any TSCrashReport spawned by a crash.
        system("taskkill /F /IM TSCrashReport.exe >NUL 2>&1");
        CloseHandle(hThread);
        return 98;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    // Post-check: did TSCrashReport.exe spawn (indicating a crash)?
    system("tasklist /FI \"IMAGENAME eq TSCrashReport.exe\" 2>NUL | "
           "findstr /I TSCrashReport && echo [POST] TSCrashReport.exe IS "
           "running (crash occurred) || echo [POST] TSCrashReport.exe NOT "
           "running (no crash)");

    return st.exitCode;
}
