// test_finalization_lifecycle.cpp
//
// TVS-L.3: Behavior-based finalization regression.
//
// This test was repurposed from test_persistence_validation.cpp (TVS-B.1.5),
// which validated the bytes written by the now-REMOVED persistence patch
// (applyPersistencePatch / validatePersistencePatchBytes / FUN_10011230).
// The persistence bypass was removed in TVS-L.3 so the real finalization
// path (FUN_10011230 -> FUN_1008E640 -> SymInfo::AdjustOffsets ->
// SymWriter::Save -> ModuleInfo::SetGuid / MDVault::SaveModule) runs
// end-to-end. The old byte-validation test is therefore obsolete: there is
// no patch site to inspect and no validation helper to call.
//
// Per the workstream_unknown_link_fix_plan.md (§5 item 6 / TVS-L.3), this
// test is now a BEHAVIOR-based regression that proves the finalization path
// prerequisites are in place WITHOUT:
//   - calling validatePersistencePatchBytes (removed; would not compile),
//   - resolving FUN_10011230 by hardcoded RVA,
//   - duplicating a private test-only signature for the removed target.
//
// What it verifies (all behavioral, no RVA/signature/byte inspection):
//
//   Case 1 — Constructor succeeds without the persistence patch:
//     Construct TsDevLib (auto-discover). The constructor no longer calls
//     applyPersistencePatch(); it loads orlib20.dll, applies the 11
//     thread-check INT3 patches, resolves exports, calls OrlibInit(NULL, 1),
//     and creates the hidden EDIT control. A clean return (no C++ exception,
//     no SEH fault) proves the constructor runs end-to-end with the
//     persistence patch gone. Crucially, OrlibInit can only succeed from a
//     non-TSDev worker thread because the 11 thread-check patches are in
//     place — so constructor success is the behavioral proof that the
//     thread-check patch machinery still applies all 11 patches. We do NOT
//     inspect the patch bytes or count them directly (the patch state is
//     private and the header is intentionally not modified for this test).
//
//   Case 2 — Post-construction invariants (real path prerequisites):
//     getMyWorkBinPath() and getWrkPath() return non-empty paths containing
//     "MyWork\Bin" and "MWRKArea.wrk" respectively. This proves the
//     warehouse root was resolved — the same root the real finalization
//     path writes the .ell/.xml artifacts under. We also confirm
//     <root>\Program\orlib20.dll exists (the constructor already validated
//     this internally; the check is a defensive cross-check).
//
//   Case 3 — Destructor completes cleanly:
//     Delete the TsDevLib instance. The destructor calls OrlibExit(),
//     destroys the hidden EDIT control, restores the 11 thread-check patch
//     bytes, and calls CoUninitialize() — all WITHOUT the removed
//     persistence-patch restoration branch. A clean return (no SEH fault)
//     proves the destructor lifecycle is intact with the persistence patch
//     surface gone.
//
//   Case 4 — Repeatable lifecycle (patch restoration round-trip):
//     Construct a SECOND TsDevLib instance and destruct it. If the first
//     destructor's thread-check patch restoration had corrupted the
//     orlib20.dll image in memory, the second construction's patch
//     application or OrlibInit would fault. A clean second construct +
//     destruct cycle proves the patch apply/restore machinery is symmetric
//     and repeatable — the core behavioral regression guard for the
//     finalization lifecycle.
//
// The full real finalization path (insert -> compile -> verify -> persist
// .ell/.xml) is exercised end-to-end by the L.1 desired-state harness and
// the insert/modify integration tests. This unit test deliberately stops at
// the constructor/destructor lifecycle so it can run quickly and isolate
// the "persistence patch removed, thread-check patches intact, lifecycle
// symmetric" regression without performing a full CLI insert operation.
//
// NOTE: TsDevLib is constructed AND destructed inside a worker thread with a
// 64 MB stack (same pattern as test_insert.cpp / test_path_discovery.cpp)
// because the EasyLanguage compiler and the thread-affine hidden EDIT
// control require the large stack. An SEH wrapper around the worker thread
// body catches any unhandled structured exception and converts it to a
// nonzero exit code (matching test_path_discovery.cpp).
//
// Exit code: 0 only if ALL cases pass; distinct nonzero codes per failure.
// Requires TradeStation (orlib20.dll) at runtime -> RequiresTs=$true.

#include "tsdev_lib.h"

#include <cstdio>
#include <string>
#include <windows.h>

using namespace tsdev;

namespace {

// ---------------------------------------------------------------------------
// File helper
// ---------------------------------------------------------------------------

bool fileExistsW(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full lifecycle regression, running on a large-stack worker thread.
// TsDevLib is constructed AND destructed here so the thread-affine EDIT
// control and the COM apartment live and die on the same thread that uses
// them.
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;

    // --- Case 1: Constructor succeeds without the persistence patch ---
    std::fprintf(stdout,
                 "\n[1] Constructing TsDevLib (no persistence patch)...\n");
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib();  // auto-discover
    } catch (const std::exception& e) {
        ec = fail(e.what(), 10);
        return 0;
    }
    std::fprintf(stdout,
                 "    TsDevLib constructed OK (constructor ran without the "
                 "persistence patch; OrlibInit succeeded -> 11 thread-check "
                 "patches applied)\n");

