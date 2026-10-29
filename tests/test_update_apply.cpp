// test_update_apply.cpp
//
// Unit test for the self-update apply engine src/update_apply.cpp
// (design tmp/UPDATE_DESIGN.md section 9). Exercises the REAL staging /
// unpack / completeness-gate / swap / rollback / mutex-name code paths on
// REAL temp directories under %TEMP%. No network: the test builds its own
// bundle zips with the OS tar.exe (bsdtar; allowed in-test) from a fake
// payload (fake tsdev_cli.exe + tsdev_automation\__init__.py + README.md)
// and drives applyUpdate() with an explicit installDir/selfExePath.
//
// This is a PURE UNIT TEST -- it does NOT load orlib20.dll and does NOT
// touch the TradeStation installation. It runs on a worker thread with a
// 64 MB stack only to match the project's test-harness convention.
//
// Checks:
//   1. Completeness gate: a zip missing README.md fails BEFORE any rename
//      (IncompletePayload; install dir untouched).
//   2. Successful swap: old members land as *.old, new payload members are
//      in place, filesReplaced counts all payload files.
//   3. Rollback: a locked file inside a planted tsdev_automation.old makes
//      the mid-swap .old removal fail; the exe member is restored
//      (rolledBack, SwapFailedRollbackOk).
//   4. Mutex-name determinism: same dir -> same name, different dir ->
//      different name, trailing-slash normalized.
//   5. Stale-artifact reaping: *.old / .tsdev_update_tmp_* leftovers are
//      removed at apply start.
//   6. Deferred apply: a lock-held running exe triggers the
//      deferred_apply note and keeps the staged payload.
//   7. Renamed exe: a non-tsdev_cli.exe selfExePath produces the
//      renamed_exe note.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/update_apply.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

using tsdev::update::ApplyError;
using tsdev::update::ApplyResult;
using tsdev::update::applyUpdate;
using tsdev::update::otherTsdevCliInstancesRunning;
using tsdev::update::resolveUnpacker;
using tsdev::update::Unpacker;
using tsdev::update::updateMutexName;
using tsdev::update::updateStagingDir;
using tsdev::update::verifyPayloadComplete;

namespace fs = std::filesystem;

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "\n[FAIL] %s (code=%d)\n", msg, code);
    return code;
}

bool writeFileW(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, content.data(),
                              static_cast<DWORD>(content.size()), &w,
                              nullptr);
    CloseHandle(h);
    return ok && w == content.size();
}

std::string readFileW(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    char buf[256];
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf), &n, nullptr);
    CloseHandle(h);
    return std::string(buf, n);
}

// Create a unique temp dir under %TEMP% and return it (no trailing \).
std::wstring makeTempDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"tsdev_ua_" + tag + L"_" +
                       std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64());
    fs::create_directories(fs::path(dir));
    return dir;
}

// Populate a bundle payload dir: fake tsdev_cli.exe + tsdev_automation\
// __init__.py + README.md. When `withReadme` is false the README is
// omitted (the incomplete-payload fixture).
void buildPayloadDir(const std::wstring& dir, bool withReadme,
                     const char* marker) {
    fs::create_directories(fs::path(dir) / L"tsdev_automation");
    writeFileW(dir + L"\\tsdev_cli.exe",
               std::string("MZ-FAKE-EXE-") + marker);
    writeFileW(dir + L"\\tsdev_automation\\__init__.py",
               std::string("# pkg ") + marker);
    writeFileW(dir + L"\\tsdev_automation\\client.py",
               std::string("# client ") + marker);
    if (withReadme) {
        writeFileW(dir + L"\\README.md", std::string("# readme ") + marker);
    }
}

// Zip a directory's contents with the OS tar.exe. Returns "" on failure.
std::wstring makeZip(const std::wstring& payloadDir, const std::wstring& zipPath) {
    const wchar_t* kTar = L"C:\\Windows\\System32\\tar.exe";
    std::wstring cmd = std::wstring(L"\"") + kTar + L"\" -cf \"" + zipPath +
                       L"\" -C \"" + payloadDir + L"\" .";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return L"";
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (code == 0 && fs::exists(zipPath)) ? zipPath : L"";
}

