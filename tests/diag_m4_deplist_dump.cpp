// diag_m4_deplist_dump.cpp
//
// DIAGNOSTIC ONLY (not part of the regression suite). Read-only with respect
// to production code: this file does NOT modify any src/ file.
//
// Purpose: Dump the FULL dep-list content for existing Strategies and
// Indicators. This historical diagnostic probes only 0xfa4 entries; the later
// M.4 evidence found compiler-written function references use 0xfa3 instead.
// It is retained to document the rejected expectation and is not current
// production or M.4 acceptance logic.
//
// Background (confirmed by prior diagnostics):
//   - The linker (ordinal 85) populates element arrays for existing items.
//   - Dep-lists are at element+0x7b (ptr) and element+0x73 (size) per element.
//   - Dep-list entries have format:
//       {type dword, length byte, sourceLine int, sourceCol int, payload}
//     i.e. type at +0x00 (4 bytes), length at +0x04 (1 byte), sourceLine at
//     +0x05 (4 bytes), sourceCol at +0x09 (4 bytes), payload at +0x0d.
//   - Historical expected type 0xfa4 = function dependency, with function
//     reference (a ProgId) at entry+0x0d (4-byte int). The M.4 evidence found
//     compiler-written function references use 0xfa3.
//   - Strategies and Indicators have populated dep-lists; Functions do not.
//
// Flow:
//   1. Print PID and pre-check TSCrashReport.exe.
//   2. Construct a TsDevLib (test-only ctor, real ComApi, no hooks) on a
//      64 MB-stack worker thread.
//   3. Resolve Ordinal_28, Ordinal_70, _LoadMasterRecsEX@16, and ordinal 85
//      (linker).
//   4. Enumerate existing Strategies via _LoadMasterRecsEX@16(0, 1, ...).
//      Pick the first 10.
//   5. For each existing Strategy:
//        a. Call Ordinal_28(0, progId, 0, buf) to get the partial record.
//        b. Call linker(0, progId, 0, partialBuf) using callLinkerSafe.
//        c. If rec is non-NULL:
//             - Read rec+0x29 (elemCount), rec+0x4f (elemArrayPtr).
//             - For each element (0 to elemCount-1):
//                 element = elemArrayPtr + elemIdx * 0xb5
//                 Read element+0x73 (dep-list size) and
//                 element+0x7b (dep-list ptr).
//                 If dep-list ptr is non-NULL and size > 0:
//                   Dump the FULL dep-list buffer (up to 4096 bytes,
//                   SEH-protected).
//                   Parse the entries: for each entry, print type (dword at
//                   +0), length (byte at +4), and, for the historical 0xfa4
//                   expectation, print the function reference (dword at +0xd).
//                   Print: "ProgId=X elem[Y] depListSize=S entries:
//                   [type=0xT len=L ref=R]..."
//             - Release via Ordinal_70(rec, 0).
//   6. Also test 10 existing Indicators (category 2) with the same approach.
//   7. Also test 5 existing Functions (category 4) — although empty dep-lists
//      were the historical expectation, verify and print for completeness.
//   8. Check for TSCrashReport.exe after all operations.
//
// All orlib20 calls are SEH-protected (__try/__except). READ-ONLY — does NOT
// insert/modify/delete any items. Its 240-second limit is an operational
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
    // Per diag_m4_subtype ABI: (category, outBuf, path, filterMode) with
    // filterMode=1. Proven to work.
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
// Dep-list parsing.
//
// Entry layout (per task spec):
//   +0x00: type   (dword, 4 bytes)
//   +0x04: length (byte, 1 byte)  — total entry length INCLUDING this header
//   +0x05: sourceLine (int, 4 bytes)
//   +0x09: sourceCol  (int, 4 bytes)
//   +0x0d: payload    (variable; under the historical 0xfa4 expectation, the
//                      first 4 bytes are the function reference / ProgId)
//
// We walk the buffer from offset 0, advancing by the length byte at +0x04
// each iteration. If length is 0, we break to avoid an infinite loop. We
// also stop if we walk past the buffer end.
// -------------------------------------------------------------------------
struct ParsedEntry {
    unsigned int type;
    unsigned char length;
    int sourceLine;
    int sourceCol;
    int funcRef;  // historical field: used only for type == 0xfa4
};

