// vss_snapshot.cpp
//
// Crash-consistent VSS snapshot for copying files held open deny-read.
// See vss_snapshot.h for the lifecycle contract.

#include "vss_snapshot.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <wbemidl.h>

// The modern Windows SDK no longer ships vscoordintfp.h, so the VSS
// requestor coordinator class id is spelled out explicitly (the value is
// the stable, documented one — 0xE579AB5F65CC4234 form).
// {E579AB5F-65CC-4234-81F8-1EB93482-1D5F}
static const CLSID CLSID_VSSCoordinator = {
    0xE579AB5F, 0x65CC, 0x4234,
    {0x81, 0xF8, 0x1E, 0xB9, 0x34, 0x82, 0x1D, 0x5F}};

namespace tsdev {
namespace {

// Bounded wait for one IVssAsync step. Returns the step's completion HRESULT
// (VSS_S_ASYNC_FINISHED on success) or VSS_E_UNEXPECTED-style failure text
// via *err.
HRESULT waitAsync(IVssAsync* async, DWORD timeoutMs, std::string* err) {
    HRESULT hr = async->Wait(timeoutMs);
    if (FAILED(hr)) {
        *err = "IVssAsync::Wait failed";
        return hr;
    }
    if (hr == VSS_S_ASYNC_CANCELLED) {
        *err = "VSS async operation cancelled";
        return hr;
    }
    HRESULT completed = 0;
    hr = async->QueryStatus(&completed, nullptr);
    if (FAILED(hr)) {
        *err = "IVssAsync::QueryStatus failed";
        return hr;
    }
    if (completed != VSS_S_ASYNC_FINISHED) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "VSS async step did not finish within %ums "
                      "(status=0x%08lX)",
                      static_cast<unsigned>(timeoutMs),
                      static_cast<unsigned long>(completed));
        *err = buf;
        return completed;
    }
    return S_OK;
}

void setErr(std::string* err, const char* step, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", step,
                  static_cast<unsigned long>(hr));
    *err = buf;
}

// Best-effort: enable a privilege already present on the process token so a
// WMI provider impersonating this client sees it ENABLED (elevated tokens
// hold SeBackupPrivilege but disabled — observed Win32_ShadowCopy::Create
// ReturnValue=5 until -EnableAllPrivileges was used).
void enableTokenPrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES,
                          &tok))
        return;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(tok);
}

// Base64 over the UTF-16LE bytes of `s` — the -EncodedCommand format.
std::string base64EncodeUtf16(const std::wstring& s) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* d =
        reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size() * 2;
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        const unsigned a = d[i];
        const unsigned b = (i + 1 < n) ? d[i + 1] : 0;
        const unsigned c = (i + 2 < n) ? d[i + 2] : 0;
        out += T[a >> 2];
        out += T[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < n) ? T[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < n) ? T[c & 63] : '=';
    }
    return out;
}

// Run `script` in a hidden 64-bit powershell.exe (reached through the
// WOW64-only Sysnative redirect) and capture stdout into *out. Returns true
// when the process ran to completion within timeoutMs.
bool runPs64(const std::wstring& script, DWORD timeoutMs, std::string* out,
             std::string* err) {
    out->clear();
    wchar_t win[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        *err = "GetWindowsDirectoryW failed";
        return false;
    }
    const std::wstring exe = std::wstring(win) +
        L"\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *err = "64-bit powershell.exe not reachable via Sysnative";
        return false;
    }
    const std::string b64 = base64EncodeUtf16(script);
    std::wstring cmd = L"\"" + exe +
        L"\" -NoProfile -NonInteractive -EncodedCommand " +
        std::wstring(b64.begin(), b64.end());

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        *err = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Child stderr goes to NUL, NOT the parsed stdout pipe — diagnostics and
    // progress records must never corrupt the RV=/SID=/DEV= output.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = (nul != INVALID_HANDLE_VALUE)
                       ? nul
                       : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(L'\0');
    const BOOL launched = CreateProcessW(
        exe.c_str(), cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!launched) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateProcess(powershell x64) "
                      "failed (err=%lu)",
                      static_cast<unsigned long>(GetLastError()));
        *err = buf;
        CloseHandle(rd);
        return false;
    }
    // Drain the stdout pipe WHILE waiting — a child that fills the pipe
    // buffer would deadlock against a single post-exit Wait+Read.
    const ULONGLONG deadline =
        GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    bool ok = true;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) &&
               avail > 0) {
            DWORD got = 0;
            const DWORD want =
                avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
            if (!ReadFile(rd, buf, want, &got, nullptr) || got == 0) break;
            out->append(buf, got);
        }
        const DWORD w = WaitForSingleObject(pi.hProcess, 100);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_TIMEOUT) {
            ok = false;
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            *err = "powershell bridge timed out";
            ok = false;
            break;
        }
    }
    // Final drain after exit/terminate — the process handle is closed, so
    // ReadFile reaches EOF once the child-side writer is gone.
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        out->append(buf, got);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok;
}

} // namespace

