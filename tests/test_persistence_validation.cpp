// test_persistence_validation.cpp
//
// TVS-B.1.5: Validate original bytes before persistence patch. Tests
// validatePersistencePatchBytes for mismatch detection, match confirmation,
// and wildcard logic.
//
// The test constructs a TsDevLib instance (which applies the persistence
// patch internally — after construction, the 8 bytes at the persistence
// patch site RVA (see signatures/v9.50.01.3344.json) are the
// REPLACEMENT bytes B8 01 00 00 00 C2 0C 00). It then calls
// validatePersistencePatchBytes three times:
//
//   Case 1 — Mismatch detection (RED):
//     expected = all zeros, mask = all check -> expect false (bytes differ).
//   Case 2 — Match confirmation (positive):
//     expected = replacement bytes, mask = all check -> expect true.
//   Case 3 — Wildcard logic:
//     expected = replacement bytes with bytes 6-7 changed to 0xAA,0xBB,
//     mask = wildcard at 6-7 -> expect true (wildcarded bytes ignored).
//
// Exit code: 0 on success, 1/2/3 on per-case failure, 10 on construction
// failure.
//
// NOTE: TsDevLib is constructed AND destructed inside a worker thread with a
// 64 MB stack (same pattern as test_insert.cpp) because the hidden EDIT
// control is thread-affine.

#include "tsdev_lib.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace tsdev;

namespace {

// Replacement bytes written by applyPersistencePatch() at the persistence
// patch site RVA (see signatures/v9.50.01.3344.json).
const unsigned char kReplacementBytes[8] = {
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00
};

// Print a failure banner and return the given code.
int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s\n", msg);
    return code;
}

// Shared state between main and the worker thread.
struct TestState {
    int exitCode = 0;
};

// The full validation flow, running on a large-stack worker thread. TsDevLib
// is constructed AND destructed here so the thread-affine EDIT control lives
// and dies on the same thread that uses it.
DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);

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

    // --- Step 2: Case 1 — Mismatch detection (RED) ---
    std::fprintf(stdout, "\n[2] Case 1: mismatch detection (expect false)...\n");
    {
        const unsigned char expected[8] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        const unsigned char mask[8] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        bool result = lib->validatePersistencePatchBytes(expected, mask, 8);
        std::fprintf(stdout, "    validatePersistencePatchBytes = %s\n",
                     result ? "true" : "false");
        if (result) {
            st->exitCode = fail(
                "Case 1: expected false (mismatch), got true", 1);
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    Case 1 OK (mismatch correctly detected)\n");
    }

    // --- Step 3: Case 2 — Match confirmation (positive) ---
    std::fprintf(stdout, "\n[3] Case 2: match confirmation (expect true)...\n");
    {
        const unsigned char expected[8] = {
            0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00
        };
        const unsigned char mask[8] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        bool result = lib->validatePersistencePatchBytes(expected, mask, 8);
        std::fprintf(stdout, "    validatePersistencePatchBytes = %s\n",
                     result ? "true" : "false");
        if (!result) {
            st->exitCode = fail(
                "Case 2: expected true (match), got false", 2);
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    Case 2 OK (match correctly confirmed)\n");
    }

    // --- Step 4: Case 3 — Wildcard logic ---
    std::fprintf(stdout, "\n[4] Case 3: wildcard logic (expect true)...\n");
    {
        const unsigned char expected[8] = {
            0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0xAA, 0xBB
        };
        const unsigned char mask[8] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00
        };
        bool result = lib->validatePersistencePatchBytes(expected, mask, 8);
        std::fprintf(stdout, "    validatePersistencePatchBytes = %s\n",
                     result ? "true" : "false");
        if (!result) {
            st->exitCode = fail(
                "Case 3: expected true (wildcard), got false", 3);
            delete lib;
            return 0;
        }
        std::fprintf(stdout, "    Case 3 OK (wildcard bytes ignored)\n");
    }

    // --- Step 5: SUCCESS ---
    std::fprintf(stdout,
                 "\nSUCCESS: all three validatePersistencePatchBytes cases "
                 "passed (mismatch, match, wildcard)\n");

    delete lib; // calls OrlibExit + destroys EDIT control (same thread)
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stdout,
                 "=== test_persistence_validation: patch byte validation ===\n");

    // Run the entire test on a worker thread with a 64 MB stack. The hidden
    // EDIT control is thread-affine so TsDevLib must live on this thread.
    TestState st;
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n",
                     GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::fprintf(stdout,
                 "\n=== test_persistence_validation complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
