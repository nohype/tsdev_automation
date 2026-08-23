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