bool VssSnapshot::create(const std::wstring& pathOnVolume, std::string* err) {
    const DWORD kWaitMs = 120 * 1000; // bounded wait per MSDN async pattern
    err->clear();

    // MTA apartment for the VSS coordinator. S_FALSE means COM was already
    // initialized by the caller — leave that apartment alone (do NOT
    // CoUninitialize what we did not init).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInitialized = (hr == S_OK);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        setErr(err, "CoInitializeEx", hr);
        return false;
    }

    hr = CoCreateInstance(CLSID_VSSCoordinator, nullptr,
                          CLSCTX_LOCAL_SERVER, IID_IVssBackupComponents,
                          reinterpret_cast<void**>(&m_comp));
    if (hr == REGDB_E_CLASSNOTREG) {
        // vssapi's requestor coordinator is registered 64-bit-only on x64
        // Windows, so a Win32 build cannot create it in-proc. Fall back to
        // WMI Win32_ShadowCopy, which is serviced by a 64-bit WmiPrvSE
        // provider host — client process bitness is irrelevant.
        return createViaWmi(pathOnVolume, err);
    }
    if (FAILED(hr)) {
        setErr(err, "CoCreateInstance(VSSCoordinator)", hr);
        release();
        return false;
    }
    hr = m_comp->InitializeForBackup();
    if (FAILED(hr)) {
        setErr(err, "InitializeForBackup", hr);
        release();
        return false;
    }
    // VSS_CTX_APP_ROLLBACK: the lightest context that exposes a read-only
    // snapshot device for direct file access — MSDN documents it for
    // file-level copies; writer participation is not required.
    hr = m_comp->SetContext(VSS_CTX_APP_ROLLBACK);
    if (FAILED(hr)) {
        setErr(err, "SetContext(VSS_CTX_APP_ROLLBACK)", hr);
        release();
        return false;
    }
    VSS_ID setId{};
    hr = m_comp->StartSnapshotSet(&setId);
    if (FAILED(hr)) {
        setErr(err, "StartSnapshotSet", hr);
        release();
        return false;
    }
    m_setId = setId;

    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(pathOnVolume.c_str(), vol, MAX_PATH)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "GetVolumePathNameW failed (err=%lu)",
                      static_cast<unsigned long>(GetLastError()));
        *err = buf;
        release();
        return false;
    }
    VSS_ID snapId{};
    hr = m_comp->AddToSnapshotSet(vol, GUID_NULL, &snapId);
    if (FAILED(hr)) {
        setErr(err, "AddToSnapshotSet", hr);
        release();
        return false;
    }
    m_snapId = snapId;

    // PrepareForBackup is the MSDN-canonical next step even with no writers
    // gathered; it is a fast no-op in that configuration.
    IVssAsync* async = nullptr;
    hr = m_comp->PrepareForBackup(&async);
    if (FAILED(hr)) {
        setErr(err, "PrepareForBackup", hr);
        release();
        return false;
    }
    hr = waitAsync(async, kWaitMs, err);
    async->Release();
    if (FAILED(hr) || !err->empty()) {
        if (err->empty()) setErr(err, "PrepareForBackup wait", hr);
        release();
        return false;
    }

    async = nullptr;
    hr = m_comp->DoSnapshotSet(&async);
    if (FAILED(hr)) {
        setErr(err, "DoSnapshotSet", hr);
        release();
        return false;
    }
    hr = waitAsync(async, kWaitMs, err);
    async->Release();
    if (FAILED(hr) || !err->empty()) {
        if (err->empty()) setErr(err, "DoSnapshotSet wait", hr);
        release();
        return false;
    }

    VSS_SNAPSHOT_PROP props{};
    hr = m_comp->GetSnapshotProperties(snapId, &props);
    if (FAILED(hr)) {
        setErr(err, "GetSnapshotProperties", hr);
        release();
        return false;
    }
    if (props.m_pwszSnapshotDeviceObject) {
        m_device = props.m_pwszSnapshotDeviceObject;
    }
    VssFreeSnapshotProperties(&props);
    if (m_device.empty()) {
        *err = "snapshot device object is empty";
        release();
        return false;
    }
    m_haveSnapshot = true;
    return true;
}