struct ParseResult {
    std::vector<ParsedEntry> entries;
    bool truncated = false;     // hit buffer end mid-entry
    bool zeroLength = false;    // encountered a zero-length entry (stopped)
    bool sehError = false;
};

ParseResult parseDepList(const unsigned char* buf, size_t bufLen) {
    ParseResult pr;
    size_t off = 0;
    while (off + 5 <= bufLen) {
        ParsedEntry e;
        e.type        = *(const unsigned int*)(buf + off + 0);
        e.length      = *(const unsigned char*)(buf + off + 4);
        if (e.length == 0) {
            pr.zeroLength = true;
            break;
        }
        if (off + e.length > bufLen) {
            pr.truncated = true;
            break;
        }
        // sourceLine at +0x05, sourceCol at +0x09 (if present)
        if (e.length >= 13) {
            e.sourceLine = *(const int*)(buf + off + 5);
            e.sourceCol  = *(const int*)(buf + off + 9);
            e.funcRef    = *(const int*)(buf + off + 13);
        } else {
            e.sourceLine = 0;
            e.sourceCol  = 0;
            e.funcRef    = 0;
        }
        pr.entries.push_back(e);
        off += e.length;
    }
    return pr;
}

// -------------------------------------------------------------------------
// Per-item test: get partial record, call linker, walk element array,
// dump + parse each element's dep-list (element+0x73 size, element+0x7b ptr).
// READ-ONLY — does NOT modify any item.
// -------------------------------------------------------------------------
struct ElementDepInfo {
    int elemIdx = 0;
    unsigned int depListSize = 0;   // element+0x73
    void* depListPtr = nullptr;     // element+0x7b
    bool dumped = false;
    bool sehOnRead = false;
    int entryCount = 0;
    int fa4Count = 0;
    std::vector<ParsedEntry> entries;
};

struct ItemTestResult {
    int progId = 0;
    char name[64] = {};
    // Step 1 (partial copy via Ord28)
    bool ord28_ok = false;
    unsigned int ord28_ret = 0;
    // Step 2 (linker)
    bool linker_ok = false;
    bool linker_seh = false;
    void* rec = nullptr;
    int rec_elemCount = 0;          // +0x29
    void* rec_elemArray = nullptr;  // +0x4f
    bool released = false;
    // Step 3 (per-element dep-lists)
    std::vector<ElementDepInfo> elements;
};

