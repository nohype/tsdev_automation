// update_apply.h -- self-update staged-swap apply engine
// (design docs/UPDATE_DESIGN.md section 9).
//
// Pure-Win32 module (kernel32 + toolhelp + tar.exe/Expand-Archive
// subprocesses; no orlib20, no TS). Semantics:
//   * Staging <install_dir>\.tsdev_update_tmp_<pid> on the same volume;
//     stale .tsdev_update_tmp_* / *.old reaped at apply start only.
//   * Unpack via C:\Windows\System32\tar.exe, else PowerShell
//     Expand-Archive, else a typed no-unpacker error.
//   * Whitelist completeness gate BEFORE any rename: tsdev_cli.exe,
//     tsdev_automation\ (>=1 file), README.md.
//   * Swap: per member, rename existing -> <name>.old then move the staged
//     member in (MoveFileW, same volume). Mid-failure rolls the .old set
//     back over the partials.
//   * Self-concurrency: Local\tsdev_cli_update_<sha256-16> mutex where the
//     suffix is the first 16 hex chars of the lowercase SHA-256 of the
//     normalized install dir (lowercased, '\' -> '/', UTF-8).
//   * Deferred-apply fallback: a running exe that cannot be renamed leaves
//     the staged payload in place; a detached helper finishes the copy
//     after this PID exits -> "deferred_apply" note.
//   * Notes: "other_instances_running" (another tsdev_cli.exe alive),
//     "renamed_exe" (own basename != tsdev_cli.exe),
//     "staging_cleanup_deferred" (detached .old cleanup could not be
//     scheduled -- never fatal).

#pragma once

#include <string>
#include <vector>

namespace tsdev {
namespace update {

// Which OS-provided unpacker is available on this machine.
enum class Unpacker {
    None,        // neither tar.exe nor PowerShell Expand-Archive
    Tar,         // C:\Windows\System32\tar.exe (bsdtar)
    PowerShell,  // Expand-Archive fallback
};

// Resolve the unpacker capability BEFORE any download starts.
Unpacker resolveUnpacker();

enum class ApplyError {
    None,
    NoUnpacker,               // tar.exe AND Expand-Archive absent
    StagingCreateFailed,      // cannot create the staging dir (write denied)
    UnpackFailed,             // unpacker exited nonzero / unpack produced nothing
    IncompletePayload,        // staged payload missing whitelisted members
    LockCreateFailed,         // CreateMutexW failed (fail closed)
    LockHeld,                 // another update holds the mutex (60 s wait)
    SwapFailedRollbackOk,     // swap died mid-flight; .old set restored
    SwapFailedRollbackFailed, // swap died AND restore failed
};

struct ApplyResult {
    ApplyError  error = ApplyError::None;
    int         filesReplaced = 0;   // payload members written to install dir
    bool        rolledBack = false;  // .old restore ran after a swap failure
    std::vector<std::string> notes;  // machine tokens (see header comment)
    std::string detail;              // win32 detail for --log-file/messages
};

// <install_dir>\.tsdev_update_tmp_<pid> -- same volume as the install dir
// so the swap renames stay in-volume.
std::wstring updateStagingDir(const std::wstring& installDir);

// Best-effort removal of stale .tsdev_update_* artifacts (staging dirs and
// download scratch files) and *.old files in the install dir. Runs at the
// start of an APPLY run only (update check is strictly read-only).
// `excludePath` (optional) protects the in-flight download zip.
void reapStaleUpdateArtifacts(const std::wstring& installDir,
                              const std::wstring& excludePath = L"");

// Extract `zipPath` into the empty `dir` via the resolved unpacker.
bool unpackZipToDir(Unpacker u, const std::wstring& zipPath,
                    const std::wstring& dir, std::string& detail);

// Completeness gate: staged tsdev_cli.exe + tsdev_automation\ dir with
// >=1 file + README.md must ALL be present before any rename happens.
bool verifyPayloadComplete(const std::wstring& stagingDir);

// Local\tsdev_cli_update_<sha256-16> for the normalized install dir.
std::wstring updateMutexName(const std::wstring& installDir);

// Toolhelp snapshot: another live process whose image basename is
// tsdev_cli.exe (own PID excluded).
bool otherTsdevCliInstancesRunning();

// Detached post-exit helpers (hidden powershell.exe, no wait handle kept).
// They wait on `waitPid` then act: scheduleOldCleanup deletes every *.old
// sibling in the install dir; scheduleDeferredApply copies ONLY the
// whitelisted payload members over the install dir then removes the
// staging dir. A dead `waitPid` makes the helper act immediately (used
// by tests). Never fatal to the update.
bool scheduleOldCleanup(const std::wstring& installDir,
                        unsigned long waitPid);
bool scheduleDeferredApply(const std::wstring& stagingDir,
                           const std::wstring& installDir,
                           unsigned long waitPid);

// Full apply: acquire the self-concurrency mutex -> reap stale artifacts
// -> create staging -> unpack -> verify payload -> swap -> notes ->
// schedule .old cleanup. `selfExePath` is explicit for testability
// (production passes GetModuleFileNameW); the running exe is never
// MoveFile'd across volumes and a failed rename of it triggers the
// deferred-apply fallback.
ApplyResult applyUpdate(const std::wstring& installDir,
                        const std::wstring& zipPath,
                        const std::wstring& selfExePath);

} // namespace update
} // namespace tsdev