// WMI fallback: Win32_ShadowCopy.Create on ROOT\CIMV2, synchronous. Two
// stages: a direct COM WMI call from this process, then — because the
// shadow-copy method provider refuses WOW64 clients on x64 (observed
// 0x80041014 WBEM_E_PROVIDER_NOT_FOUND / "Initialization failure" from
// 32-bit COM and 32-bit PowerShell alike) — the identical call through a
// 64-bit powershell.exe child via Sysnative. On success m_shadowId /
// m_device are set (plus m_wbem for the direct stage) and release()
// deletes the instance through the matching mechanism.
bool VssSnapshot::createViaWmi(const std::wstring& pathOnVolume,
                               std::string* err) {
    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(pathOnVolume.c_str(), vol, MAX_PATH)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "GetVolumePathNameW failed (err=%lu)",
                      static_cast<unsigned long>(GetLastError()));
        *err = buf;
        release();
        return false;
    }
    // The VSS method provider impersonates our token; backup privileges
    // must be ENABLED, not merely present.
    enableTokenPrivilege(L"SeBackupPrivilege");
    enableTokenPrivilege(L"SeRestorePrivilege");

    std::string directErr;
    if (createViaWmiDirect(vol, &directErr)) return true;
    std::string bridgeErr;
    if (createViaWmiPs64(vol, &bridgeErr)) return true;
    *err = "WMI direct: " + directErr + "; powershell x64 bridge: " +
           bridgeErr;
    release();
    return false;
}

