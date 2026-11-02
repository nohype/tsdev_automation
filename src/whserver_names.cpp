// whserver_names.cpp
//
// Implementation of the WHServer IWHouse COM stored-name enumeration. See
// src/whserver_names.h for the contract and tmp/enum_mechanism_report.md
// Sec. 2 for the live proof of the exact invocation shapes used here.
//
// All COM members are resolved BY NAME via IDispatch::GetIDsOfNames against
// the WHServer typeinfo -- zero hardcoded dispids, zero hardcoded RVAs. The
// only non-typelib constants are the custom factory IID + its vtable slot 8
// (the project's proven activation contract, docs/KNOWLEDGE_BASE.md Sec. 17)
// and the stable CLSIDs/IIDs.

#include "whserver_names.h"

#include "dispatch_watchdog.h"  // in-flight dispatch marker (wedged-RPC containment)

#include <cstdlib>   // _lrotr (factory license-name scramble)
#include <cstdio>
#include <stdexcept>
#include <oleauto.h>

namespace tsdev {

// --- Pure orchestration ------------------------------------------------------

bool collectTypeNames(IWhNameSource& source, ComponentType type,
                      std::vector<std::string>& outNames) {
    outNames.clear();

    // Every source call takes the EXPANDED whType (the ComponentType enum
    // value IS the expanded whType; tsdev_lib.h is the SSOT) -- the exact
    // I2 parameter the typelib members take.
    const int whType = static_cast<int>(type);

    try {
        int count = 0;
        if (!source.getTypeCount(whType, count)) return false;

        // Empty type: SUCCESS with zero names; the id walk is skipped.
        if (count <= 0) return true;

        int nextId = 0;
        if (!source.getNextId(whType, nextId)) return false;

        // Sparse-id walk (ids 1 .. NextId-1); vacant ids are normal.
        for (int id = 1; id < nextId; id++) {
            bool found = false;
            std::string name;
            if (!source.getProgramName(whType, id, found, name)) {
                outNames.clear();
                return false;
            }
            if (found) outNames.push_back(name);
        }

        // Count consistency: the WALKED names are the authoritative element
        // count. A disagreement (count > returned included) is a report, not
        // a failure (the name index can disagree with its own count field;
        // the walk is what exists/get-source cross-check against).
        if (static_cast<int>(outNames.size()) != count) {
            tsdev_log_message(
                TsDevLogLevel::Warning,
                "tsdev: list name-count drift (whType=%d): "
                "ProgramsCountByType=%d, enumerated=%u "
                "(names list is authoritative)\n",
                whType, count,
                static_cast<unsigned>(outNames.size()));
        }
        return true;
    } catch (...) {
        // A source exception (or bad_alloc) is a real failure: return false
        // with outNames EMPTY (documented state; partials discarded).
        outNames.clear();
        return false;
    }
}

// --- SEH-wrapped COM call helpers -------------------------------------------
// Two-layer pattern copied from tsdev_lib.cpp's orlib20 call helpers: the
// Impl holds only POD types and owns the __try/__except block (MSVC C2712
// forbids objects with destructors in functions with __try); the wrapper
// enters the in-flight dispatch marker and leaves it in __finally so the
// leave also runs on SEH/unwinding. Every helper returns false on SEH --
// never crashes through.
namespace {

std::string hrDetail(const char* stage, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s hr=0x%08lX", stage,
                  static_cast<unsigned long>(hr));
    return std::string(buf);
}

// POD-only Impl layer.
bool callCoGetClassObjectImpl(const CLSID* clsid, const IID* iid, void** out,
                              HRESULT* outHr) {
    *outHr = E_FAIL;
    __try {
        *outHr = CoGetClassObject(*clsid, CLSCTX_ALL, nullptr, *iid, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callCoGetClassObject(const CLSID* clsid, const IID* iid, void** out,
                          HRESULT* outHr, const char* site) {
    dispatchEnter(site, 0);
    bool ok = false;
    __try { ok = callCoGetClassObjectImpl(clsid, iid, out, outHr); }
    __finally { dispatchLeave(); }
    return ok;
}

// Raw custom-factory create: vtable slot 8 (vtbl+0x20), the
// CreateInstanceLic-style custom factory contract (KB Sec. 17).
bool callFactoryCreateImpl(IUnknown* fact, const IID* riid, BSTR name,
                           void** outObj, BSTR* outName, HRESULT* outHr) {
    *outHr = E_FAIL;
    __try {
        typedef HRESULT (__stdcall *FactoryCreateFn)(
            IUnknown* self, unsigned a0, unsigned a1, const IID* riid,
            BSTR name, void** outObj, BSTR* outName);
        void** vt = *reinterpret_cast<void***>(fact);
        FactoryCreateFn create = (FactoryCreateFn)vt[8];  // vtbl+0x20
        *outHr = create(fact, 0, 0, riid, name, outObj, outName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callFactoryCreate(IUnknown* fact, const IID* riid, BSTR name,
                       void** outObj, BSTR* outName, HRESULT* outHr,
                       const char* site) {
    dispatchEnter(site, 0);
    bool ok = false;
    __try {
        ok = callFactoryCreateImpl(fact, riid, name, outObj, outName, outHr);
    } __finally { dispatchLeave(); }
    return ok;
}

bool callQueryInterfaceImpl(IUnknown* obj, const IID* iid, void** out,
                            HRESULT* outHr) {
    *outHr = E_FAIL;
    __try {
        *outHr = obj->QueryInterface(*iid, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callQueryInterface(IUnknown* obj, const IID* iid, void** out,
                        HRESULT* outHr, const char* site) {
    dispatchEnter(site, 0);
    bool ok = false;
    __try { ok = callQueryInterfaceImpl(obj, iid, out, outHr); }
    __finally { dispatchLeave(); }
    return ok;
}

bool callGetIdsImpl(IDispatch* disp, LPOLESTR name, DISPID* outId,
                    HRESULT* outHr) {
    *outHr = E_FAIL;
    __try {
        *outHr = disp->GetIDsOfNames(IID_NULL, &name, 1,
                                     LOCALE_SYSTEM_DEFAULT, outId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callGetIds(IDispatch* disp, const wchar_t* name, DISPID* outId,
                HRESULT* outHr, const char* site) {
    dispatchEnter(site, 0);
    bool ok = false;
    LPOLESTR mutableName = const_cast<LPOLESTR>(name);
    __try { ok = callGetIdsImpl(disp, mutableName, outId, outHr); }
    __finally { dispatchLeave(); }
    return ok;
}

bool callInvokeImpl(IDispatch* disp, DISPID dispid, WORD flags,
                    DISPPARAMS* dp, VARIANT* result, HRESULT* outHr) {
    *outHr = E_FAIL;
    __try {
        EXCEPINFO exc = {};
        UINT argErr = 0;
        *outHr = disp->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, flags,
                              dp, result, &exc, &argErr);
        // EXCEPINFO BSTRs are owned by the caller even on failure.
        if (exc.bstrSource) SysFreeString(exc.bstrSource);
        if (exc.bstrDescription) SysFreeString(exc.bstrDescription);
        if (exc.bstrHelpFile) SysFreeString(exc.bstrHelpFile);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool callInvoke(IDispatch* disp, DISPID dispid, WORD flags, DISPPARAMS* dp,
                VARIANT* result, HRESULT* outHr, const char* site,
                uint32_t aux) {
    dispatchEnter(site, aux);
    bool ok = false;
    __try {
        ok = callInvokeImpl(disp, dispid, flags, dp, result, outHr);
    } __finally { dispatchLeave(); }
    return ok;
}

// ProgramsCountByType / NextId share one shape: PROPERTYGET (VT_I2 Type)
// -> I4. `dispid` is the name-resolved member id.
bool invokeTypeI4(IDispatch* disp, DISPID dispid, int whType, int& outValue,
                  const char* site) {
    if (!disp || dispid == DISPID_UNKNOWN) return false;

    VARIANT arg;
    VariantInit(&arg);
    arg.vt = VT_I2;
    arg.iVal = static_cast<SHORT>(whType);
    DISPPARAMS dp = {};
    dp.rgvarg = &arg;
    dp.cArgs = 1;

    VARIANT res;
    VariantInit(&res);
    HRESULT hr = E_FAIL;
    const bool ok = callInvoke(disp, dispid, DISPATCH_PROPERTYGET, &dp, &res,
                               &hr, site, static_cast<uint32_t>(whType));
    VariantClear(&arg);
    if (!ok || FAILED(hr)) {
        VariantClear(&res);
        return false;
    }
    if (res.vt != VT_I4) {  // typelib: -> I4; anything else is a contract break
        VariantClear(&res);
        return false;
    }
    outValue = res.lVal;
    VariantClear(&res);
    return true;
}

// IProgram::Name returns a BSTR; the stored names are the ANSI warehouse
// strings (the same bytes `exists` reports), so convert via CP_ACP.
bool narrowName(BSTR b, std::string& out) {
    out.clear();
    if (!b) return true;
    const int wlen = static_cast<int>(SysStringLen(b));
    if (wlen <= 0) return true;
    const int n = WideCharToMultiByte(CP_ACP, 0, b, wlen, nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return false;
    std::string s(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, b, wlen, &s[0], n, nullptr,
                            nullptr) <= 0) {
        return false;
    }
    out = s;
    return true;
}

} // namespace

// --- WhComNameSource (real COM provider) ------------------------------------

WhComNameSource::WhComNameSource(const std::wstring& myWorkDir)
    : m_myWorkDir(myWorkDir) {
    std::string detail;
    if (!bind(detail)) {
        release();
        throw std::runtime_error("tsdev: WHServer name source bind failed: " +
                                 detail);
    }
}

WhComNameSource::~WhComNameSource() {
    release();
}

void WhComNameSource::release() {
    // Release() return values are deliberately ignored and never reported:
    // RPC errors on an already-dead server are expected here. CoUninitialize
    // is NOT called -- the apartment belongs to TsDevLib's COM guard.
    if (m_whouse)  { m_whouse->Release();  m_whouse  = nullptr; }
    if (m_dctrl)   { m_dctrl->Release();   m_dctrl   = nullptr; }
    if (m_ctl)     { m_ctl->Release();     m_ctl     = nullptr; }
    if (m_factory) { m_factory->Release(); m_factory = nullptr; }
    if (m_outName) { SysFreeString(m_outName); m_outName = nullptr; }
    m_idCount  = DISPID_UNKNOWN;
    m_idNextId = DISPID_UNKNOWN;
    m_idById   = DISPID_UNKNOWN;
}

bool WhComNameSource::bind(std::string& detail) {
    // Hard precondition (same as ComRecycleAttach): NEVER invoke WHouseEx
    // with Create=1 against an unresolved or guessed MyWork path.
    if (m_myWorkDir.empty()) {
        detail = "mywork_unresolved";
        return false;
    }

    // The TsDevLib COM apartment guard already initialized this thread's
    // STA; the call here only confirms it (S_OK / S_FALSE expected).
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr) && coHr != S_FALSE) {
        detail = hrDetail("CoInitializeEx", coHr);
        return false;
    }

    CLSID clsidCtl;
    IID   iidFactory;
    IID   iidCtl;
    if (FAILED(CLSIDFromString(L"{7CEEA90C-D59E-11D3-8394-00C04F7BDF10}",
                               &clsidCtl)) ||
        FAILED(CLSIDFromString(L"{7CEEACBC-D59E-11D3-8394-00C04F7BDF10}",
                               &iidFactory)) ||
        FAILED(CLSIDFromString(L"{7CEEA90A-D59E-11D3-8394-00C04F7BDF10}",
                               &iidCtl))) {
        detail = "CLSIDFromString";
        return false;
    }

    HRESULT hr = E_FAIL;
    if (!callCoGetClassObject(&clsidCtl, &iidFactory,
                              reinterpret_cast<void**>(&m_factory), &hr,
                              "WhNames.GetClassObject") ||
        FAILED(hr) || !m_factory) {
        detail = hrDetail("CoGetClassObject", hr);
        return false;
    }

    // License-style factory name: "<268435478>,<scrambled tick>" (KB Sec. 17).
    const unsigned int tick = GetTickCount();
    const unsigned int scrambled =
        _lrotr(((tick >> 12) & 0xFFFF | 0x160000u) ^ 0x1256abefu, 6);
    wchar_t nameBuf[64];
    _snwprintf_s(nameBuf, 64, _TRUNCATE, L"%u,%u", 268435478u, scrambled);
    BSTR nm = SysAllocString(nameBuf);
    void* outObj = nullptr;
    const bool created = callFactoryCreate(m_factory, &iidCtl, nm, &outObj,
                                           &m_outName, &hr,
                                           "WhNames.FactoryCreate");
    if (nm) SysFreeString(nm);
    if (!created || FAILED(hr) || !outObj) {
        detail = hrDetail("factory_create", hr);
        return false;
    }
    m_ctl = reinterpret_cast<IUnknown*>(outObj);

    if (!callQueryInterface(m_ctl, &IID_IDispatch,
                            reinterpret_cast<void**>(&m_dctrl), &hr,
                            "WhNames.QueryInterface") ||
        FAILED(hr) || !m_dctrl) {
        detail = hrDetail("QI_IDispatch", hr);
        return false;
    }

    DISPID whouseExId = DISPID_UNKNOWN;
    if (!callGetIds(m_dctrl, L"WHouseEx", &whouseExId, &hr,
                    "WhNames.GetIdsOfNames") ||
        FAILED(hr)) {
        detail = hrDetail("GetIDsOfNames(WHouseEx)", hr);
        return false;
    }

    // rgvarg REVERSED: [0]=Create(I4=1), [1]=DirName(BSTR MyWork).
    VARIANT vargs[2];
    VariantInit(&vargs[0]);
    vargs[0].vt = VT_I4;
    vargs[0].lVal = 1;
    VariantInit(&vargs[1]);
    vargs[1].vt = VT_BSTR;
    vargs[1].bstrVal = SysAllocString(m_myWorkDir.c_str());
    DISPPARAMS dp = {};
    dp.rgvarg = vargs;
    dp.cArgs = 2;

    VARIANT vres;
    VariantInit(&vres);
    const bool invoked = callInvoke(m_dctrl, whouseExId, DISPATCH_PROPERTYGET,
                                    &dp, &vres, &hr, "WhNames.WHouseEx", 0);
    VariantClear(&vargs[1]);
    if (!invoked || FAILED(hr) || vres.vt != VT_DISPATCH || !vres.pdispVal) {
        detail = hrDetail("Invoke(WHouseEx)", FAILED(hr) ? hr : E_FAIL);
        VariantClear(&vres);
        return false;
    }
    // Transfer ownership OUT of the VARIANT (see ComRecycleAttach): m_whouse
    // holds the single owned ref; release() must not double-release it.
    m_whouse = vres.pdispVal;
    vres.vt = VT_EMPTY;
    vres.pdispVal = nullptr;

    // Resolve the three IWHouse member dispids BY NAME (once, at bind).
    if (!callGetIds(m_whouse, L"ProgramsCountByType", &m_idCount, &hr,
                    "WhNames.GetIdsOfNames") ||
        FAILED(hr)) {
        detail = hrDetail("GetIDsOfNames(ProgramsCountByType)", hr);
        return false;
    }
    if (!callGetIds(m_whouse, L"NextId", &m_idNextId, &hr,
                    "WhNames.GetIdsOfNames") ||
        FAILED(hr)) {
        detail = hrDetail("GetIDsOfNames(NextId)", hr);
        return false;
    }
    if (!callGetIds(m_whouse, L"ProgramById", &m_idById, &hr,
                    "WhNames.GetIdsOfNames") ||
        FAILED(hr)) {
        detail = hrDetail("GetIDsOfNames(ProgramById)", hr);
        return false;
    }

    detail.clear();
    return true;
}

bool WhComNameSource::getTypeCount(int whType, int& outCount) {
    return invokeTypeI4(m_whouse, m_idCount, whType, outCount,
                        "WhNames.Count");
}

bool WhComNameSource::getNextId(int whType, int& outNextId) {
    return invokeTypeI4(m_whouse, m_idNextId, whType, outNextId,
                        "WhNames.NextId");
}

bool WhComNameSource::getProgramName(int whType, int id, bool& outFound,
                                     std::string& outName) {
    outFound = false;
    outName.clear();
    if (!m_whouse || m_idById == DISPID_UNKNOWN) return false;

    // ProgramById (DISPATCH_PROPERTYGET, params (Type, Id) -> DISPATCH):
    // rgvarg REVERSED: [0]=Id (VT_I4), [1]=Type (VT_I2).
    VARIANT args[2];
    VariantInit(&args[0]);
    args[0].vt = VT_I4;
    args[0].lVal = id;
    VariantInit(&args[1]);
    args[1].vt = VT_I2;
    args[1].iVal = static_cast<SHORT>(whType);
    DISPPARAMS dp = {};
    dp.rgvarg = args;
    dp.cArgs = 2;

    VARIANT res;
    VariantInit(&res);
    HRESULT hr = E_FAIL;
    const bool ok = callInvoke(m_whouse, m_idById, DISPATCH_PROPERTYGET, &dp,
                               &res, &hr, "WhNames.ProgramById",
                               static_cast<uint32_t>(id));
    VariantClear(&args[0]);
    VariantClear(&args[1]);
    if (!ok || FAILED(hr)) {
        VariantClear(&res);
        return false;
    }

    // Vacant id (null/empty ProgramById result) is NORMAL in the sparse id
    // space: success with outFound=false.
    if (res.vt == VT_EMPTY || res.vt == VT_NULL) {
        VariantClear(&res);
        return true;
    }
    if (res.vt != VT_DISPATCH) {
        VariantClear(&res);
        return false;  // unexpected result type: contract break
    }
    if (res.pdispVal == nullptr) {
        VariantClear(&res);
        return true;
    }

    // IProgram::Name (DISPATCH_PROPERTYGET, no args) -> BSTR. The program
    // proxy is owned via `res` (VariantClear releases it); it stays on this
    // creating STA (never marshal a QI across apartments).
    IDispatch* prog = res.pdispVal;
    DISPID nameId = DISPID_UNKNOWN;
    if (!callGetIds(prog, L"Name", &nameId, &hr, "WhNames.GetIdsOfNames") ||
        FAILED(hr)) {
        VariantClear(&res);
        return false;
    }

    VARIANT nameRes;
    VariantInit(&nameRes);
    DISPPARAMS dp0 = {};
    const bool gotName = callInvoke(prog, nameId, DISPATCH_PROPERTYGET, &dp0,
                                    &nameRes, &hr, "WhNames.Name",
                                    static_cast<uint32_t>(id));
    if (!gotName || FAILED(hr) || nameRes.vt != VT_BSTR) {
        VariantClear(&nameRes);
        VariantClear(&res);
        return false;
    }

    const bool narrowed = narrowName(nameRes.bstrVal, outName);
    VariantClear(&nameRes);
    VariantClear(&res);
    if (!narrowed) {
        outName.clear();
        return false;
    }
    outFound = true;
    return true;
}

} // namespace tsdev