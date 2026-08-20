// test_batch_upsert.cpp
//
// Integration test for the library-level composition that the
// `batch-upsert` CLI subcommand (TVS-I.1) relies on. Batch-upsert reuses
// the existing doUpsert() core logic but with flushPerItem=false in the batch
// context: a SINGLE closeWarehouse() at the end (like the other batch ops),
// NOT one per item.
//
// This test validates the KEY batch-upsert invariant: multiple upsert items
// on a SINGLE shared TsDevLib instance, mixing insert-path and modify-path,
// with NO per-item closeWarehouse() (only one at the very end), preserve
// each item's ProgId across the insert->modify transition.
//
// TVS-I.1: this is a passing invariant test for the implemented batch-upsert
// feature. It validates the library-level composition that batch-upsert relies
// on: multi-item upsert on a single shared TsDevLib instance with
// flushPerItem=false (no per-item closeWarehouse, only one at the end), mixing
// insert-path and modify-path, with ProgId preservation across the
// insert->modify transition. The CLI-level E2E test is test_batch_upsert.ps1
// (a separate file).
//
// The test uses TWO probe strategies with unique names, each exercised
// through the full insert->modify lifecycle on the SAME TsDevLib instance
// with NO intermediate closeWarehouse (mirroring flushPerItem=false + the
// batch "flush once" pattern). The critical assertion is that each probe's
// ProgId is preserved after modifyEntry (no per-item flush causes identity
// loss).
//
// The test is self-cleaning: it inserts probe strategies, verifies the
// batch-upsert composition, then deletes the probes.
//
// Flow:
//   1. Construct ONE TsDevLib (loads orlib20.dll, patches, inits, EDIT control)
//   2. Pre-clean: for each probe, findExistPos(name, Strategy) — delete if exists
//   3. For each probe, findExistPos(name, Strategy) — ASSERT returns <= 0 (absent)
//   4. For each probe (insert-path, NO closeWarehouse between items — the
//      batch flushPerItem=false invariant): createEntry — assert ProgId > 0.
//      Record each ProgId.
//   5. For each probe: saveSource(sourceA) — assert success
//   6. For each probe: verifyModule — assert no errors
//   7. For each probe: findExistPos — ASSERT returns SAME ProgId (found after
//      insert, no flush)
//   8. For each probe (modify-path, still NO closeWarehouse): modifyEntry(
//      sourceB) — assert success
//   9. For each probe: findExistPos — ASSERT returns SAME ProgId (KEY
//      batch-upsert invariant: preserved after modify, no per-item flush)
//  10. Cleanup — deleteEntry for each probe
//  11. closeWarehouse() ONCE at the end (the batch "flush once" pattern)
//  12. Print PASS / FAIL
//
// Exit code: 0 on success, nonzero on failure.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_new) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). The reference probe (archive/elevated_test20.cpp) runs its
// ENTIRE flow — including orlib init and window creation — in a worker thread
// with a 64 MB stack. We do the same: TsDevLib is constructed AND destructed
// inside the worker thread. An SEH wrapper around the worker thread body
// catches any unhandled structured exception and converts it to a nonzero
// exit code.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Test fixture constants.
// Unique names minimize collision risk with other tests. The "zzz_" prefix
// sorts them to the end of the warehouse and signals test entries.
const char* kTestNameA = "zzz_test_batch_upsert_a";
const char* kTestNameB = "zzz_test_batch_upsert_b";

// Minimal valid EasyLanguage strategy source (initial) for probe A. In
// EasyLanguage `{ ... }` is a multi-line COMMENT block, so the actual code
// must be OUTSIDE it. The leading comment names the strategy (TSDev
// convention); the body issues a simple market buy order. This mirrors the
// proven source from test_upsert.cpp.
const char* kSourceA_A =
    "{ zzz_test_batch_upsert_a }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Modified source (source B) for probe A: changes the strategy from Buy to
// Sell. This is the "update" half of the upsert — the same modification
// pattern used in test_modify.cpp.
const char* kSourceB_A =
    "{ zzz_test_batch_upsert_a }\r\n"
    "Sell (\"EnterShort\") next bar at Market;\r\n";

// Source A (initial) for probe B.
const char* kSourceA_B =
    "{ zzz_test_batch_upsert_b }\r\n"
    "Buy (\"EnterLong\") next bar at Market;\r\n";

