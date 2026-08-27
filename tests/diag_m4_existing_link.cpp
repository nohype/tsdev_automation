// diag_m4_existing_link.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: Historical diagnostic for the linker/dep-list layout. Its original
// hypothesis was that the linker wrote dep-lists at record+0x6f/+0x73 while
// GetDependencies read element+0x7b. Later M.4 evidence corrected that
// interpretation: production scans linked per-element dep-lists at
// element+0x7b/+0x73. This diagnostic is retained to compare both locations;
// it is not current M.4 acceptance logic.
//
// This diagnostic:
//   1. Constructs a TsDevLib (test-only ctor, real ComApi, no hooks) on a 64MB
//      stack worker thread.
//   2. Resolves Ordinal_28, Ordinal_70, _LoadMasterRecsEX@16, and ordinal 85
//      (linker).
//   3. Enumerates EXISTING items (read-only, no modification):
//        - First 5 existing Strategies (category 0) via
//          _LoadMasterRecsEX@16(0, 1, &buf, NULL).
//        - First 5 existing Functions (category 4) via
//          _LoadMasterRecsEX@16(4, 1, &buf, NULL).
//        - First 5 existing Indicators (category 2) via
//          _LoadMasterRecsEX@16(2, 1, &buf, NULL).
//      For each item, performs Steps 1-3 below.
//   4. For each item:
//        Step 1: Get partial record via Ordinal_28(category, progId, 0, buf).
//                Read +0x79 (gate flag — bit 2 = 0x04), +0x29 (elemCount),
//                +0x4f (elemArrayPtr).
//        Step 2: Call the linker directly (ordinal 85) via callLinkerSafe.
//                If rec is non-NULL, read rec+0x29 (elemCount),
//                rec+0x4f (elemArrayPtr), rec+0x55 (verified flag),
//                rec+0x6f (dep-list ptr), rec+0x73 (dep-list size).
//                If depListPtr is non-NULL and depListSize > 0, dump first
//                64 bytes of the dep-list buffer (SEH-protected).
//                Release via Ordinal_70(rec, 0).
//        Step 3: For each element (up to 5), read element+0x73 (size) and
//                element+0x7b (ptr) — SEH-protected.
//   5. Check for TSCrashReport.exe before and after.
//
// All orlib20 calls are SEH-protected (__try/__except). READ-ONLY — does NOT
// insert/modify/delete any items. Its 180-second limit is an operational
// timeout, not a measured completion time or M.4 acceptance evidence.

#include "../src/orlib_resolver.h"          // OrlibResolver (public)
#include "../src/tsdev_lib_test_hooks.h"    // internal/test-only header
#include "tsdev_lib.h"
#include "process_inspection.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
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
// Ordinal_28 (FUN_101a03f0): __stdcall, 4 args, RET 0x10.
//   uint (int category, int progId, int flag, void* outBuf)
//   Writes a 0x10A-byte partial copy into outBuf. Returns 1 if found/copied.
//
// Ordinal_70 (FUN_1015df30): __stdcall, 2 args, RET 0x08.
//   int (void* rec, int flag) — releases refcounted record.
//
// _LoadMasterRecsEX@16 (ord 568): __stdcall, 4 args, RET 0x10.
//   int (int category, HGLOBAL* outBuf, const char* path, int filterMode)
//   Returns count; allocates HGLOBAL of count * 0x10A bytes. ProgId at +0x00
//   (2-byte short) in each record.
//
// Linker FUN_101a3340 (ordinal 85): __fastcall/__stdcall hybrid — ECX=type,
//   EDX=id, stack: errorFlag (int), partialBuf (void*). We use the manual
//   inline-asm wrapper from diag_m4_frida_linker.cpp so the call is robust
//   to either RET 0x10 (stdcall, popped 16) or RET 0x8 (fastcall, popped 8)
//   cleanup, AND we load ECX=type, EDX=id so the call is valid under BOTH
//   conventions.
// -------------------------------------------------------------------------
typedef unsigned int (__stdcall *Ordinal28Fn)(int category, int progId,
                                              int flag, void* outBuf);
