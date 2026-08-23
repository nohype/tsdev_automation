// com_apartment_guard.cpp
//
// Implementation of the RAII STA COM apartment guard for TsDevLib (TVS-L.2).
//
// See com_apartment_guard.h for the full API contract and thread-affinity
// rules. This TU is intentionally minimal: it forwards to the injectable
// ComApi function pointers and records the HRESULT for diagnostic reporting.
//
// The default ComApi binds the real CoInitializeEx / CoUninitialize from
// ole32.dll; tests inject mock function pointers so the unit tests never
// touch the test process's real COM apartment state.

#include "com_apartment_guard.h"

ComApartmentGuard::ComApartmentGuard(ComApi api)
    : m_api(api),
      m_initialized(false),
      m_initResult(E_FAIL) {  // safe default; overwritten below
    // Enter the STA. COINIT_APARTMENTTHREADED (0x2) is the apartment model
    // TSDev.exe / orlib20.dll expect: COM objects created on this thread are
    // single-threaded and only ever accessed from this thread.
    m_initResult = m_api.initialize(nullptr, COINIT_APARTMENTTHREADED);

    // S_OK  : the apartment was freshly entered on this thread.
    // S_FALSE: the apartment was already entered on this thread (a previous
    //          CoInitializeEx succeeded). Per COM rules, each successful
    //          CoInitializeEx — including the S_FALSE one — must be balanced
    //          by exactly one CoUninitialize on the same thread.
    // Any other HRESULT (including RPC_E_CHANGED_MODE): the COM runtime was
    // NOT entered on this thread, so NO CoUninitialize must be called. The
    // HRESULT is retained for the owning object's diagnostic reporting.
    if (m_initResult == S_OK || m_initResult == S_FALSE) {
        m_initialized = true;
    } else {
        m_initialized = false;
    }
}

ComApartmentGuard::~ComApartmentGuard() {
    // Balance COM exactly once iff the apartment was actually entered. On
    // constructor-unwind (a later constructor step of the owning object
    // throws after a successful CoInitializeEx), this destructor still runs
    // during stack unwinding and balances COM exactly once on the
    // constructing thread.
    if (m_initialized) {
        m_api.uninitialize();
    }
}

bool ComApartmentGuard::initialized() const {
    return m_initialized;
}

HRESULT ComApartmentGuard::initResult() const {
    return m_initResult;
}
