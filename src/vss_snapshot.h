// vss_snapshot.h
//
// Minimal Volume Shadow Copy (VSS) snapshot lifecycle for reading files that
// another process holds open deny-read (e.g. TradeStation Desktop's locked
// cache files). Crash-consistent ONLY — NO writer coordination
// (GatherWriterMetadata is deliberately never called); the snapshot is a
// point-in-time filesystem view used to satisfy "copy locked files".
//
// RAII: the snapshot is deleted (DeleteSnapshots + Release) on ALL exits —
// success, failure, and best-effort on crash (process teardown).
//
// Sequence (MSDN-canonical): CoCreateInstance(CLSID_VSSCoordinator,
// IID_IVssBackupComponents) -> InitializeForBackup -> SetContext ->
// StartSnapshotSet -> AddToSnapshotSet(GetVolumePathNameW(path)) ->
// PrepareForBackup(+wait) -> DoSnapshotSet(+bounded wait) ->
// GetSnapshotProperties -> m_pwszSnapshotDeviceObject
// (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN).

#ifndef TSDEV_VSS_SNAPSHOT_H
#define TSDEV_VSS_SNAPSHOT_H

#include <windows.h>

#include <string>

struct IVssBackupComponents;
struct IWbemServices;

namespace tsdev {

class VssSnapshot {
public:
    VssSnapshot() = default;
    ~VssSnapshot() { release(); }
    VssSnapshot(const VssSnapshot&) = delete;
    VssSnapshot& operator=(const VssSnapshot&) = delete;
    VssSnapshot(VssSnapshot&& o) noexcept { steal(o); }
    VssSnapshot& operator=(VssSnapshot&& o) noexcept {
        if (this != &o) {
            release();
            steal(o);
        }
        return *this;
    }

    // Create a crash-consistent snapshot of the volume containing
    // `pathOnVolume`. On success active() == true and deviceObject() holds
    // the \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN path. On failure
    // returns false with a hex-HRESULT/detail string in *err.
    bool create(const std::wstring& pathOnVolume, std::string* err);

    // Delete the snapshot (if taken) and release the COM object. Idempotent.
    void release();

    bool active() const { return m_haveSnapshot; }
    const std::wstring& deviceObject() const { return m_device; }

    // Map an absolute path on the snapshotted volume onto the snapshot
    // device: "C:\dir\f" -> "<deviceObject>\dir\f". Empty on error.
    std::wstring shadowPath(const std::wstring& originalAbsolutePath) const;

    // Pure/testable form of the mapping above (device supplied explicitly).
    static std::wstring buildShadowPath(
        const std::wstring& deviceObject,
        const std::wstring& originalAbsolutePath);

private:
    // Second mechanism: WMI Win32_ShadowCopy (see .cpp). Used when the
    // in-proc vssapi coordinator cannot be created with
    // REGDB_E_CLASSNOTREG — the 32-bit-build-on-x64-Windows case, where the
    // VSS requestor COM class is registered 64-bit only.
    bool createViaWmi(const std::wstring& pathOnVolume, std::string* err);

    // createViaWmi stage 1: direct COM WMI call from this process. Fails on
    // x64 when built Win32 — the shadow-copy method provider refuses WOW64
    // clients (WBEM_E_PROVIDER_NOT_FOUND / initialization failure).
    bool createViaWmiDirect(const std::wstring& volumeRoot, std::string* err);

    // createViaWmi stage 2: identical Win32_ShadowCopy.Create executed by a
    // 64-bit powershell.exe child reached via %windir%\Sysnative (a WOW64-
    // only redirect, i.e. precisely the scenario this fallback covers).
    bool createViaWmiPs64(const std::wstring& volumeRoot, std::string* err);

    void steal(VssSnapshot& o) {
        m_comp = o.m_comp;
        m_setId = o.m_setId;
        m_snapId = o.m_snapId;
        m_haveSnapshot = o.m_haveSnapshot;
        m_comInitialized = o.m_comInitialized;
        m_device = o.m_device;
        m_wbem = o.m_wbem;
        m_shadowId = o.m_shadowId;
        o.m_comp = nullptr;
        o.m_haveSnapshot = false;
        o.m_comInitialized = false;
        o.m_wbem = nullptr;
        o.m_shadowId.clear();
    }

    IVssBackupComponents* m_comp = nullptr;
    GUID m_setId{};
    GUID m_snapId{};
    bool m_haveSnapshot = false;
    bool m_comInitialized = false; // CoInitializeEx(MULTITHREADED) by us
    std::wstring m_device;
    // WMI mechanism state: created shadow ID (GUID string including braces,
    // as returned by the provider). m_wbem is set when the shadow was made
    // in-process (release() calls DeleteInstance); when null the shadow was
    // made by the 64-bit PowerShell bridge (release() re-invokes it).
    IWbemServices* m_wbem = nullptr;
    std::wstring m_shadowId;
};

} // namespace tsdev

#endif // TSDEV_VSS_SNAPSHOT_H