typedef int   (__stdcall *Ordinal70Fn)(void* rec, int flag);
typedef int   (__stdcall *LoadMasterRecsExFn)(int category, HGLOBAL* outBuf,
                                              const char* path,
                                              int filterMode);

// -------------------------------------------------------------------------
// SEH-protected call wrappers. POD-only so __try/__except is legal under
// MSVC C2712.
// -------------------------------------------------------------------------
struct Ord28SehResult { unsigned int result; };
bool callOrd28(Ordinal28Fn fn, int category, int progId, int flag,
               void* outBuf, Ord28SehResult* out) {
    out->result = 0xFFFFFFFFu;
    __try { out->result = fn(category, progId, flag, outBuf); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct Ord70SehResult { int result; };
bool callOrd70(Ordinal70Fn fn, void* rec, int flag, Ord70SehResult* out) {
    out->result = -1;
    __try { out->result = fn(rec, flag); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct LoadMasterRecsExSehResult { int result; };
bool callLoadMasterRecsEx(LoadMasterRecsExFn fn, int category, HGLOBAL* outBuf,
                          const char* path, int filterMode,
                          LoadMasterRecsExSehResult* out) {
    out->result = -1;
    __try { out->result = fn(category, outBuf, path, filterMode); }
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
//
// COPIED EXACTLY from diag_m4_frida_linker.cpp.
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
bool sehReadUInt(const void* p, unsigned int* out) {
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
// Hex dump of a byte buffer (canonical 16-byte rows with ASCII gutter).
// -------------------------------------------------------------------------
void hexDump(const unsigned char* buf, size_t n, const char* label) {
    std::printf("    %s (%zu bytes):\n", label, n);
    for (size_t i = 0; i < n; i += 16) {
        std::printf("    %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            size_t off = i + j;
            if (off < n) {
                std::printf("%02x ", buf[off]);
            } else {
                std::printf("   ");
            }
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16; j++) {
            size_t off = i + j;
            if (off < n) {
                unsigned char c = buf[off];
                std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
            } else {
                std::printf(" ");
            }
        }
        std::printf("|\n");
    }
}

// -------------------------------------------------------------------------
// Existing-record enumeration via _LoadMasterRecsEX@16.
// Read-only — does NOT modify any record.
// -------------------------------------------------------------------------
struct ExistingRec {
    short progId = 0;
    char name[64] = {};  // inline long name at +0x17 (null-terminated)
};

std::vector<ExistingRec> enumerateExisting(LoadMasterRecsExFn loadFn,
                                           int category, int maxCount,
                                           const char* catLabel) {
    std::vector<ExistingRec> result;
    if (!loadFn) {
        std::printf("  [_LoadMasterRecsEX@16 NOT resolved for %s]\n",
                    catLabel);
        return result;
    }

    HGLOBAL buf = NULL;
    LoadMasterRecsExSehResult seh;
    // Per task spec: _LoadMasterRecsEX@16(category, 1, &buf, &count).
    // Per diag_m4_subtype ABI: (category, outBuf, path, filterMode) with
    // filterMode=1. We follow the diag_m4_subtype ABI (proven to work).
    if (!callLoadMasterRecsEx(loadFn, category, &buf,
                              (const char*)NULL, /*filterMode=*/1, &seh)) {
        std::printf("  [_LoadMasterRecsEX@16 raised SEH for %s]\n",
                    catLabel);
        return result;
    }
    int count = seh.result;
    std::printf("  [_LoadMasterRecsEX@16(%d, 1) returned count=%d, "
                "buf=%p]\n", category, count, (void*)buf);
    if (count <= 0 || buf == NULL) {
        if (buf) GlobalFree(buf);
        return result;
    }

    char* records = (char*)GlobalLock(buf);
    if (!records) {
        std::printf("  [GlobalLock FAILED for %s buf]\n", catLabel);
        GlobalFree(buf);
        return result;
    }

    const int kRecordSize   = 0x10A;
    const int kNameOffset   = 0x17;
    const int kProgIdOffset = 0x00;

    int collected = 0;
    for (int i = 0; i < count && collected < maxCount; i++) {
        char* rec = records + (size_t)i * kRecordSize;
        ExistingRec er;
        if (!sehReadShort(rec + kProgIdOffset, &er.progId)) continue;
        if (er.progId <= 0) continue;
        const char* namePtr = rec + kNameOffset;
        size_t nameLen = 0;
        while (nameLen < 63 && namePtr[nameLen] != '\0') {
            er.name[nameLen] = namePtr[nameLen];
            nameLen++;
        }
        er.name[nameLen] = '\0';
        result.push_back(er);
        collected++;
    }

    GlobalUnlock(buf);
    GlobalFree(buf);
    return result;
}

// -------------------------------------------------------------------------
// Per-item test: Steps 1-3 from the task spec.
// READ-ONLY — does NOT modify any item.
// -------------------------------------------------------------------------
struct ItemTestResult {
    int progId = 0;
    // Step 1 (partial copy via Ord28)
    bool ord28_ok = false;
    unsigned int ord28_ret = 0;
    unsigned char gate79 = 0;       // +0x79 byte
    bool gate_set = false;          // bit 2 (0x04) set?
    int partial_elemCount = 0;      // +0x29
    void* partial_elemArray = nullptr;  // +0x4f
    // Step 2 (linker)
    bool linker_ok = false;
    bool linker_seh = false;
    void* rec = nullptr;
    int rec_elemCount = 0;          // +0x29
    void* rec_elemArray = nullptr;  // +0x4f
    unsigned char rec_verified = 0; // +0x55
    void* depListPtr = nullptr;     // +0x6f  (THE KEY FIELD)
    unsigned int depListSize = 0;   // +0x73  (THE KEY FIELD)
    bool depList_dumped = false;
    bool released = false;
    // Step 3 (element+0x7b)
    int element_count_read = 0;
};

void testItem(int category, int progId, const char* name,
              const char* catLabel,
              Ordinal28Fn ord28, Ordinal70Fn ord70, void* linkerRaw,
              ItemTestResult* out) {
    out->progId = progId;
    std::printf("\n  >>> %s ProgId=%d name=\"%s\" <<<\n",
                catLabel, progId, name);

    // --- Step 1: Get partial record via Ordinal_28 ---
    std::printf("  [Step 1] Ordinal_28(%d, %d, 0, buf)...\n",
                category, progId);
    unsigned char partialBuf[0x10A] = {};
    if (ord28) {
        Ord28SehResult r;
        bool ok = callOrd28(ord28, category, progId, /*flag=*/0,
                            partialBuf, &r);
        out->ord28_ok = ok;
        out->ord28_ret = r.result;
        std::printf("    Ordinal_28: %s, ret=%u\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.result);
        if (ok && r.result == 1) {
            unsigned char b79 = 0;
            sehReadByte(partialBuf + 0x79, &b79);
            out->gate79 = b79;
            out->gate_set = (b79 & 0x04) != 0;
            int ec = 0; sehReadInt(partialBuf + 0x29, &ec);
            void* eap = nullptr; sehReadPtr(partialBuf + 0x4f, &eap);
            out->partial_elemCount = ec;
            out->partial_elemArray = eap;
            std::printf("    ProgId=%d +0x79=0x%02X gate=%s "
                        "elemCount=%d elemArray=%p\n",
                        progId, (unsigned)b79,
                        out->gate_set ? "SET(bit2)" : "CLEAR",
                        ec, eap);
        } else {
            std::printf("    [Ordinal_28 did not return 1 — skipping "
                        "partial reads]\n");
        }
    } else {
        std::printf("    [Ordinal_28 not resolved — skipping Step 1]\n");
    }

    // --- Step 2: Call the linker directly (ordinal 85) ---
    std::printf("  [Step 2] linker(%d, %d, 0, partialBuf=%p)...\n",
                category, progId,
                out->ord28_ok ? (void*)partialBuf : nullptr);
    LinkerSehResult r;
    bool ok = callLinkerSafe(linkerRaw, /*type=*/category, progId,
                             /*errorFlag=*/0,
                             out->ord28_ok ? partialBuf : nullptr, &r);
    out->linker_ok = ok;
    out->linker_seh = r.seh;
    out->rec = r.ptr;
    std::printf("    linker call: %s, rec=%p%s\n",
                ok ? "OK (no SEH)" : "SEH raised", r.ptr,
                r.seh ? " [SEH]" : "");

    if (ok && r.ptr) {
        const unsigned char* rec = (const unsigned char*)r.ptr;
        int ec = 0; sehReadInt(rec + 0x29, &ec);
        void* eap = nullptr; sehReadPtr(rec + 0x4f, &eap);
        unsigned char vf = 0; sehReadByte(rec + 0x55, &vf);
        void* dlp = nullptr; sehReadPtr(rec + 0x6f, &dlp);
        unsigned int dls = 0; sehReadUInt(rec + 0x73, &dls);
        out->rec_elemCount = ec;
        out->rec_elemArray = eap;
        out->rec_verified = vf;
        out->depListPtr = dlp;
        out->depListSize = dls;
        std::printf("    ProgId=%d linker OK elemCount=%d elemArray=%p "
                    "depListPtr=%p depListSize=%u\n",
                    progId, ec, eap, dlp, dls);
        std::printf("    +0x55 verified flag=0x%02X\n", (unsigned)vf);

        if (dlp && dls > 0) {
            std::printf("    *** BREAKTHROUGH: dep-list POPULATED "
                        "(ptr=%p, size=%u) ***\n", dlp, dls);
            // Dump first 64 bytes of the dep-list buffer (SEH-protected).
            unsigned char dumpBuf[64] = {};
            size_t dumpN = dls < 64 ? dls : 64;
            if (sehMemcpy(dlp, dumpBuf, dumpN)) {
                out->depList_dumped = true;
                hexDump(dumpBuf, dumpN, "dep-list first 64 bytes");
            } else {
                std::printf("    [SEH reading dep-list buffer — dump "
                            "skipped]\n");
            }
        } else {
            std::printf("    [dep-list NOT populated (ptr=%p, size=%u)]\n",
                        dlp, dls);
        }

        // Release via Ordinal_70(rec, 0)
        if (ord70) {
            Ord70SehResult r70;
            bool ok70 = callOrd70(ord70, r.ptr, 0, &r70);
            out->released = ok70;
            std::printf("    [release] Ordinal_70(rec, 0): %s, ret=%d\n",
                        ok70 ? "OK" : "SEH", r70.result);
        }
    } else {
        std::printf("    ProgId=%d linker returned NULL\n", progId);
    }

    // --- Step 3: Read element+0x7b for comparison ---
    if (out->rec_elemCount > 0 && out->rec_elemArray) {
        std::printf("  [Step 3] Reading element+0x73/+0x7b (up to 5 "
                    "elements)...\n");
        int maxE = out->rec_elemCount < 5 ? out->rec_elemCount : 5;
        const size_t kElemSize = 0xb5;
        for (int ei = 0; ei < maxE; ei++) {
            const unsigned char* elem =
                (const unsigned char*)out->rec_elemArray +
                (size_t)ei * kElemSize;
            unsigned int esz = 0; sehReadUInt(elem + 0x73, &esz);
            void* eptr = nullptr; sehReadPtr(elem + 0x7b, &eptr);
            std::printf("    element[%d] +0x73(size)=%u +0x7b(ptr)=%p\n",
                        ei, esz, eptr);
            out->element_count_read++;
        }
    } else {
        std::printf("  [Step 3] Skipped (elemCount=%d, elemArray=%p)\n",
                    out->rec_elemCount, out->rec_elemArray);
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

    std::printf("=== diag_m4_existing_link: linker dep-list (rec+0x6f/+0x73) "
                "diagnostic ===\n");
    std::printf("Purpose: Test the Ghidra finding that the linker (ord 85) "
                "writes the dep-list\n");
    std::printf("  to record+0x6f (ptr) and record+0x73 (size), while "
                "GetDependencies reads\n");
    std::printf("  from element+0x7b (different location). READ-ONLY on "
                "existing items.\n");

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
    std::printf("\n[2] Resolving Ordinal_28 (28), Ordinal_70 (70), "
                "_LoadMasterRecsEX@16, and ordinal 85 (linker)...\n");
    Ordinal28Fn ord28 = nullptr;
    Ordinal70Fn ord70 = nullptr;
    LoadMasterRecsExFn loadMasterRecsEx = nullptr;
    void* linkerRaw = nullptr;
    try {
        OrlibResolver resolver(L"orlib20.dll");
        ord28 = reinterpret_cast<Ordinal28Fn>(
            resolver.resolveExportByOrdinal(28));
        ord70 = reinterpret_cast<Ordinal70Fn>(
            resolver.resolveExportByOrdinal(70));
        loadMasterRecsEx = reinterpret_cast<LoadMasterRecsExFn>(
            resolver.resolveExport("_LoadMasterRecsEX@16"));
        FARPROC ord85 = resolver.resolveExportByOrdinal(85);
        if (ord85) linkerRaw = (void*)ord85;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: OrlibResolver threw: %s\n", e.what());
        st->exitCode = 102;
        return 0;
    }
    std::printf("    Resolved: Ord28=%p Ord70=%p LoadMasterRecsEX=%p "
                "Linker(85)=%p\n",
                (void*)ord28, (void*)ord70, (void*)loadMasterRecsEx,
                linkerRaw);
    if (!ord28) std::fprintf(stderr, "WARN: Ordinal_28 unresolved\n");
    if (!ord70) std::fprintf(stderr, "WARN: Ordinal_70 unresolved\n");
    if (!loadMasterRecsEx)
        std::fprintf(stderr, "WARN: _LoadMasterRecsEX@16 unresolved\n");
    if (!linkerRaw) {
        std::fprintf(stderr, "FATAL: ordinal 85 (linker) unresolved\n");
        st->exitCode = 103;
        return 0;
    }

    // --- Step 3: enumerate existing items and test each ---
    std::printf("\n[3] Enumerating EXISTING items (READ-ONLY — no "
                "modification)...\n");

    // 3a. Existing Strategies (category 0).
    std::printf("\n  --- 3a. Existing Strategies (category 0) ---\n");
    auto existingStrats = enumerateExisting(loadMasterRecsEx, /*cat=*/0,
                                            /*maxCount=*/5, "Strategy");
    std::vector<ItemTestResult> stratResults;
    for (const auto& er : existingStrats) {
        ItemTestResult tr;
        testItem(/*category=*/0, er.progId, er.name, "Strategy",
                 ord28, ord70, linkerRaw, &tr);
        stratResults.push_back(tr);
    }
    if (existingStrats.empty()) {
        std::printf("    [no existing Strategies enumerated]\n");
    }

    // 3b. Existing Functions (category 4).
    std::printf("\n  --- 3b. Existing Functions (category 4) ---\n");
    auto existingFuncs = enumerateExisting(loadMasterRecsEx, /*cat=*/4,
                                           /*maxCount=*/5, "Function");
    std::vector<ItemTestResult> funcResults;
    for (const auto& er : existingFuncs) {
        ItemTestResult tr;
        testItem(/*category=*/4, er.progId, er.name, "Function",
                 ord28, ord70, linkerRaw, &tr);
        funcResults.push_back(tr);
    }
    if (existingFuncs.empty()) {
        std::printf("    [no existing Functions enumerated]\n");
    }

    // 3c. Existing Indicators (category 2).
    std::printf("\n  --- 3c. Existing Indicators (category 2) ---\n");
    auto existingInds = enumerateExisting(loadMasterRecsEx, /*cat=*/2,
                                          /*maxCount=*/5, "Indicator");
    std::vector<ItemTestResult> indResults;
    for (const auto& er : existingInds) {
        ItemTestResult tr;
        testItem(/*category=*/2, er.progId, er.name, "Indicator",
                 ord28, ord70, linkerRaw, &tr);
        indResults.push_back(tr);
    }
    if (existingInds.empty()) {
        std::printf("    [no existing Indicators enumerated]\n");
    }

    // --- Final summary ---
    std::printf("\n=== FINAL SUMMARY ===\n");
    auto printSummary = [](const char* label,
                           const std::vector<ItemTestResult>& rs) {
        std::printf("\n--- %s (count=%zu) ---\n", label, rs.size());
        for (const auto& tr : rs) {
            std::printf("  ProgId=%d: ord28=%s(ret=%u) gate79=0x%02X(%s) "
                        "partial[ec=%d arr=%p] | linker=%s rec=%p "
                        "rec[ec=%d arr=%p ver=0x%02X] depList[ptr=%p "
                        "size=%u] released=%s\n",
                        tr.progId,
                        tr.ord28_ok ? "OK" : "SEH",
                        tr.ord28_ret,
                        (unsigned)tr.gate79,
                        tr.gate_set ? "SET" : "CLEAR",
                        tr.partial_elemCount, tr.partial_elemArray,
                        tr.linker_ok ? "OK" : "SEH",
                        tr.rec,
                        tr.rec_elemCount, tr.rec_elemArray,
                        (unsigned)tr.rec_verified,
                        tr.depListPtr, tr.depListSize,
                        tr.released ? "yes" : "no");
        }
    };
    printSummary("Strategies", stratResults);
    printSummary("Functions", funcResults);
    printSummary("Indicators", indResults);

    // Aggregate breakthrough detection
    int breakthroughs = 0;
    for (const auto& tr : stratResults)
        if (tr.depListPtr && tr.depListSize > 0) breakthroughs++;
    for (const auto& tr : funcResults)
        if (tr.depListPtr && tr.depListSize > 0) breakthroughs++;
    for (const auto& tr : indResults)
        if (tr.depListPtr && tr.depListSize > 0) breakthroughs++;
    std::printf("\n>>> DEP-LIST BREAKTHROUGH COUNT: %d (items with "
                "rec+0x6f!=NULL && rec+0x73>0) <<<\n", breakthroughs);

    int gateSetCount = 0;
    for (const auto& tr : stratResults) if (tr.gate_set) gateSetCount++;
    for (const auto& tr : funcResults) if (tr.gate_set) gateSetCount++;
    for (const auto& tr : indResults) if (tr.gate_set) gateSetCount++;
    std::printf(">>> +0x79 GATE FLAG (bit 2) SET COUNT: %d <<<\n",
                gateSetCount);

    std::printf("\n=== DIAG COMPLETE ===\n");
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
                     "inspected before the diagnostic; no process was terminated.\n");
        return 97;
    }

    // Run on a 64 MB-stack worker thread (TsDevLib is thread-affine and
    // orlib20 calls need a large stack).
    DiagState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runDiag, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    // 180-second hard timeout — report a hang without terminating the worker.
    // Enumeration of 3 categories + linker calls on 15 items may take longer
    // than 120 seconds.
    DWORD wait = WaitForSingleObject(hThread, 180000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 180s; worker was "
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
                     "FATAL: TSCrashReport.exe was detected or could not be inspected after the "
                     "diagnostic; no process cleanup was attempted.\n");
        return 97;
    }

    return st.exitCode;
}
