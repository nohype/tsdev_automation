// update_apply.cpp -- self-update staged-swap apply engine (see
// update_apply.h and design tmp/UPDATE_DESIGN.md section 9).

#include "update_apply.h"
#include "update_feed.h"   // sha256BytesHex for the mutex name

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tsdev {
namespace update {

namespace {

// Case-insensitive ASCII compare (kept local; the CLI TU has its own).
bool iequalsA(const std::wstring& a, const wchar_t* b) {
    const size_t blen = std::wcslen(b);
    if (a.size() != blen) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

std::wstring basenameW(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

// Run a detached, windowless child and wait for it to exit (returns the
// child exit code; -1 on spawn/wait failure). Used for tar.exe and the
// Expand-Archive fallback -- these must complete before the swap.
int runAndWait(std::wstring cmdLine) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Spawn a fully detached hidden process (no wait). Used for the cleanup
// helpers that outlive this process.
bool spawnDetached(std::wstring cmdLine) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// PowerShell single-quoted literal for a path (' -> '').
std::wstring psQuote(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    out += L"'";
    return out;
}

const wchar_t* kPowerShellExe =
    L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
const wchar_t* kTarExe = L"C:\\Windows\\System32\\tar.exe";

// The whitelisted payload members, in swap order. The exe is swapped FIRST
// so the deferred-apply fallback (a rename failure on the running exe)
// always triggers before any other member has moved.
struct PayloadMember {
    const wchar_t* name;
    bool           isDir;
};
const PayloadMember kMembers[] = {
    {L"tsdev_cli.exe",     false},
    {L"tsdev_automation",  true },
    {L"README.md",         false},
};

// Recursive file count under a staged payload member (1 for files).
int countPayloadFiles(const std::wstring& staged) {
    std::error_code ec;
    if (!fs::is_directory(fs::path(staged), ec)) return 1;
    int n = 0;
    for (fs::recursive_directory_iterator it(
             fs::path(staged), fs::directory_options::skip_permission_denied,
             ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) ++n;
    }
    return n;
}

// Delete a file-or-dir path; false when the removal itself failed.
bool removePath(const std::wstring& p) {
    std::error_code ec;
    if (!fs::exists(fs::path(p), ec)) return true;
    fs::remove_all(fs::path(p), ec);
    return !ec;
}

// Detached helper that waits for `waitPid` to exit then deletes every
// *.old sibling in the install dir. Never fatal to the update.
bool scheduleOldCleanup(const std::wstring& installDir, DWORD waitPid) {
    std::wstring cmd = std::wstring(kPowerShellExe) +
        L" -NoProfile -NonInteractive -Command \"Wait-Process -Id " +
        std::to_wstring(waitPid) +
        L" -Timeout 60 -ErrorAction SilentlyContinue; "
        L"Get-ChildItem -LiteralPath " + psQuote(installDir) +
        L" -Filter *.old | Remove-Item -Recurse -Force "
        L"-ErrorAction SilentlyContinue\"";
    return spawnDetached(cmd);
}

// Deferred-apply fallback: leave the staged payload in place and spawn a
// detached helper that waits for this PID then copies the staged members
// over the install dir (the running exe could not be renamed).
bool scheduleDeferredApply(const std::wstring& stagingDir,
                           const std::wstring& installDir, DWORD waitPid) {
    std::wstring cmd = std::wstring(kPowerShellExe) +
        L" -NoProfile -NonInteractive -Command \"Wait-Process -Id " +
        std::to_wstring(waitPid) +
        L" -Timeout 600 -ErrorAction SilentlyContinue; "
        L"Copy-Item -Path " + psQuote(stagingDir + L"\\*") +
        L" -Destination " + psQuote(installDir) +
        L" -Recurse -Force -ErrorAction SilentlyContinue; "
        L"Remove-Item -LiteralPath " + psQuote(stagingDir) +
        L" -Recurse -Force -ErrorAction SilentlyContinue; "
        L"Get-ChildItem -LiteralPath " + psQuote(installDir) +
        L" -Filter *.old | Remove-Item -Recurse -Force "
        L"-ErrorAction SilentlyContinue\"";
    return spawnDetached(cmd);
}

} // namespace

Unpacker resolveUnpacker() {
    if (GetFileAttributesW(kTarExe) != INVALID_FILE_ATTRIBUTES) {
        return Unpacker::Tar;
    }
    if (GetFileAttributesW(kPowerShellExe) != INVALID_FILE_ATTRIBUTES) {
        return Unpacker::PowerShell;
    }
    return Unpacker::None;
}

std::wstring updateStagingDir(const std::wstring& installDir) {
    return installDir + L"\\.tsdev_update_tmp_" +
           std::to_wstring(GetCurrentProcessId());
}

void reapStaleUpdateArtifacts(const std::wstring& installDir,
                              const std::wstring& excludePath) {
    std::error_code ec;
    const fs::path root(installDir);
    for (fs::directory_iterator it(root, fs::directory_options::
                                   skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (!excludePath.empty() &&
            it->path().wstring() == excludePath) {
            continue;  // the in-flight download zip
        }
        // .tsdev_update_* covers both the staging dirs (_tmp_<pid>) and a
        // leftover download scratch (.tsdev_update_dl_<pid>.zip).
        if (name.rfind(L".tsdev_update_", 0) == 0 ||
            (name.size() > 4 &&
             iequalsA(name.substr(name.size() - 4), L".old"))) {
            fs::remove_all(it->path(), ec);
            ec.clear();  // best-effort: keep reaping
        }
    }
}

bool unpackZipToDir(Unpacker u, const std::wstring& zipPath,
                    const std::wstring& dir, std::string& detail) {
    if (u == Unpacker::Tar) {
        const std::wstring cmd = std::wstring(L"\"") + kTarExe +
            L"\" -xf \"" + zipPath + L"\" -C \"" + dir + L"\"";
        const int rc = runAndWait(cmd);
        if (rc != 0) {
            detail = "tar.exe exited " + std::to_string(rc);
            return false;
        }
        return true;
    }
    if (u == Unpacker::PowerShell) {
        const std::wstring cmd = std::wstring(L"\"") + kPowerShellExe +
            L"\" -NoProfile -NonInteractive -Command \"Expand-Archive"
            L" -LiteralPath " + psQuote(zipPath) +
            L" -DestinationPath " + psQuote(dir) + L" -Force\"";
        const int rc = runAndWait(cmd);
        if (rc != 0) {
            detail = "Expand-Archive exited " + std::to_string(rc);
            return false;
        }
        return true;
    }
    detail = "no unpacker available";
    return false;
}

bool verifyPayloadComplete(const std::wstring& stagingDir) {
    std::error_code ec;
    const fs::path staged(stagingDir);
    if (!fs::is_regular_file(staged / L"tsdev_cli.exe", ec)) return false;
    const fs::path pkgDir = staged / L"tsdev_automation";
    if (!fs::is_directory(pkgDir, ec)) return false;
    bool anyFile = false;
    for (fs::recursive_directory_iterator it(
             pkgDir, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) { anyFile = true; break; }
    }
    if (ec || !anyFile) return false;
    if (!fs::is_regular_file(staged / L"README.md", ec)) return false;
    return true;
}

std::wstring updateMutexName(const std::wstring& installDir) {
    // Normalized form: absolute path lowercased, '\' -> '/', UTF-8 encoded.
    std::error_code ec;
    std::wstring norm = fs::absolute(fs::path(installDir), ec).wstring();
    if (ec || norm.empty()) norm = installDir;
    std::replace(norm.begin(), norm.end(), L'\\', L'/');
    std::transform(norm.begin(), norm.end(), norm.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    // Collapse a trailing '/' so "dir" and "dir\" hash identically.
    while (!norm.empty() && norm.back() == L'/') norm.pop_back();
    const std::string utf8 = wideToUtf8(norm);
    const std::string hex = sha256BytesHex(utf8.data(), utf8.size());
    return L"Local\\tsdev_cli_update_" +
           std::wstring(hex.begin(), hex.begin() +
                        std::min<size_t>(16, hex.size()));
}

bool otherTsdevCliInstancesRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    struct SnapGuard {
        HANDLE h;
        ~SnapGuard() { CloseHandle(h); }
    } guard{snap};
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self &&
                iequalsA(pe.szExeFile, L"tsdev_cli.exe")) {
                return true;
            }
        } while (Process32NextW(snap, &pe));
    }
    return false;
}

ApplyResult applyUpdate(const std::wstring& installDir,
                        const std::wstring& zipPath,
                        const std::wstring& selfExePath) {
    ApplyResult res;

    // Best-effort reap of leftovers from prior runs (apply runs only).
    // The just-downloaded zip matches the .tsdev_update_* glob, so it is
    // passed as the explicit exclusion.
    reapStaleUpdateArtifacts(installDir, zipPath);

    const Unpacker unpacker = resolveUnpacker();
    if (unpacker == Unpacker::None) {
        res.error = ApplyError::NoUnpacker;
        return res;
    }

    // Fresh staging dir (reap already removed a stale one for this PID).
    const std::wstring staging = updateStagingDir(installDir);
    {
        std::error_code ec;
        if (!fs::create_directories(fs::path(staging), ec) || ec) {
            res.error = ApplyError::StagingCreateFailed;
            res.detail = ec ? ec.message() : "create_directories failed";
            return res;
        }
    }
    struct StagingGuard {
        std::wstring dir;
        bool keep = false;
        ~StagingGuard() {
            if (!keep) {
                std::error_code ec;
                fs::remove_all(fs::path(dir), ec);
            }
        }
    } stagingGuard{staging, false};

    if (!unpackZipToDir(unpacker, zipPath, staging, res.detail)) {
        res.error = ApplyError::UnpackFailed;
        return res;
    }
    if (!verifyPayloadComplete(staging)) {
        res.error = ApplyError::IncompletePayload;
        return res;
    }

    // Self-concurrency mutex (fail closed). A second updater waits up to
    // 60 s then reports the held lock.
    const std::wstring mutexName = updateMutexName(installDir);
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!hMutex) {
        res.error = ApplyError::LockCreateFailed;
        res.detail = "CreateMutexW error " + std::to_string(GetLastError());
        return res;
    }
    struct MutexGuard {
        HANDLE h;
        ~MutexGuard() { CloseHandle(h); }
    } mg{hMutex};
    const DWORD wait = WaitForSingleObject(hMutex, 60 * 1000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        res.error = ApplyError::LockHeld;
        return res;
    }

    // --- Staged swap ------------------------------------------------------
    const std::wstring selfBase = basenameW(selfExePath);
    const bool selfIsTsdevCli = iequalsA(selfBase, L"tsdev_cli.exe");
    if (!selfIsTsdevCli) res.notes.push_back("renamed_exe");

    // Per-member swap state for rollback: hadOld = an existing member was
    // renamed aside to <name>.old; movedIn = the staged member was moved
    // into the install dir.
    bool hadOld[_countof(kMembers)] = {false, false, false};
    bool movedIn[_countof(kMembers)] = {false, false, false};
    bool deferred = false;

    for (size_t i = 0; i < _countof(kMembers); ++i) {
        const std::wstring cur = installDir + L"\\" + kMembers[i].name;
        const std::wstring old = cur + L".old";
        const std::wstring staged = staging + L"\\" + kMembers[i].name;
        std::error_code ec;
        if (fs::exists(fs::path(old), ec) && !removePath(old)) {
            res.error = ApplyError::SwapFailedRollbackOk;
            res.detail = "cannot remove stale .old for " +
                         wideToUtf8(kMembers[i].name);
            break;
        }
        if (fs::exists(fs::path(cur), ec)) {
            if (!MoveFileW(cur.c_str(), old.c_str())) {
                const DWORD err = GetLastError();
                if (i == 0 && selfIsTsdevCli) {
                    // The running exe refused the rename: leave the staged
                    // payload, defer the copy to a detached helper, and
                    // still report success (spec 9.6). A helper-spawn
                    // failure degrades to a swap failure instead.
                    deferred = scheduleDeferredApply(staging, installDir,
                                                   GetCurrentProcessId());
                    if (deferred) {
                        res.notes.push_back("deferred_apply");
                        break;
                    }
                }
                res.error = ApplyError::SwapFailedRollbackOk;
                res.detail = "rename aside failed for " +
                             wideToUtf8(kMembers[i].name) +
                             " (error " + std::to_string(err) + ")";
                break;
            }
            hadOld[i] = true;
        }
        if (!MoveFileW(staged.c_str(), cur.c_str())) {
            res.error = ApplyError::SwapFailedRollbackOk;
            res.detail = "move-in failed for " +
                         wideToUtf8(kMembers[i].name) +
                         " (error " + std::to_string(GetLastError()) + ")";
            break;
        }
        movedIn[i] = true;
        res.filesReplaced += countPayloadFiles(cur);
    }

    if (res.error != ApplyError::None) {
        // Rollback: remove partial new members and rename the .old set
        // back over them. A restore failure upgrades the error class.
        bool restoreFailed = false;
        for (size_t i = 0; i < _countof(kMembers); ++i) {
            const std::wstring cur = installDir + L"\\" + kMembers[i].name;
            const std::wstring old = cur + L".old";
            if (movedIn[i] && !removePath(cur)) restoreFailed = true;
            if (hadOld[i] && !MoveFileW(old.c_str(), cur.c_str())) {
                restoreFailed = true;
            }
        }
        res.rolledBack = true;
        res.filesReplaced = 0;
        if (restoreFailed) res.error = ApplyError::SwapFailedRollbackFailed;
        return res;
    }

    if (deferred) {
        // Staging stays (the detached helper consumes it); no .old set was
        // created because the exe member is swapped first.
        stagingGuard.keep = true;
        res.filesReplaced = 0;
        for (const PayloadMember& m : kMembers) {
            res.filesReplaced +=
                countPayloadFiles(staging + L"\\" + m.name);
        }
        return res;
    }

    // Swap complete. The .old set is locked while this process runs, so a
    // detached helper deletes it after this PID exits.
    stagingGuard.keep = false;
    if (!scheduleOldCleanup(installDir, GetCurrentProcessId())) {
        res.notes.push_back("staging_cleanup_deferred");
    }
    if (otherTsdevCliInstancesRunning()) {
        res.notes.push_back("other_instances_running");
    }
    return res;
}

} // namespace update
} // namespace tsdev