void installOldVersion(const std::wstring& install) {
    fs::create_directories(fs::path(install) / L"tsdev_automation");
    writeFileW(install + L"\\tsdev_cli.exe", "MZ-OLD-EXE");
    writeFileW(install + L"\\tsdev_automation\\__init__.py", "# old pkg");
    writeFileW(install + L"\\README.md", "# old readme");
}

struct TestState {
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int& ec = st->exitCode;
    ec = 0;

    if (resolveUnpacker() != Unpacker::Tar) {
        // This test's fixture builder uses tar.exe; bail loudly if absent.
        ec = fail("prereq: C:\\Windows\\System32\\tar.exe unavailable", 5);
        return 0;
    }

    const std::wstring selfFake =
        makeTempDir(L"self") + L"\\tsdev_cli.exe";  // basename matches

    // ------------------------------------------------------------------
    // Check 1: completeness gate -- incomplete payload fails BEFORE any
    // rename; the install dir stays untouched.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c1i");
        const std::wstring payload = makeTempDir(L"c1p");
        installOldVersion(install);
        buildPayloadDir(payload, /*withReadme=*/false, "new");
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) { ec = fail("check1: zip build failed", 10); return 0; }
        const ApplyResult r = applyUpdate(install, zip, selfFake);
        if (r.error != ApplyError::IncompletePayload) {
            std::fprintf(stderr, "\n[FAIL] check1: error=%d expected "
                         "IncompletePayload\n", (int)r.error);
            ec = 11; return 0;
        }
        // Nothing renamed: originals intact, no .old, staging reaped.
        if (readFileW(install + L"\\tsdev_cli.exe") != "MZ-OLD-EXE" ||
            !fs::exists(install + L"\\README.md")) {
            ec = fail("check1: install dir was modified", 12); return 0;
        }
        std::error_code sec;
        for (auto& e : fs::directory_iterator(install, sec)) {
            const std::wstring n = e.path().filename().wstring();
            if (n.size() > 4 && n.substr(n.size() - 4) == L".old") {
                ec = fail("check1: .old left behind", 13); return 0;
            }
        }
        std::fprintf(stdout, "    check1 completeness gate PASS\n");
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    // ------------------------------------------------------------------
    // Check 2: successful swap -- .old set created, new payload in place,
    // filesReplaced counts every payload file.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c2i");
        const std::wstring payload = makeTempDir(L"c2p");
        installOldVersion(install);
        buildPayloadDir(payload, true, "new");
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) { ec = fail("check2: zip build failed", 20); return 0; }
        const ApplyResult r = applyUpdate(install, zip, selfFake);
        if (r.error != ApplyError::None) {
            std::fprintf(stderr, "\n[FAIL] check2: error=%d detail=%s\n",
                         (int)r.error, r.detail.c_str());
            ec = 21; return 0;
        }
        // exe + 2 pkg files + README = 4 payload files.
        if (r.filesReplaced != 4) {
            std::fprintf(stderr, "\n[FAIL] check2: filesReplaced=%d "
                         "expected 4\n", r.filesReplaced);
            ec = 22; return 0;
        }
        if (readFileW(install + L"\\tsdev_cli.exe") != "MZ-FAKE-EXE-new" ||
            readFileW(install + L"\\README.md") != "# readme new" ||
            readFileW(install + L"\\tsdev_automation\\client.py") !=
                "# client new") {
            ec = fail("check2: new payload not in place", 23); return 0;
        }
        // .old set still present (the detached cleaner waits for our PID).
        if (!fs::exists(install + L"\\tsdev_cli.exe.old") ||
            !fs::exists(install + L"\\README.md.old") ||
            !fs::exists(install + L"\\tsdev_automation.old")) {
            ec = fail("check2: .old set missing", 24); return 0;
        }
        if (readFileW(install + L"\\tsdev_cli.exe.old") != "MZ-OLD-EXE") {
            ec = fail("check2: .old content wrong", 25); return 0;
        }
        // Staging dir is gone (cleaned synchronously on the normal path).
        std::error_code sec;
        bool stagingLeft = false;
        for (auto& e : fs::directory_iterator(install, sec)) {
            if (e.path().filename().wstring().rfind(
                    L".tsdev_update_tmp_", 0) == 0) stagingLeft = true;
        }
        if (stagingLeft) {
            ec = fail("check2: staging dir left behind", 26); return 0;
        }
        std::fprintf(stdout, "    check2 successful swap PASS "
                     "(filesReplaced=%d)\n", r.filesReplaced);
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    // ------------------------------------------------------------------
    // Check 3: rollback -- a locked file inside a planted stale
    // tsdev_automation.old makes the member-1 .old removal fail AFTER the
    // exe member already swapped; the exe must be restored.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c3i");
        const std::wstring payload = makeTempDir(L"c3p");
        installOldVersion(install);
        buildPayloadDir(payload, true, "new");
        // Plant a stale tsdev_automation.old dir holding a lock-open file
        // so remove_all on it fails at swap time (the reap pass fails on
        // it silently, leaving it for the swap's stale-.old step).
        fs::create_directories(fs::path(install) / L"tsdev_automation.old");
        writeFileW(install + L"\\tsdev_automation.old\\lock.txt", "x");
        HANDLE hLock = CreateFileW(
            (install + L"\\tsdev_automation.old\\lock.txt").c_str(),
            GENERIC_READ, 0 /* no sharing */, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLock == INVALID_HANDLE_VALUE) {
            ec = fail("check3: lock setup failed", 30); return 0;
        }
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) {
            CloseHandle(hLock);
            ec = fail("check3: zip build failed", 31); return 0;
        }
        const ApplyResult r = applyUpdate(install, zip, selfFake);
        CloseHandle(hLock);
        if (r.error != ApplyError::SwapFailedRollbackOk || !r.rolledBack) {
            std::fprintf(stderr, "\n[FAIL] check3: error=%d rolledBack=%d\n",
                         (int)r.error, (int)r.rolledBack);
            ec = 32; return 0;
        }
        if (r.filesReplaced != 0) {
            ec = fail("check3: filesReplaced must be 0 on rollback", 33);
            return 0;
        }
        // The original exe must be restored in place (rollback renamed
        // tsdev_cli.exe.old back over the moved-in new exe).
        if (readFileW(install + L"\\tsdev_cli.exe") != "MZ-OLD-EXE") {
            ec = fail("check3: original exe not restored", 34); return 0;
        }
        std::fprintf(stdout, "    check3 mid-swap rollback PASS\n");
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    // ------------------------------------------------------------------
    // Check 4: mutex-name determinism.
    // ------------------------------------------------------------------
    {
        const std::wstring a = updateMutexName(L"C:\\Tools\\TsDev");
        const std::wstring a2 = updateMutexName(L"C:\\Tools\\TsDev\\");
        const std::wstring b = updateMutexName(L"C:\\Other\\Dir");
        if (a != a2) {
            ec = fail("check4: trailing-slash forms must hash identically",
                      40); return 0;
        }
        if (a == b) {
            ec = fail("check4: different dirs must hash differently", 41);
            return 0;
        }
        if (a.rfind(L"Local\\tsdev_cli_update_", 0) != 0 ||
            a.size() != std::wstring(L"Local\\tsdev_cli_update_").size() + 16) {
            std::fprintf(stderr, "\n[FAIL] check4: mutex name shape: %ls\n",
                         a.c_str());
            ec = 42; return 0;
        }
        std::fprintf(stdout, "    check4 mutex-name determinism PASS\n");
    }

    // ------------------------------------------------------------------
    // Check 5: stale reaping -- *.old and .tsdev_update_tmp_* leftovers
    // vanish at apply start.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c5i");
        const std::wstring payload = makeTempDir(L"c5p");
        installOldVersion(install);
        buildPayloadDir(payload, true, "new");
        writeFileW(install + L"\\leftover.old", "stale");
        fs::create_directories(fs::path(install) / L".tsdev_update_tmp_1");
        writeFileW(install + L"\\.tsdev_update_tmp_1\\x.bin", "stale");
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) { ec = fail("check5: zip build failed", 50); return 0; }
        const ApplyResult r = applyUpdate(install, zip, selfFake);
        if (r.error != ApplyError::None) {
            ec = fail("check5: apply failed", 51); return 0;
        }
        if (fs::exists(install + L"\\leftover.old") ||
            fs::exists(install + L"\\.tsdev_update_tmp_1")) {
            ec = fail("check5: stale artifacts were not reaped", 52);
            return 0;
        }
        std::fprintf(stdout, "    check5 stale reaping PASS\n");
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    // ------------------------------------------------------------------
    // Check 6: deferred apply -- the running exe is held open with no
    // sharing so its rename fails; the staged payload stays for the
    // detached helper and the deferred_apply note fires.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c6i");
        const std::wstring payload = makeTempDir(L"c6p");
        installOldVersion(install);
        buildPayloadDir(payload, true, "new");
        HANDLE hExe = CreateFileW((install + L"\\tsdev_cli.exe").c_str(),
                                  GENERIC_READ, 0 /* no sharing */, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (hExe == INVALID_HANDLE_VALUE) {
            ec = fail("check6: exe lock setup failed", 60); return 0;
        }
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) {
            CloseHandle(hExe);
            ec = fail("check6: zip build failed", 61); return 0;
        }
        const ApplyResult r = applyUpdate(install, zip,
                                          install + L"\\tsdev_cli.exe");
        CloseHandle(hExe);
        if (r.error != ApplyError::None) {
            std::fprintf(stderr, "\n[FAIL] check6: error=%d detail=%s\n",
                         (int)r.error, r.detail.c_str());
            ec = 62; return 0;
        }
        bool sawDeferred = false;
        for (const std::string& n : r.notes) {
            if (n == "deferred_apply") sawDeferred = true;
        }
        if (!sawDeferred) {
            ec = fail("check6: deferred_apply note missing", 63); return 0;
        }
        // Staging kept (the detached helper consumes it after our PID exits).
        if (!fs::exists(install + L"\\.tsdev_update_tmp_" +
                        std::to_wstring(GetCurrentProcessId()))) {
            ec = fail("check6: staging dir was not kept for deferred apply",
                      64); return 0;
        }
        // Old exe still in place (rename failed).
        if (readFileW(install + L"\\tsdev_cli.exe") != "MZ-OLD-EXE") {
            ec = fail("check6: old exe moved despite the lock", 65);
            return 0;
        }
        std::fprintf(stdout, "    check6 deferred apply PASS\n");
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    // ------------------------------------------------------------------
    // Check 7: renamed exe -- a self path whose basename is not
    // tsdev_cli.exe produces the renamed_exe note.
    // ------------------------------------------------------------------
    {
        const std::wstring install = makeTempDir(L"c7i");
        const std::wstring payload = makeTempDir(L"c7p");
        installOldVersion(install);
        buildPayloadDir(payload, true, "new");
        const std::wstring zip = makeZip(payload, install + L"\\b.zip");
        if (zip.empty()) { ec = fail("check7: zip build failed", 70); return 0; }
        const ApplyResult r = applyUpdate(install, zip,
                                          install + L"\\mytool.exe");
        if (r.error != ApplyError::None) {
            ec = fail("check7: apply failed", 71); return 0;
        }
        bool sawRenamed = false;
        for (const std::string& n : r.notes) {
            if (n == "renamed_exe") sawRenamed = true;
        }
        if (!sawRenamed) {
            ec = fail("check7: renamed_exe note missing", 72); return 0;
        }
        std::fprintf(stdout, "    check7 renamed-exe note PASS\n");
        fs::remove_all(zip);
        fs::remove_all(payload);
    }

    std::fprintf(stdout, "\n=== ALL CHECKS PASSED ===\n");
    return 0;
}

// SEH wrapper -- POD-only so __try is legal.
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
    std::fprintf(stdout,
                 "=== test_update_apply: apply/swap engine (unit) ===\n");

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
    std::fprintf(stdout,
                 "\n=== test_update_apply complete (exit %d) ===\n", exitCode);
    if (exitCode == 0) {
        std::fprintf(stdout, "RESULT: ALL CHECKS PASSED\n");
    } else {
        std::fprintf(stdout, "RESULT: CHECKS FAILED\n");
    }
    return exitCode;
}
