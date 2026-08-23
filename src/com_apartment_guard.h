// com_apartment_guard.h
//
// RAII STA COM apartment guard for TsDevLib (TVS-L.2).
//
// TsDevLib must enter the STA (single-threaded apartment) BEFORE any orlib20
// load, support-DLL load, patch application, or OrlibInit. This guard calls
// CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) on construction and the
// matching CoUninitialize() on destruction, with exact HRESULT semantics:
//   - S_OK and S_FALSE are both success; each receives exactly one
//     CoUninitialize on the same thread.
//   - RPC_E_CHANGED_MODE and any other failed HRESULT are hard initialization
//     failures; NO CoUninitialize is called (the COM runtime was not entered).
//   - On constructor unwind (a later constructor step in the owning object
//     throws after a successful CoInitializeEx), the guard's destructor still
//     balances COM exactly once.
//
// Thread-affinity contract (HARD):
//   The thread that constructs a ComApartmentGuard MUST be the thread that
//   destructs it. CoInitializeEx / CoUninitialize are thread-affine: balancing
//   COM on a different thread from the one that entered the apartment is
//   undefined behavior. The guard is therefore non-movable as well as
//   non-copyable. The owning TsDevLib is itself non-movable and lives on a
//   single 64 MiB worker thread; the guard shares that lifetime.
//
// Injectable seam:
//   The guard does not call CoInitializeEx / CoUninitialize directly. It calls
//   them through a ComApi struct of two function pointers. The default
//   ComApi{&CoInitializeEx, &CoUninitialize} binds the real COM functions.
//   Tests inject mock function pointers so the unit tests never touch the test
//   process's real COM apartment state.
//
// This header includes <objbase.h> for the CoInitializeEx / CoUninitialize
// declarations and the COINIT_APARTMENTTHREADED / RPC_E_CHANGED_MODE constants.
// <windows.h> alone does NOT reliably declare CoInitializeEx (it is gated on
// the WIN32_LEAN_AND_MEAN / _WIN32_WINNT settings the project uses), so the
// explicit <objbase.h> include is required.

#ifndef TSDEV_COM_APARTMENT_GUARD_H
#define TSDEV_COM_APARTMENT_GUARD_H

#include <windows.h>
#include <objbase.h>

// Function pointer types for COM initialization, matching the WINAPI calling
// convention of the real CoInitializeEx / CoUninitialize.
using CoInitializeExFn = HRESULT(WINAPI*)(LPVOID pvReserved, DWORD dwCoInit);
using CoUninitializeFn = void(WINAPI*)();

// Injectable COM API. Defaults to the real COM functions; tests provide mock
// function pointers so the unit tests never manipulate the test process's real
// COM apartment state.
struct ComApi {
    CoInitializeExFn  initialize;
    CoUninitializeFn  uninitialize;
};

// RAII STA COM apartment guard. Non-copyable, non-movable (thread-affine).
//
// Construction:
//   Calls api.initialize(nullptr, COINIT_APARTMENTTHREADED). S_OK and
//   S_FALSE both mark the guard as initialized (each will receive exactly one
//   api.uninitialize() on destruction). Any other HRESULT (including
//   RPC_E_CHANGED_MODE) marks the guard as not initialized; no
//   api.uninitialize() is called on destruction. The HRESULT is retained and
//   exposed via initResult() for diagnostic reporting by the owning object.
//
// Destruction:
//   If and only if the guard is initialized, calls api.uninitialize() exactly
//   once. This runs during normal destruction AND during stack unwinding if a
//   later constructor step of the owning object throws, so COM is always
//   balanced exactly once on the constructing thread.
class ComApartmentGuard {
public:
    // Construct the guard and enter the STA. The default api binds the real
    // CoInitializeEx / CoUninitialize; tests pass mock function pointers.
    explicit ComApartmentGuard(ComApi api = ComApi{&CoInitializeEx,
                                                   &CoUninitialize});

    ~ComApartmentGuard();

    // Non-copyable: the guard owns a thread-affine COM apartment.
    ComApartmentGuard(const ComApartmentGuard&)            = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;

    // Non-movable: CoInitializeEx / CoUninitialize are thread-affine, and
    // moving the guard to a different stack location / object would break the
    // same-thread balance contract if the move crosses threads or the source
    // is destroyed on a different thread than the destination.
    ComApartmentGuard(ComApartmentGuard&&)                 = delete;
    ComApartmentGuard& operator=(ComApartmentGuard&&)      = delete;

    // True if CoInitializeEx returned S_OK or S_FALSE (the guard entered the
    // STA and will balance COM on destruction). False for any other HRESULT.
    bool initialized() const;

    // The HRESULT returned by CoInitializeEx. S_OK, S_FALSE, RPC_E_CHANGED_MODE,
    // or any other failure HRESULT. The owning object uses this for diagnostic
    // reporting on hard init failure.
    HRESULT initResult() const;

private:
    ComApi   m_api;
    bool     m_initialized;   // true iff CoInitializeEx returned S_OK/S_FALSE
    HRESULT  m_initResult;    // the raw HRESULT from CoInitializeEx
};

#endif  // TSDEV_COM_APARTMENT_GUARD_H