// Stage 1 — direct COM WMI call from this process.
bool VssSnapshot::createViaWmiDirect(const std::wstring& vol,
                                     std::string* err) {
    // Process-wide COM security: impersonate-level callers may pass an
    // authentication service. RPC_E_TOO_LATE means another component
    // already configured it — that configuration is acceptable.
    HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                      RPC_C_AUTHN_LEVEL_DEFAULT,
                                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                                      EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        setErr(err, "CoInitializeSecurity", hr);
        release();
        return false;
    }

    IWbemLocator* loc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, reinterpret_cast<void**>(&loc));
    if (FAILED(hr)) {
        setErr(err, "CoCreateInstance(WbemLocator)", hr);
        release();
        return false;
    }
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr,
                            nullptr, &m_wbem);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) {
        setErr(err, "IWbemLocator::ConnectServer(ROOT\\CIMV2)", hr);
        release();
        return false;
    }
    hr = CoSetProxyBlanket(m_wbem, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        setErr(err, "CoSetProxyBlanket(IWbemServices)", hr);
        release();
        return false;
    }

    // Win32_ShadowCopy class -> Create method -> input parameter instance.
    IWbemClassObject* cls = nullptr;
    BSTR clsName = SysAllocString(L"Win32_ShadowCopy");
    hr = m_wbem->GetObject(clsName, 0, nullptr, &cls, nullptr);
    SysFreeString(clsName);
    if (FAILED(hr)) {
        setErr(err, "GetObject(Win32_ShadowCopy)", hr);
        release();
        return false;
    }
    IWbemClassObject* inParamsCls = nullptr;
    hr = cls->GetMethod(L"Create", 0, &inParamsCls, nullptr);
    cls->Release();
    if (FAILED(hr) || !inParamsCls) {
        setErr(err, "GetMethod(Win32_ShadowCopy::Create)", hr);
        release();
        return false;
    }
    IWbemClassObject* inParams = nullptr;
    hr = inParamsCls->SpawnInstance(0, &inParams);
    inParamsCls->Release();
    if (FAILED(hr) || !inParams) {
        setErr(err, "SpawnInstance(Create in-params)", hr);
        release();
        return false;
    }
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(vol.c_str()); // e.g. L"C:\\" (trailing \)
    hr = inParams->Put(L"Volume", 0, &v, 0);
    VariantClear(&v);
    if (FAILED(hr)) {
        setErr(err, "Put(Volume)", hr);
        inParams->Release();
        release();
        return false;
    }
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"ClientAccessible");
    hr = inParams->Put(L"Context", 0, &v, 0);
    VariantClear(&v);
    if (FAILED(hr)) {
        setErr(err, "Put(Context)", hr);
        inParams->Release();
        release();
        return false;
    }
    IWbemClassObject* outParams = nullptr;
    BSTR objPath = SysAllocString(L"Win32_ShadowCopy");
    BSTR mthName = SysAllocString(L"Create");
    hr = m_wbem->ExecMethod(objPath, mthName, 0, nullptr, inParams,
                            &outParams, nullptr);
    SysFreeString(objPath);
    SysFreeString(mthName);
    inParams->Release();
    if (FAILED(hr) || !outParams) {
        setErr(err, "ExecMethod(Win32_ShadowCopy::Create)", hr);
        release();
        return false;
    }
    VARIANT rv;
    VariantInit(&rv);
    hr = outParams->Get(L"ReturnValue", 0, &rv, nullptr, nullptr);
    if (FAILED(hr)) {
        setErr(err, "Win32_ShadowCopy::Create out ReturnValue", hr);
        outParams->Release();
        release();
        return false;
    }
    const ULONG retVal =
        (rv.vt == VT_UI4) ? rv.ulVal
                          : (rv.vt == VT_I4 ? static_cast<ULONG>(rv.lVal)
                                            : ~0UL);
    VariantClear(&rv);
    if (retVal != 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "Win32_ShadowCopy::Create returned %lu",
                      static_cast<unsigned long>(retVal));
        *err = buf;
        outParams->Release();
        release();
        return false;
    }
    VARIANT sid;
    VariantInit(&sid);
    hr = outParams->Get(L"ShadowID", 0, &sid, nullptr, nullptr);
    outParams->Release();
    if (FAILED(hr) || sid.vt != VT_BSTR || !sid.bstrVal) {
        setErr(err, "Win32_ShadowCopy::Create out ShadowID", hr);
        release();
        return false;
    }
    // Set now — even if the DeviceObject query below fails, release() must
    // delete this shadow so it is not orphaned.
    m_shadowId = sid.bstrVal; // "{XXXXXXXX-...}" including braces
    VariantClear(&sid);

    // Resolve the shadow's device object for direct file access.
    const std::wstring q =
        L"SELECT DeviceObject FROM Win32_ShadowCopy WHERE ID='" +
        m_shadowId + L"'";
    BSTR lang = SysAllocString(L"WQL");
    BSTR qstr = SysAllocString(q.c_str());
    IEnumWbemClassObject* en = nullptr;
    hr = m_wbem->ExecQuery(lang, qstr,
                           WBEM_FLAG_FORWARD_ONLY |
                               WBEM_FLAG_RETURN_IMMEDIATELY,
                           nullptr, &en);
    SysFreeString(lang);
    SysFreeString(qstr);
    if (FAILED(hr) || !en) {
        setErr(err, "ExecQuery(Win32_ShadowCopy)", hr);
        release();
        return false;
    }
    IWbemClassObject* row = nullptr;
    ULONG returned = 0;
    hr = en->Next(WBEM_INFINITE, 1, &row, &returned);
    en->Release();
    if (FAILED(hr) || returned == 0 || !row) {
        *err = "Win32_ShadowCopy query returned no row for the new ID";
        release();
        return false;
    }
    VARIANT dev;
    VariantInit(&dev);
    hr = row->Get(L"DeviceObject", 0, &dev, nullptr, nullptr);
    row->Release();
    if (FAILED(hr) || dev.vt != VT_BSTR || !dev.bstrVal) {
        setErr(err, "Win32_ShadowCopy.DeviceObject", hr);
        VariantClear(&dev);
        release();
        return false;
    }
    m_device = dev.bstrVal;
    VariantClear(&dev);
    if (m_device.empty()) {
        *err = "snapshot device object is empty";
        release();
        return false;
    }
    m_haveSnapshot = true;
    return true;
}

