// tests/test_invoke_excepinfo.cpp
//
// Unit test (RED-GREEN TDD) for RC-02: EXCEPINFO BSTR leak on the SEH Invoke
// path in src/whserver_names.cpp callInvokeImpl().
//
// Bug: EXCEPINFO exc was declared INSIDE __try. The normal path freed the
// three BSTRs (bstrSource/bstrDescription/bstrHelpFile); the __except path
// returned false WITHOUT freeing them, and could not (exc out of scope).
// Fix: hoist EXCEPINFO (POD) out of __try and free the three BSTRs in
// __except exactly as the normal path does.
//
// Method: a fake IDispatch whose Invoke populates all three EXCEPINFO BSTRs
// then raises SEH (RaiseException). SysAllocString/SysFreeString calls are
// counted through a tracking seam: the test #defines them to wrappers BEFORE
// #including ../src/whserver_names.cpp (same-TU pattern as test_list_cmd),
// so production call sites tally allocs/frees. Leak <=> allocs != frees.
//
// Checks:
//   1. SEH path: Invoke fills EXCEPINFO + raises -> callInvokeImpl returns
//      false AND all 3 BSTRs are freed (RED before fix, GREEN after).
//   2. Normal failure path: Invoke fills EXCEPINFO + returns E_FAIL (no SEH)
//      -> returns true, *outHr=E_FAIL, allocs == frees (pins normal frees).
//   3. Clean success path: Invoke returns S_OK, empty EXCEPINFO -> returns
//      true, *outHr=S_OK, no alloc/free traffic.
//
// This is a PURE UNIT TEST - no orlib20, no WHServer, no TsDevLib, -NoTs.
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include <windows.h>
#include <oleauto.h>

#include <cstdio>

// --- Real OleAut32 entry points (resolved before the tracking #defines) -----
static BSTR (WINAPI *g_realAlloc)(const OLECHAR*) = nullptr;
static void (WINAPI *g_realFree)(BSTR) = nullptr;

static bool resolveRealOleAut32() {
    HMODULE mod = GetModuleHandleW(L"oleaut32.dll");
    if (!mod) return false;
    g_realAlloc = reinterpret_cast<BSTR (WINAPI*)(const OLECHAR*)>(
        GetProcAddress(mod, "SysAllocString"));
    g_realFree = reinterpret_cast<void (WINAPI*)(BSTR)>(
        GetProcAddress(mod, "SysFreeString"));
    return g_realAlloc && g_realFree;
}

// --- Tracking seam -----------------------------------------------------------
static int g_allocs = 0;
static int g_frees = 0;

static BSTR TrackSysAllocString(const OLECHAR* s) {
    ++g_allocs;
    return g_realAlloc(s);
}

static void TrackSysFreeString(BSTR b) {
    if (b) ++g_frees;
    g_realFree(b);
}

static void countersReset() { g_allocs = 0; g_frees = 0; }

// Redirect production + fake call sites in this TU to the counters.
#define SysAllocString TrackSysAllocString
#define SysFreeString TrackSysFreeString

// Pull in the production TU so the anonymous-namespace callInvokeImpl is
// directly callable. whserver_names.cpp is NOT linked (no tsdev_lib link:
// duplicate symbols), and its only external log symbol is stubbed below.
#include "../src/whserver_names.cpp"

#undef SysAllocString
#undef SysFreeString

// --- Minimal tsdev_log_message stub (whserver_names.cpp's only tsdev_lib
// --- dependency; tsdev_lib.cpp itself is never linked into this test).
namespace tsdev {
void tsdev_log_message(TsDevLogLevel, const char* /*fmt*/, ...) {}
} // namespace tsdev

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

// --- Fake IDispatch ----------------------------------------------------------
// Base: minimal IDispatch with configurable Invoke behavior. POD-safe usage:
// no __try here, so virtuals/destructors are legal (C2712 only restricts
// functions that themselves contain __try).
class FakeDispatch : public IDispatch {
public:
    volatile LONG m_ref = 1;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch)) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override {
        if (pctinfo) *pctinfo = 0;
        return S_OK;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID,
                               DISPID*) override {
        return E_NOTIMPL;
    }
};

// Fills all three EXCEPINFO BSTRs (tracked allocs) then raises SEH.
class SehThrowDispatch : public FakeDispatch {
public:
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*,
                        VARIANT*, EXCEPINFO* exc, UINT*) override {
        if (exc) {
            exc->wCode = 1001;
            exc->bstrSource = TrackSysAllocString(L"FakeSource");
            exc->bstrDescription = TrackSysAllocString(L"FakeDescription");
            exc->bstrHelpFile = TrackSysAllocString(L"FakeHelp.chm");
        }
        RaiseException(0xE0004242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return E_FAIL; // unreachable; keeps the build warning-clean
    }
};

