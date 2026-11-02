// test_master_recs_seh.cpp
//
// TDD unit test for residual RC-13: HGLOBAL leak on the _LoadMasterRecsEX
// SEH path.
//
// Production contract under test (src/tsdev_lib.cpp): the callee reports
// its record buffer through an OUT HGLOBAL* slot. If it raises AFTER
// allocating that buffer, the Impl __except handler must take ownership
// (GlobalFree + NULL the slot) so the single call site
// (TsDevLib::enumerateReverseDependents) cannot leak it on its SEH branch.
//
// The test drives the EXACT production helper through the test-only seam
// tsdev::invokeLoadMasterRecsExForTest (declared in
// src/tsdev_lib_test_hooks.h) with stub callees: no TsDevLib instance, no
// orlib20.dll, no TradeStation installation. PURE UNIT TEST, runs under
// -NoTs.
//
// Checks:
//   1. SEH-after-alloc: stub stores a real GlobalAlloc handle in *outBuf
//      then raises -> seam returns false, *outBuf is NULL (ownership
//      taken), and the saved handle is freed (GlobalSize == 0).
//   2. Clean SEH: stub raises with *outBuf left NULL -> false + NULL slot
//      (handler must not crash on the empty slot).
//   3. Normal path unaffected: stub stores a handle and returns a count ->
//      true + count preserved + handle still valid (caller still owns it;
//      the test frees it).
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/tsdev_lib_test_hooks.h"

#include <cstdio>
#include <windows.h>

namespace {

int fail(const char* message, int code) {
    std::fprintf(stderr, "[FAIL] %s (code=%d)\n", message, code);
    return code;
}

// Stub 1: emulate orlib20 allocating the record buffer, then faulting
// (access violation) before returning. g_allocHandle lets the test verify
// the exact handle's lifetime after the seam returns.
HGLOBAL g_allocHandle = NULL;

int __stdcall stubAllocThenRaise(int /*category*/, HGLOBAL* outBuf,
                                 const char* /*path*/, int /*filterMode*/) {
    g_allocHandle = GlobalAlloc(GMEM_MOVEABLE, 64);
    if (!g_allocHandle) return 0;  // OOM: not the path under test
    *outBuf = g_allocHandle;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return 0;  // unreachable
}

// Stub 2: raise with the OUT slot left NULL (callee failed before any
// allocation).
int __stdcall stubRaiseNoAlloc(int /*category*/, HGLOBAL* /*outBuf*/,
                               const char* /*path*/, int /*filterMode*/) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return 0;  // unreachable
}

// Stub 3: normal path -- store a handle, return a record count, no raise.
int __stdcall stubNormal(int /*category*/, HGLOBAL* outBuf,
                          const char* /*path*/, int /*filterMode*/) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, 32);
    if (!h) return -1;
    *outBuf = h;
    return 7;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_master_recs_seh: LoadMasterRecsEX SEH ownership "
                 "(unit) ===\n");

    // [1] SEH-after-alloc: no leak, slot NULLED, result stays -1.
    {
        g_allocHandle = NULL;
        HGLOBAL buf = NULL;
        int result = -999;
        bool ok = tsdev::invokeLoadMasterRecsExForTest(
            &stubAllocThenRaise, 1, &buf, nullptr, 1, &result);
        if (ok) {
            if (buf) GlobalFree(buf);
            return fail("SEH raise must return false", 10);
        }
        if (result != -1) {
            if (buf) GlobalFree(buf);
            return fail("SEH path must leave result at -1", 11);
        }
        if (buf != NULL) {
            // RED signal: ownership NOT taken -- caller would leak this.
            // Free it here so the test process itself stays clean.
            GlobalFree(buf);
            return fail("SEH-after-alloc must NULL the OUT slot (leak)", 12);
        }
        // Slot is NULL: the handler claims ownership. Prove the memory was
        // actually freed (not just detached): the saved handle must no
        // longer denote a live allocation.
        SIZE_T size = GlobalSize(g_allocHandle);
        if (size != 0) {
            GlobalFree(g_allocHandle);
            return fail("SEH-after-alloc handle still live (leak)", 13);
        }
        std::fprintf(stdout, "  [1] PASS: SEH-after-alloc freed + NULLED\n");
    }

    // [2] Clean SEH (no allocation): false + NULL slot, no crash.
    {
        HGLOBAL buf = NULL;
        int result = -999;
        bool ok = tsdev::invokeLoadMasterRecsExForTest(
            &stubRaiseNoAlloc, 2, &buf, nullptr, 1, &result);
        if (ok) {
            return fail("clean SEH raise must return false", 20);
        }
        if (buf != NULL || result != -1) {
            if (buf) GlobalFree(buf);
            return fail("clean SEH must leave NULL slot and -1", 21);
        }
        std::fprintf(stdout, "  [2] PASS: clean SEH leaves NULL slot\n");
    }

    // [3] Normal path: result + live handle pass through untouched.
    {
        HGLOBAL buf = NULL;
        int result = -999;
        bool ok = tsdev::invokeLoadMasterRecsExForTest(
            &stubNormal, 3, &buf, nullptr, 1, &result);
        if (!ok) {
            return fail("normal stub must return true", 30);
        }
        if (result != 7) {
            if (buf) GlobalFree(buf);
            return fail("normal stub count must pass through", 31);
        }
        if (buf == NULL || GlobalSize(buf) == 0) {
            if (buf) GlobalFree(buf);
            return fail("normal stub handle must stay live (caller-owned)",
                        32);
        }
        GlobalFree(buf);  // caller releases: ownership stayed with caller
        std::fprintf(stdout, "  [3] PASS: normal path passes handle through\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}