// Stage 2 — the identical Win32_ShadowCopy.Create run in a 64-bit
// powershell.exe child. Sysnative resolves only from a WOW64 process on
// x64 Windows, which is the only configuration that reaches this code.
// The script prints RV=/SID=/DEV= lines parsed below.
bool VssSnapshot::createViaWmiPs64(const std::wstring& vol,
                                   std::string* err) {
    const std::wstring script =
        L"$r = (Get-WmiObject -List Win32_ShadowCopy -EnableAllPrivileges)"
        L".Create('" + vol + L"', 'ClientAccessible')\r\n"
        L"\"RV=$($r.ReturnValue)\"\r\n"
        L"\"SID=$($r.ShadowID)\"\r\n"
        L"if ($r.ReturnValue -eq 0) { \"DEV=\" + (Get-WmiObject "
        L"Win32_ShadowCopy -Filter \"ID='$($r.ShadowID)'\").DeviceObject }\r\n";
    std::string out;
    std::string perr;
    if (!runPs64(script, 120 * 1000, &out, &perr)) {
        *err = perr;
        release();
        return false;
    }
    const auto tag = [&out](const char* t) -> std::string {
        const size_t p = out.find(t);
        if (p == std::string::npos) return std::string();
        const size_t s = p + std::strlen(t);
        const size_t e = out.find_first_of("\r\n", s);
        return out.substr(s, e == std::string::npos
                                 ? e : e - s);
    };
    const std::string rv = tag("RV=");
    if (rv != "0") {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Win32_ShadowCopy::Create (x64 bridge) returned %s",
                      rv.empty() ? "<no output>" : rv.c_str());
        *err = buf;
        release();
        return false;
    }
    auto widen = [](const std::string& s) {
        std::wstring w;
        w.reserve(s.size());
        for (const unsigned char c : s) w += static_cast<wchar_t>(c);
        return w;
    };
    const std::string sid = tag("SID=");
    const std::string dev = tag("DEV=");
    // Assign BEFORE the output check — a garbled DEV line must not orphan a
    // shadow that was actually created; release() deletes by m_shadowId.
    if (!sid.empty()) m_shadowId = widen(sid); // "{...}" including braces
    if (sid.empty() || dev.empty()) {
        *err = "x64 bridge output missing SID/DEV";
        release();
        return false;
    }
    m_device = widen(dev);
    if (m_device.empty()) {
        *err = "snapshot device object is empty";
        release();
        return false;
    }
    m_haveSnapshot = true;
    return true;
}

void VssSnapshot::release() {
    if (!m_shadowId.empty()) {
        // Best-effort delete of the shadow on EVERY exit path.
        if (m_wbem) {
            const std::wstring path =
                L"Win32_ShadowCopy.ID=\"" + m_shadowId + L"\"";
            BSTR p = SysAllocString(path.c_str());
            m_wbem->DeleteInstance(p, 0, nullptr, nullptr);
            SysFreeString(p);
        } else {
            const std::wstring script =
                L"try { (Get-WmiObject Win32_ShadowCopy -Filter \"ID='" +
                m_shadowId + L"'\").Delete() } catch {}\r\n";
            std::string out;
            std::string derr;
            runPs64(script, 60 * 1000, &out, &derr);
        }
        m_shadowId.clear();
    }
    if (m_wbem) {
        m_wbem->Release();
        m_wbem = nullptr;
    }
    if (m_comp) {
        if (m_haveSnapshot) {
            LONG deleted = 0;
            VSS_ID nondeleted{};
            // Best-effort delete of the whole set on EVERY exit path.
            m_comp->DeleteSnapshots(m_setId, VSS_OBJECT_SNAPSHOT_SET, TRUE,
                                    &deleted, &nondeleted);
        } else {
            m_comp->AbortBackup();
        }
        m_comp->Release();
        m_comp = nullptr;
    }
    m_haveSnapshot = false;
    m_device.clear();
    if (m_comInitialized) {
        m_comInitialized = false;
        CoUninitialize();
    }
}

std::wstring VssSnapshot::shadowPath(
    const std::wstring& originalAbsolutePath) const {
    if (m_device.empty()) return std::wstring();
    return buildShadowPath(m_device, originalAbsolutePath);
}

std::wstring VssSnapshot::buildShadowPath(
    const std::wstring& deviceObject,
    const std::wstring& originalAbsolutePath) {
    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(originalAbsolutePath.c_str(), vol, MAX_PATH))
        return std::wstring();
    const std::wstring root = vol; // e.g. "C:\" (trailing backslash kept)
    if (originalAbsolutePath.compare(0, root.size(), root) != 0)
        return std::wstring();
    // Strip the volume root ("C:\") and graft the remainder onto the
    // snapshot device object.
    const std::wstring rest = originalAbsolutePath.substr(root.size());
    std::wstring dev = deviceObject;
    while (!dev.empty() && dev.back() == L'\\') dev.pop_back();
    return dev + L"\\" + rest;
}

} // namespace tsdev
