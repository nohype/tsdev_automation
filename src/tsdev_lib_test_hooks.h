// tsdev_lib_test_hooks.h
//
// INTERNAL / TEST-ONLY header for TVS-L.13.
//
// THIS HEADER IS NOT PART OF THE PUBLIC USER API. It is included ONLY by
// unit tests and by src/tsdev_lib.cpp (which defines the factory + private
// ctor). End users of the TsDevLib library never include this header and
// never see the symbols declared here. The public header src/tsdev_lib.h
// forward-declares `struct TsDevLibTestConfig;` and a PRIVATE constructor +
// friend factory, but does NOT include this file, so the COM configuration
// (ComApi, CoInitializeEx / CoUninitialize function-pointer types, and
// <objbase.h>) does NOT leak into the public user API.
//
// Purpose:
//   Provide the smallest PRIVATE/test-only construction seam so a unit test
//   can drive TsDevLib initialization with injected COM HRESULTs
//   (S_OK, S_FALSE, RPC_E_CHANGED_MODE, generic failure, and a simulated
//   later-constructor exception after COM success) WITHOUT touching the test
//   process's real COM apartment state and WITHOUT loading orlib20.dll.
//
// Seam surface (deliberately minimal):
//   - struct TsDevLibTestConfig {
//         ComApi comApi;            // injected COM API (mock or real)
//         bool   throwAfterCom;     // if true, throw immediately after a
//                                   // successful COM init to simulate a
//                                   // later-constructor exception (exercises
//                                   // stack-unwind COM balancing)
//     };
//   - std::unique_ptr<TsDevLib> tsdev::makeTsDevLibForTest(
//         const wchar_t* tsPathOverride,
//         const TsDevLibTestConfig& config);
//
// GREEN status (current):
//   TsDevLibTestConfig is DEFINED here. makeTsDevLibForTest(...) is DECLARED
//   here and DEFINED in src/tsdev_lib.cpp (the private ctor
//   TsDevLib::TsDevLib(const wchar_t*, const TsDevLibTestConfig&) it calls is
//   also defined there). A test that references makeTsDevLibForTest links
//   cleanly against tsdev_lib and runs the assertions — the GREEN signal.
//
// Contract (enforced by tests/test_tsdev_com_init_seam.cpp):
//   - The private ctor uses config.comApi (NOT the real CoInitializeEx) for
//     the COM apartment guard.
//   - On COM init failure (any HRESULT other than S_OK / S_FALSE, including
//     RPC_E_CHANGED_MODE and a generic E_FAIL), the ctor throws
//     std::runtime_error naming the HRESULT and performs NO SetDllDirectoryW,
//     NO orlib20 / support-DLL load, NO patch application, NO OrlibInit, and
//     NO warehouse access. The injected CoUninitialize is NOT called (the COM
//     runtime was never entered).
//   - On COM init success (S_OK or S_FALSE) with config.throwAfterCom == true,
//     the ctor throws std::runtime_error immediately after the COM guard is
//     constructed. The guard's destructor runs during unwinding and calls the
//     injected CoUninitialize exactly once. No orlib20 / patch / warehouse
//     side effect runs.
//   - On COM init success with config.throwAfterCom == false, the ctor
//     proceeds with the normal TsDevLib constructor body (identical to the
//     public ctor) — this path requires a real TradeStation install and is
//     NOT exercised by the pure unit test.

#ifndef TSDEV_LIB_TEST_HOOKS_H
#define TSDEV_LIB_TEST_HOOKS_H

#include <memory>

#include "com_apartment_guard.h"  // ComApi (NOT included by the public tsdev_lib.h)
#include "tsdev_lib.h"            // TsDevLib

// TsDevLibTestConfig — the test-only construction configuration.
//
// Defined here (NOT in the public header) so ComApi stays out of the public
// API. The public header forward-declares this struct as an opaque type.
struct TsDevLibTestConfig {
    // Injected COM API. Tests pass mock CoInitializeEx / CoUninitialize
    // function pointers; production code would pass the real ComApi. The
    // default-constructed ComApi has nullptr function pointers and MUST NOT
    // be used without explicit initialization.
    ComApi comApi{nullptr, nullptr};

    // If true, the private ctor throws std::runtime_error immediately after a
    // SUCCESSFUL COM init (S_OK or S_FALSE), simulating a later-constructor
    // exception. This lets a pure unit test (no orlib20 / no TradeStation)
    // exercise the stack-unwind COM-balancing path: the COM guard's destructor
    // runs during unwinding and calls the injected CoUninitialize exactly
    // once. When false, the ctor proceeds with the normal body (requires a
    // real TradeStation install).
    bool throwAfterCom = false;

    // Workstream M slice M.1 — optional test hook invoked by verifyModule()
    // immediately BEFORE the _VerifyModule_New call (i.e. AFTER the
    // StartCompilingProcess call and AFTER the +0x00/+0x04 result-field
    // pre-zeroing, matching FUN_004D9480). Gives a test live read access to
    // the 0x58-byte CELCompiler object state so it can assert the TSDev-parity
    // field layout (+0x00, +0x04, +0x08, +0x0C, +0x10, +0x18, +0x28, ...).
    //
    // Signature:
    //   void onCompilerReady(void* compilerMem, int componentType, int progId)
    //     compilerMem    — pointer to the live CELCompiler object (0x58 bytes)
    //     componentType  — the ComponentType enum value being verified
    //                      (passed as static_cast<int>(type))
    //     progId         — the ProgId passed to verifyModule
    //
    // When nullptr (the default), verifyModule() does NOT invoke any hook and
    // behaves exactly as in production. The hook is invoked ONLY on the
    // success path between StartCompilingProcess and _VerifyModule_New; it is
    // NOT invoked on the OOM / SEH-ctor / SEH-StartCompiling early-return
    // paths (no compiler object exists yet on those paths).
    void (*onCompilerReady)(void* compilerMem, int componentType, int progId) = nullptr;