void testItem(int category, int progId, const char* name,
              const char* catLabel,
              Ordinal28Fn ord28, Ordinal70Fn ord70, void* linkerRaw,
              ItemTestResult* out) {
    out->progId = progId;
    strncpy(out->name, name, 63);
    out->name[63] = '\0';
    std::printf("\n  >>> %s ProgId=%d name=\"%s\" <<<\n",
                catLabel, progId, name);

    // --- Step 1: Get partial record via Ordinal_28 ---
    std::printf("  [Step 1] Ordinal_28(%d, %d, 0, buf)...\n",
                category, progId);
    unsigned char partialBuf[0x10A] = {};
    bool havePartial = false;
    if (ord28) {
        Ord28SehResult r;
        bool ok = callOrd28(ord28, category, progId, /*flag=*/0,
                            partialBuf, &r);
        out->ord28_ok = ok;
        out->ord28_ret = r.result;
        std::printf("    Ordinal_28: %s, ret=%u\n",
                    ok ? "OK (no SEH)" : "SEH raised", r.result);
        if (ok && r.result == 1) {
            havePartial = true;
        } else {
            std::printf("    [Ordinal_28 did not return 1 — skipping "
                        "partial reads]\n");
        }
    } else {
        std::printf("    [Ordinal_28 not resolved — skipping Step 1]\n");
    }

    // --- Step 2: Call the linker directly (ordinal 85) ---
    std::printf("  [Step 2] linker(%d, %d, 0, partialBuf=%p)...\n",
                category, progId, havePartial ? (void*)partialBuf : nullptr);
    LinkerSehResult r;
    bool ok = callLinkerSafe(linkerRaw, /*type=*/category, progId,
                             /*errorFlag=*/0,
                             havePartial ? partialBuf : nullptr, &r);
    out->linker_ok = ok;
    out->linker_seh = r.seh;
    out->rec = r.ptr;
    std::printf("    linker call: %s, rec=%p%s\n",
                ok ? "OK (no SEH)" : "SEH raised", r.ptr,
                r.seh ? " [SEH]" : "");

    if (!(ok && r.ptr)) {
        std::printf("    ProgId=%d linker returned NULL\n", progId);
        return;
    }

    const unsigned char* rec = (const unsigned char*)r.ptr;
    int ec = 0; sehReadInt(rec + 0x29, &ec);
    void* eap = nullptr; sehReadPtr(rec + 0x4f, &eap);
    out->rec_elemCount = ec;
    out->rec_elemArray = eap;
    std::printf("    ProgId=%d linker OK elemCount=%d elemArray=%p\n",
                progId, ec, eap);

    // --- Step 3: Walk element array, dump + parse each dep-list ---
    if (ec <= 0 || !eap) {
        std::printf("  [Step 3] Skipped (elemCount=%d, elemArray=%p)\n",
                    ec, eap);
    } else {
        std::printf("  [Step 3] Walking %d elements (elem size 0xb5)...\n",
                    ec);
        const size_t kElemSize = 0xb5;
        for (int ei = 0; ei < ec; ei++) {
            ElementDepInfo ed;
            ed.elemIdx = ei;
            const unsigned char* elem =
                (const unsigned char*)eap + (size_t)ei * kElemSize;

            unsigned int esz = 0;
            void* eptr = nullptr;
            bool okSize = sehReadUInt(elem + 0x73, &esz);
            bool okPtr  = sehReadPtr (elem + 0x7b, &eptr);
            ed.depListSize = esz;
            ed.depListPtr  = eptr;
            if (!okSize || !okPtr) {
                ed.sehOnRead = true;
                std::printf("    element[%d] SEH reading +0x73/+0x7b "
                            "(size=%u ptr=%p)\n", ei, esz, eptr);
                out->elements.push_back(ed);
                continue;
            }

            std::printf("    element[%d] +0x73(size)=%u +0x7b(ptr)=%p\n",
                        ei, esz, eptr);

            if (!eptr || esz == 0) {
                std::printf("      [no dep-list for element[%d]]\n", ei);
                out->elements.push_back(ed);
                continue;
            }

            // Dump the FULL dep-list buffer (up to 4096 bytes).
            const size_t kMaxDump = 4096;
            size_t dumpN = esz < kMaxDump ? esz : kMaxDump;
            unsigned char dumpBuf[kMaxDump] = {};
            if (!sehMemcpy(eptr, dumpBuf, dumpN)) {
                std::printf("      [SEH reading dep-list buffer — dump "
                            "skipped]\n");
                ed.sehOnRead = true;
                out->elements.push_back(ed);
                continue;
            }
            ed.dumped = true;

            char label[64];
            std::snprintf(label, sizeof(label),
                          "ProgId=%d elem[%d] dep-list (size=%u, dumping "
                          "%zu bytes)", progId, ei, esz, dumpN);
            hexDump(dumpBuf, dumpN, label);

            // Parse the entries.
            ParseResult pr = parseDepList(dumpBuf, dumpN);
            ed.entries = pr.entries;
            ed.entryCount = (int)pr.entries.size();

            std::printf("      Parsed %d entries (truncated=%d "
                        "zeroLen=%d):\n",
                        ed.entryCount, (int)pr.truncated,
                        (int)pr.zeroLength);

            // Print one-line summary: "ProgId=X elem[Y] depListSize=S
            // entries: [type=0xT len=L ref=R]..."
            std::printf("      ProgId=%d elem[%d] depListSize=%u entries: ",
                        progId, ei, esz);
            for (size_t k = 0; k < pr.entries.size(); k++) {
                const ParsedEntry& e = pr.entries[k];
                if (e.type == 0xfa4) {
                    ed.fa4Count++;
                    std::printf("[type=0x%X len=%u ref=%d] ",
                                e.type, (unsigned)e.length, e.funcRef);
                } else {
                    std::printf("[type=0x%X len=%u] ",
                                e.type, (unsigned)e.length);
                }
            }
            std::printf("\n");

            // Detailed per-entry print for the historical 0xfa4 expectation.
            for (size_t k = 0; k < pr.entries.size(); k++) {
                const ParsedEntry& e = pr.entries[k];
                if (e.type == 0xfa4) {
                    std::printf("        0xfa4 entry[%zu]: len=%u "
                                "srcLine=%d srcCol=%d funcRef=%d "
                                "(0x%X) [%s]\n",
                                k, (unsigned)e.length,
                                e.sourceLine, e.sourceCol,
                                e.funcRef, (unsigned)e.funcRef,
                                (e.funcRef > 0 && e.funcRef < 100000)
                                    ? "LOOKS LIKE PROGID"
                                    : "NOT a typical ProgId range");
                }
            }

            if (pr.truncated) {
                std::printf("      [PARSE TRUNCATED at dumpN=%zu — "
                            "dep-list may continue beyond %zu bytes]\n",
                            dumpN, dumpN);
            }
            if (pr.zeroLength) {
                std::printf("      [PARSE STOPPED: zero-length entry "
                            "encountered]\n");
            }

            out->elements.push_back(ed);
        }
    }

    // Release via Ordinal_70(rec, 0)
    if (ord70) {
        Ord70SehResult r70;
        bool ok70 = callOrd70(ord70, r.ptr, 0, &r70);
        out->released = ok70;
        std::printf("    [release] Ordinal_70(rec, 0): %s, ret=%d\n",
                    ok70 ? "OK" : "SEH", r70.result);
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

    std::printf("=== diag_m4_deplist_dump: full dep-list dump for existing "
                "Strategies/Indicators/Functions ===\n");
    std::printf("Purpose: Verify 0xfa4 (function dependency) entries are "
                "present in\n");
    std::printf("  Strategy/Indicator dep-lists with valid function "
                "references (ProgIds)\n");
    std::printf("  at entry+0xd. READ-ONLY on existing items — no "
                "insert/modify/delete.\n");

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

    // 3a. Existing Strategies (category 0) — first 10.
    std::printf("\n  --- 3a. Existing Strategies (category 0, first 10) "
                "---\n");
    auto existingStrats = enumerateExisting(loadMasterRecsEx, /*cat=*/0,
                                            /*maxCount=*/10, "Strategy");
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

    // 3b. Existing Indicators (category 2) — first 10.
    std::printf("\n  --- 3b. Existing Indicators (category 2, first 10) "
                "---\n");
    auto existingInds = enumerateExisting(loadMasterRecsEx, /*cat=*/2,
                                          /*maxCount=*/10, "Indicator");
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

    // 3c. Existing Functions (category 4) — first 5 (expected empty
    // dep-lists).
    std::printf("\n  --- 3c. Existing Functions (category 4, first 5 — "
                "expected empty dep-lists) ---\n");
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

    // --- Final summary ---
    std::printf("\n=== FINAL SUMMARY ===\n");

    auto printSummary = [](const char* label,
                           const std::vector<ItemTestResult>& rs) {
        std::printf("\n--- %s (count=%zu) ---\n", label, rs.size());
        for (const auto& tr : rs) {
            int totalFa4 = 0;
            int totalEntries = 0;
            int elemsWithDepList = 0;
            for (const auto& ed : tr.elements) {
                totalFa4 += ed.fa4Count;
                totalEntries += ed.entryCount;
                if (ed.depListPtr && ed.depListSize > 0) elemsWithDepList++;
            }
            std::printf("  ProgId=%d name=\"%s\": ord28=%s(ret=%u) "
                        "linker=%s rec=%p elemCount=%d "
                        "elemsWithDepList=%d totalEntries=%d "
                        "totalFa4=%d released=%s\n",
                        tr.progId, tr.name,
                        tr.ord28_ok ? "OK" : "SEH",
                        tr.ord28_ret,
                        tr.linker_ok ? "OK" : "SEH",
                        tr.rec, tr.rec_elemCount,
                        elemsWithDepList, totalEntries, totalFa4,
                        tr.released ? "yes" : "no");
            // Per-element summary line.
            for (const auto& ed : tr.elements) {
                std::printf("    elem[%d] depListSize=%u ptr=%p "
                            "entries=%d fa4=%d%s\n",
                            ed.elemIdx, ed.depListSize, ed.depListPtr,
                            ed.entryCount, ed.fa4Count,
                            ed.sehOnRead ? " [SEH]" : "");
            }
        }
    };
    printSummary("Strategies", stratResults);
    printSummary("Indicators", indResults);
    printSummary("Functions  (expected empty)", funcResults);

    // Aggregate 0xfa4 detection.
    int stratFa4 = 0, indFa4 = 0, funcFa4 = 0;
    int stratItemsWithFa4 = 0, indItemsWithFa4 = 0, funcItemsWithFa4 = 0;
    std::vector<int> allFa4Refs;
    for (const auto& tr : stratResults) {
        int itemFa4 = 0;
        for (const auto& ed : tr.elements) {
            stratFa4 += ed.fa4Count;
            itemFa4 += ed.fa4Count;
            for (const auto& e : ed.entries)
                if (e.type == 0xfa4) allFa4Refs.push_back(e.funcRef);
        }
        if (itemFa4 > 0) stratItemsWithFa4++;
    }
    for (const auto& tr : indResults) {
        int itemFa4 = 0;
        for (const auto& ed : tr.elements) {
            indFa4 += ed.fa4Count;
            itemFa4 += ed.fa4Count;
            for (const auto& e : ed.entries)
                if (e.type == 0xfa4) allFa4Refs.push_back(e.funcRef);
        }
        if (itemFa4 > 0) indItemsWithFa4++;
    }
    for (const auto& tr : funcResults) {
        int itemFa4 = 0;
        for (const auto& ed : tr.elements) {
            funcFa4 += ed.fa4Count;
            itemFa4 += ed.fa4Count;
            for (const auto& e : ed.entries)
                if (e.type == 0xfa4) allFa4Refs.push_back(e.funcRef);
        }
        if (itemFa4 > 0) funcItemsWithFa4++;
    }

    std::printf("\n>>> 0xfa4 (FUNCTION DEPENDENCY) SUMMARY <<<\n");
    std::printf("  Strategies : %d items with 0xfa4, %d total 0xfa4 "
                "entries (out of %zu items tested)\n",
                stratItemsWithFa4, stratFa4, stratResults.size());
    std::printf("  Indicators : %d items with 0xfa4, %d total 0xfa4 "
                "entries (out of %zu items tested)\n",
                indItemsWithFa4, indFa4, indResults.size());
    std::printf("  Functions  : %d items with 0xfa4, %d total 0xfa4 "
                "entries (out of %zu items tested) [expected 0]\n",
                funcItemsWithFa4, funcFa4, funcResults.size());

    std::printf("\n  All 0xfa4 function references (entry+0xd):\n");
    for (int ref : allFa4Refs) {
        std::printf("    funcRef=%d (0x%X) [%s]\n", ref, (unsigned)ref,
                    (ref > 0 && ref < 100000)
                        ? "LOOKS LIKE PROGID"
                        : "NOT a typical ProgId range");
    }
    if (allFa4Refs.empty()) {
        std::printf("    [no 0xfa4 entries found]\n");
    }

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
    // 240-second hard timeout — report a hang without terminating the worker.
    // Enumeration of 3 categories + linker calls on up to 25 items may take
    // longer than 180 seconds.
    DWORD wait = WaitForSingleObject(hThread, 240000);
    if (wait == WAIT_TIMEOUT) {
        std::fprintf(stderr,
                     "\nFATAL: diagnostic timed out after 240s; worker was "
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