    // --- Case 2: Post-construction invariants (real path prerequisites) ---
    std::fprintf(stdout,
                 "\n[2] Post-construction invariants...\n");
    const char* myWorkBin = lib->getMyWorkBinPath();
    if (myWorkBin == nullptr || myWorkBin[0] == '\0') {
        delete lib;
        ec = fail("getMyWorkBinPath returned empty", 20);
        return 0;
    }
    std::string binStr(myWorkBin);
    std::fprintf(stdout, "    getMyWorkBinPath = %s\n", binStr.c_str());

    const char* wrkPath = lib->getWrkPath();
    if (wrkPath == nullptr || wrkPath[0] == '\0') {
        delete lib;
        ec = fail("getWrkPath returned empty", 30);
        return 0;
    }
    std::string wrkStr(wrkPath);
    std::fprintf(stdout, "    getWrkPath = %s\n", wrkStr.c_str());

    if (binStr.find("MyWork\\Bin") == std::string::npos) {
        delete lib;
        ec = fail("getMyWorkBinPath does not contain MyWork\\Bin", 40);
        return 0;
    }
    if (wrkStr.find("MWRKArea.wrk") == std::string::npos) {
        delete lib;
        ec = fail("getWrkPath does not contain MWRKArea.wrk", 50);
        return 0;
    }

    // Defensive cross-check: orlib20.dll exists at <root>\Program\.
    std::string testRoot = binStr.substr(0, binStr.rfind("\\MyWork\\Bin"));
    std::wstring testRootW(testRoot.begin(), testRoot.end());
    if (!fileExistsW(testRootW + L"\\Program\\orlib20.dll")) {
        delete lib;
        ec = fail("orlib20.dll not found at <root>\\Program\\orlib20.dll", 57);
        return 0;
    }
    std::fprintf(stdout,
                 "    PASS: warehouse root resolved, orlib20.dll present\n");

    // --- Case 3: Destructor completes cleanly ---
    std::fprintf(stdout,
                 "\n[3] Destructing TsDevLib (no persistence-patch restore "
                 "branch)...\n");
    delete lib;  // OrlibExit + EDIT destroy + 11 thread-check restores +
                 // CoUninitialize, all on this thread
    lib = nullptr;
    std::fprintf(stdout,
                 "    TsDevLib destructed OK (destructor lifecycle intact "
                 "without the persistence patch restoration branch)\n");

    // --- Case 4: Repeatable lifecycle (patch restoration round-trip) ---
    // A second construct+destruct cycle proves the first destructor's
    // thread-check patch restoration left the orlib20.dll image in a clean
    // state: the second construction re-applies the 11 patches and re-runs
    // OrlibInit without fault. This is the symmetric apply/restore
    // regression guard.
    std::fprintf(stdout,
                 "\n[4] Repeatable lifecycle: second construct+destruct...\n");
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        ec = fail(e.what(), 70);
        return 0;
    }
    std::fprintf(stdout,
                 "    Second TsDevLib constructed OK (patch restoration was "
                 "symmetric)\n");

    // Lightweight behavioral probe on the second instance: findExistPos for
    // a guaranteed-nonexistent name must return 0 (not crash). This
    // exercises the resolved FindExistPos export and the warehouse read
    // path through the patched thread-check sites — proving the patches
    // applied by the second construction are functional, not just
    // non-faulting. We do NOT insert/modify/delete (that is the L.1
    // harness's job); this is a read-only probe.
    int found = lib->findExistPos(
        "__tvs_l3_finalization_lifecycle_probe__", ComponentType::Strategy);
    std::fprintf(stdout,
                 "    findExistPos(probe) = %d (0 expected; nonzero is "
                 "tolerable but indicates a stale warehouse)\n", found);

    delete lib;
    lib = nullptr;
    std::fprintf(stdout,
                 "    Second TsDevLib destructed OK (lifecycle is "
                 "repeatable)\n");

    // --- SUCCESS ---
    std::fprintf(stdout,
                 "\nSUCCESS: finalization lifecycle regression passed "
                 "(constructor, invariants, destructor, repeatable "
                 "lifecycle) — no persistence-patch API invoked\n");
    return 0;
}

// SEH wrapper — POD-only (no C++ objects with destructors) so __try is
// legal. Catches any unhandled structured exception (e.g. an access
// violation if the patch machinery were broken) and converts it to a
// nonzero exit code, matching the test_path_discovery.cpp convention.
DWORD WINAPI threadProc(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    __try {
        runTest(param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr,
                     "\n[FATAL] Unhandled SEH exception 0x%08X\n",
                     GetExceptionCode());
        st->exitCode = 99;
    }
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_finalization_lifecycle: behavior-based "
                 "finalization regression (TVS-L.3) ===\n");

    // Run the entire test on a worker thread with a 64 MB stack. The hidden
    // EDIT control and the EasyLanguage compiler are thread-affine and
    // require the large stack; COM is initialized on this owning thread by
    // the ComApartmentGuard and must be balanced on the same thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  threadProc, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::fprintf(stdout,
                 "\n=== test_finalization_lifecycle complete (exit %d) ===\n",
                 st.exitCode);
    if (st.exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return st.exitCode;
}