// Fills all three EXCEPINFO BSTRs (tracked allocs) then returns E_FAIL.
class FailDispatch : public FakeDispatch {
public:
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*,
                        VARIANT*, EXCEPINFO* exc, UINT*) override {
        if (exc) {
            exc->wCode = 1002;
            exc->bstrSource = TrackSysAllocString(L"FailSource");
            exc->bstrDescription = TrackSysAllocString(L"FailDescription");
            exc->bstrHelpFile = TrackSysAllocString(L"FailHelp.chm");
        }
        return E_FAIL;
    }
};

// Returns S_OK with an empty EXCEPINFO (no BSTR traffic).
class OkDispatch : public FakeDispatch {
public:
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*,
                        VARIANT*, EXCEPINFO*, UINT*) override {
        return S_OK;
    }
};

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    std::fprintf(stdout,
                 "=== test_invoke_excepinfo: EXCEPINFO SEH frees ===\n");

    if (!resolveRealOleAut32()) {
        ec = fail("cannot resolve real SysAllocString/SysFreeString", 1);
        return 0;
    }

    // ------------------------------------------------------------------
    // Check 1 (RC-02): SEH path frees all three EXCEPINFO BSTRs.
    // ------------------------------------------------------------------
    {
        SehThrowDispatch* disp = new SehThrowDispatch();
        DISPPARAMS dp = {};
        VARIANT res;
        VariantInit(&res);
        HRESULT hr = S_OK;
        countersReset();
        const bool ok =
            tsdev::callInvokeImpl(disp, 1, DISPATCH_PROPERTYGET, &dp, &res,
                                  &hr);
        const int allocs = g_allocs;
        const int frees = g_frees;
        disp->Release();
        VariantClear(&res);
        std::fprintf(stdout,
                     "  seh: ok=%d allocs=%d frees=%d\n", (int)ok, allocs,
                     frees);
        if (ok) {
            ec = fail("SEH Invoke must return false", 11);
            return 0;
        }
        if (allocs != 3) {
            ec = fail("fake must allocate exactly 3 EXCEPINFO BSTRs", 12);
            return 0;
        }
        if (frees != allocs) {
            ec = fail("SEH path leaked EXCEPINFO BSTRs (RC-02)", 10);
            return 0;
        }
        std::fprintf(stdout, "    PASS: SEH path freed all EXCEPINFO BSTRs\n");
    }

    // ------------------------------------------------------------------
    // Check 2: normal failure path still frees (pins existing behavior).
    // ------------------------------------------------------------------
    {
        FailDispatch* disp = new FailDispatch();
        DISPPARAMS dp = {};
        VARIANT res;
        VariantInit(&res);
        HRESULT hr = S_OK;
        countersReset();
        const bool ok =
            tsdev::callInvokeImpl(disp, 1, DISPATCH_PROPERTYGET, &dp, &res,
                                  &hr);
        const int allocs = g_allocs;
        const int frees = g_frees;
        disp->Release();
        VariantClear(&res);
        std::fprintf(stdout,
                     "  fail: ok=%d hr=0x%08lX allocs=%d frees=%d\n",
                     (int)ok, static_cast<unsigned long>(hr), allocs, frees);
        if (!ok || FAILED(hr)) {
            if (!ok) {
                ec = fail("failing Invoke (no SEH) must return true", 20);
                return 0;
            }
            if (hr != E_FAIL) {
                ec = fail("outHr must carry the Invoke HRESULT", 21);
                return 0;
            }
        }
        if (allocs != 3 || frees != 3) {
            ec = fail("normal failure path must free all EXCEPINFO BSTRs",
                      22);
            return 0;
        }
        std::fprintf(stdout,
                     "    PASS: normal failure path freed all BSTRs\n");
    }

    // ------------------------------------------------------------------
    // Check 3: clean success path, no BSTR traffic.
    // ------------------------------------------------------------------
    {
        OkDispatch* disp = new OkDispatch();
        DISPPARAMS dp = {};
        VARIANT res;
        VariantInit(&res);
        HRESULT hr = E_FAIL;
        countersReset();
        const bool ok =
            tsdev::callInvokeImpl(disp, 1, DISPATCH_PROPERTYGET, &dp, &res,
                                  &hr);
        const int allocs = g_allocs;
        const int frees = g_frees;
        disp->Release();
        VariantClear(&res);
        if (!ok || hr != S_OK) {
            ec = fail("clean Invoke must return true + S_OK", 30);
            return 0;
        }
        if (allocs != 0 || frees != 0) {
            ec = fail("clean path must have no EXCEPINFO BSTR traffic", 31);
            return 0;
        }
        std::fprintf(stdout, "    PASS: clean path, no BSTR traffic\n");
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper - POD-only (no C++ objects with destructors) so __try is legal.
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
    std::fprintf(stdout, "=== test_invoke_excepinfo (unit) ===\n");

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

    int exitCode = st.exitCode;
    std::fprintf(stdout, "\n=== test_invoke_excepinfo complete (exit %d) ===\n",
                 exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