// Source B (modified) for probe B.
const char* kSourceB_B =
    "{ zzz_test_batch_upsert_b }\r\n"
    "Sell (\"EnterShort\") next bar at Market;\r\n";

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full test flow, running on a large-stack worker thread. TsDevLib is
// constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

    // Probe descriptors to keep the per-probe loops compact.
    const char* names[2]  = { kTestNameA, kTestNameB };
    const char* srcA[2]   = { kSourceA_A, kSourceA_B };
    const char* srcB[2]   = { kSourceB_A, kSourceB_B };
    int progIds[2]        = { 0, 0 };

    // --- Step 1: Construct TsDevLib ---
    std::fprintf(stdout, "\n[1] Constructing TsDevLib...\n");
    TsDevLib* lib = nullptr;
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        st->exitCode = fail(e.what(), 10);
        return 0;
    }
    std::fprintf(stdout, "    TsDevLib constructed OK\n");

    // --- Step 2: Pre-clean — ensure neither entry already exists ---
    std::fprintf(stdout, "\n[2] Pre-clean: findExistPos for each probe...\n");
    for (int i = 0; i < 2; i++) {
        int existing = lib->findExistPos(names[i], ComponentType::Strategy);
        std::fprintf(stdout, "    [%d] findExistPos(\"%s\") = %d\n",
                     i, names[i], existing);
        if (existing > 0) {
            std::fprintf(stdout,
                         "    [%d] Entry already exists (ProgId=%d) — deleting...\n",
                         i, existing);
            bool deleted = lib->deleteEntry(ComponentType::Strategy, names[i]);
            std::fprintf(stdout, "    [%d] deleteEntry = %s\n",
                         i, deleted ? "true" : "false");
        }
    }

    // --- Step 3: findExistPos for NONEXISTENT names — ASSERT <= 0 ---
    std::fprintf(stdout,
                 "\n[3] findExistPos for each probe — asserting absent...\n");
    for (int i = 0; i < 2; i++) {
        int notFound = lib->findExistPos(names[i], ComponentType::Strategy);
        std::fprintf(stdout, "    [%d] findExistPos(\"%s\") = %d\n",
                     i, names[i], notFound);
        if (notFound > 0) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] findExistPos returned positive for nonexistent item", i);
            st->exitCode = fail(buf, 20);
            lib->deleteEntry(ComponentType::Strategy, names[i]);
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    [%d] PASS: nonexistent item not found\n", i);
    }

    // --- Step 4: createEntry for each probe (insert-path, NO closeWarehouse) ---
    std::fprintf(stdout,
                 "\n[4] createEntry for each probe (no closeWarehouse between items)...\n");
    for (int i = 0; i < 2; i++) {
        int errCode = 0;
        int progId = lib->createEntry(ComponentType::Strategy, names[i], errCode);
        std::fprintf(stdout, "    [%d] createEntry(\"%s\") = %d, errCode = 0x%X\n",
                     i, names[i], progId, errCode);
        if (progId <= 0) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] createEntry returned ProgId <= 0", i);
            st->exitCode = fail(buf, 30);
            // Clean up anything already created.
            for (int j = 0; j <= i; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
        progIds[i] = progId;
    }

    // --- Step 5: saveSource for each probe ---
    std::fprintf(stdout, "\n[5] saveSource(sourceA) for each probe...\n");
    for (int i = 0; i < 2; i++) {
        bool saved = lib->saveSource(ComponentType::Strategy, progIds[i], srcA[i]);
        std::fprintf(stdout, "    [%d] saveSource(ProgId=%d) = %s\n",
                     i, progIds[i], saved ? "true" : "false");
        if (!saved) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] saveSource returned false", i);
            st->exitCode = fail(buf, 40);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
    }

    // --- Step 6: verifyModule for each probe (compile + persist) ---
    std::fprintf(stdout, "\n[6] verifyModule for each probe...\n");
    for (int i = 0; i < 2; i++) {
        std::string errors =
            lib->verifyModule(ComponentType::Strategy, progIds[i]);
        if (errors.empty()) {
            std::fprintf(stdout, "    [%d] verifyModule(ProgId=%d): no errors\n",
                         i, progIds[i]);
        } else {
            std::fprintf(stderr, "    [%d] verifyModule errors:\n%s\n",
                         i, errors.c_str());
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] verifyModule returned compilation errors", i);
            st->exitCode = fail(buf, 50);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
    }

    // --- Step 7: findExistPos for EXISTING names — ASSERT same ProgId ---
    std::fprintf(stdout,
                 "\n[7] findExistPos for each probe — asserting exists with same ProgId...\n");
    for (int i = 0; i < 2; i++) {
        int found = lib->findExistPos(names[i], ComponentType::Strategy);
        std::fprintf(stdout, "    [%d] findExistPos(\"%s\") = %d (expected %d)\n",
                     i, names[i], found, progIds[i]);
        if (found <= 0) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] findExistPos returned <= 0 for existing item", i);
            st->exitCode = fail(buf, 60);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
        if (found != progIds[i]) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] ProgId mismatch after insert: %d != %d",
                          i, found, progIds[i]);
            st->exitCode = fail(buf, 65);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    [%d] PASS: existing item found (ProgId=%d)\n",
                     i, found);
    }

    // --- Step 8: modifyEntry for each probe (modify-path, NO closeWarehouse) ---
    std::fprintf(stdout,
                 "\n[8] modifyEntry(sourceB) for each probe (no closeWarehouse)...\n");
    for (int i = 0; i < 2; i++) {
        Result modRes = lib->modifyEntry(ComponentType::Strategy, names[i],
                                         srcB[i]);
        std::fprintf(stdout, "    [%d] modifyEntry(\"%s\"): success=%d message=\"%s\"\n",
                     i, names[i], (int)modRes.success, modRes.message.c_str());
        if (!modRes.success) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] modifyEntry returned failure on valid source B", i);
            st->exitCode = fail(buf, 70);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
    }

    // --- Step 9: findExistPos again — ASSERT SAME ProgId (KEY invariant) ---
    std::fprintf(stdout,
                 "\n[9] findExistPos for each probe — asserting SAME ProgId after modify...\n");
    for (int i = 0; i < 2; i++) {
        int after = lib->findExistPos(names[i], ComponentType::Strategy);
        std::fprintf(stdout, "    [%d] findExistPos(\"%s\") = %d (was %d)\n",
                     i, names[i], after, progIds[i]);
        if (after <= 0) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] findExistPos returned <= 0 after modify", i);
            st->exitCode = fail(buf, 80);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
        if (after != progIds[i]) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] ProgId changed after modify: %d -> %d (must NOT change)",
                          i, progIds[i], after);
            st->exitCode = fail(buf, 85);
            for (int j = 0; j < 2; j++) {
                lib->deleteEntry(ComponentType::Strategy, names[j]);
            }
            delete lib;
            return 0;
        }
        std::fprintf(stdout,
                     "    [%d] PASS: ProgId preserved after modify (%d)\n",
                     i, after);
    }

    // --- Step 10: Cleanup — delete each probe entry ---
    std::fprintf(stdout, "\n[10] Cleanup: deleteEntry for each probe...\n");
    for (int i = 0; i < 2; i++) {
        bool deleted = lib->deleteEntry(ComponentType::Strategy, names[i]);
        std::fprintf(stdout, "    [%d] deleteEntry(\"%s\") = %s\n",
                     i, names[i], deleted ? "true" : "false");
        if (!deleted) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "[%d] cleanup deleteEntry failed", i);
            st->exitCode = fail(buf, 90);
        }
    }

    // --- Step 11: closeWarehouse ONCE at the end (batch "flush once") ---
    std::fprintf(stdout, "\n[11] closeWarehouse() (single flush at end)...\n");
    lib->closeWarehouse();
    std::fprintf(stdout, "    closeWarehouse done\n");

    // --- Step 12: Result ---
    if (st->exitCode == 0) {
        std::fprintf(stdout,
                     "\nPASS: batch-upsert composition verified — "
                     "multiple items on shared TsDevLib, "
                     "insert + modify preserve ProgId, single closeWarehouse\n");
    }

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

// ---------------------------------------------------------------------------
// SEH wrapper — POD-only (no C++ objects with destructors) so __try is legal.
// ---------------------------------------------------------------------------
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
                 "=== test_batch_upsert: batch-upsert composition (TVS-I.1) ===\n");
    std::fprintf(stdout, "Strategy names: %s, %s\n", kTestNameA, kTestNameB);

    // Run the entire test on a worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread.
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

    std::fprintf(stdout, "\n=== test_batch_upsert complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