    // Workstream M slice M.2 — optional test hook invoked by saveSource()
    // immediately BEFORE the _SaveModule_New@20 call, capturing the flags
    // value (arg3) and password buffer (arg5) for TSDev-parity assertions.
    // See docs/RE/RE_M2_SAVEMODULE_NEW.md (UPDATE 2026-08-25, Corrections 1
    // and 2) for the confirmed flag values and password buffer contents.
    //
    // Signature:
    //   void onSaveModuleNewReady(const unsigned int* flags,
    //                             const char* passwordBuf,
    //                             int passwordBufSize,
    //                             int componentType,
    //                             int progId)
    //     flags          — pointer to the flags word passed as arg3 (may be
    //                      NULL in the current production code; the GREEN
    //                      phase will pass a non-NULL pointer to a word
    //                      computed as (getCategory(type) & 0x7) | 0x60).
    //     passwordBuf    — pointer to the 64-byte password buffer passed as
    //                      arg5 (may be NULL in the current production code;
    //                      the GREEN phase will pass a non-NULL pointer to a
    //                      zeroed-then-!p$Kn@aSus-filled buffer).
    //     passwordBufSize— size of the password buffer (64 in GREEN; 0 while
    //                      production still passes NULL).
    //     componentType  — static_cast<int>(type) being saved.
    //     progId         — the ProgId passed to saveSource.
    //
    // When nullptr (the default), saveSource() does NOT invoke any hook and
    // behaves exactly as in production. The hook is invoked ONLY on the path
    // immediately before the m_pSaveModuleNew call.
    //
    // NOTE on the password buffer: "!p$Kn@aSus" (10 bytes at 0x00567b40 in
    // TSDev.exe) is TSDev's INTERNAL DEFAULT password, NOT a user secret. It
    // is a hardcoded constant baked into TSDev.exe, used as the encryption
    // key material for items TSDev force-protects. The CLI must replicate
    // this fixed internal default string, never accept user input for it.
    void (*onSaveModuleNewReady)(const unsigned int* flags,
                                 const char* passwordBuf,
                                 int passwordBufSize,
                                 int componentType,
                                 int progId) = nullptr;

    // Workstream M slice M.3 — optional test hook invoked by verifyModule()
    // and deleteEntry() immediately BEFORE the 0x40A notification would be
    // sent (via orlib20 Ordinal_57 / FUN_101526f0) after a successful verify
    // or a successful (category-gated) delete. Captures the exact
    // (msg, wParam, lParam) tuple plus the affected componentType and progId
    // so a test can assert the TSDev-parity packing.
    //
    // See docs/RE/RE_M3_0x40A_NOTIFICATION.md (R-M.3.1, R-M.3.2):
    //   msg    = 0x040A
    //   wParam = 0
    //   lParam = (category << 16) | (ProgId & 0xFFFF)
    // The low word of lParam IS the ProgId (warehouse record index), NOT the
    // subtype. category = getCategory(type) (low 8 bits of the expanded
    // whType).
    //
    // Confirmed operation set (R-M.3.2):
    //   - verify-success (insert/modify/upsert) sends 0x40A
    //   - delete-success sends 0x40A (gated category != 4)
    //   - failed compile/verify does NOT send 0x40A
    //
    // Signature:
    //   void onNotifyReady(UINT msg, WPARAM wParam, LPARAM lParam,
    //                      int componentType, int progId)
    //     msg           — the message ID the production code is about to post
    //                    (0x040A). Passed as-is so the test can assert it
    //                    without hardcoding in the hook plumbing.
    //     wParam        — the wParam value (0).
    //     lParam        — the packed lParam value
    //                    ((category << 16) | (ProgId & 0xFFFF)).
    //     componentType — static_cast<int>(type) of the affected module.
    //     progId        — the ProgId of the affected module.
    //
    // When nullptr (the default), verifyModule() and deleteEntry() do NOT
    // invoke any hook and behave exactly as in production. The hook is
    // invoked ONLY on the post-success path (the same point at which TSDev
    // calls orlib20 Ordinal_57); it is NOT invoked on the verify-failure or
    // delete-failure paths.
    //
    // ERROR SEMANTICS (R-M.3.3): Ordinal_57 always returns 1; send failure
    // is silently ignored; no receiver = silent no-op. The hook therefore
    // does NOT model receiver delivery — it only captures the would-be send
    // arguments. Receiver-aware testing is the test's responsibility (it
    // registers via _OrlibRegisterNotificationHandle@4 / ordinal 595 in the
    // GREEN phase; the RED test asserts only the hook capture).
    void (*onNotifyReady)(UINT msg, WPARAM wParam, LPARAM lParam,
                          int componentType, int progId) = nullptr;
};

namespace tsdev {

// Test-only factory. DECLARED here and DEFINED in src/tsdev_lib.cpp as a
// thin wrapper around the private TsDevLib(const wchar_t*,
// const TsDevLibTestConfig&) constructor. Returns a std::unique_ptr<TsDevLib>
// so the caller's ownership is explicit and the public ~TsDevLib() balances
// COM + OrlibExit on the same thread.
std::unique_ptr<TsDevLib> makeTsDevLibForTest(
    const wchar_t* tsPathOverride,
    const TsDevLibTestConfig& config);

} // namespace tsdev

#endif // TSDEV_LIB_TEST_HOOKS_H
