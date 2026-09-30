// test_cache_backup.cpp
//
// Unit test for src/cache_backup.cpp: enumerate, staged backup/swap,
// backup.info round-trip, transactional restore, lock probe, and the
// runningTsProcessNames snapshot.
//
// This is a PURE UNIT TEST. Everything runs under %TEMP% — it never touches
// the real TradeStation installation, %LOCALAPPDATA%, or orlib20.
//
// The implementation in src/cache_backup.cpp is compiled directly into this
// test executable (see CMakeLists.txt), so no library link is needed.
//
// Exit code: 0 only if ALL checks pass; distinct nonzero codes per failure.

#include "../src/cache_backup.h"
#include "../src/vss_snapshot.h"

#include <windows.h>
#include <wbemidl.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int fail(const char* msg, int code) {
    std::fprintf(stderr, "  FAIL: %s (exit %d)\n", msg, code);
    return code;
}

fs::path g_tmp;

bool writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
    return out.good();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::wstring w(const fs::path& p) { return p.wstring(); }

int progressCalls = 0;
void countProgress(uint64_t, uint64_t, const std::wstring&) {
    ++progressCalls;
}

// Count live Win32_ShadowCopy instances via WMI (same mechanism the engine
// falls back to on Win32). Returns the count, or -1 when WMI is genuinely
// unavailable so callers can skip leak checks gracefully.
int wmiShadowCount() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInit = (hr == S_OK);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return -1;
    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                              EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        if (weInit) CoUninitialize();
        return -1;
    }
    int count = -1;
    IWbemLocator* loc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                   reinterpret_cast<void**>(&loc)))) {
        IWbemServices* svc = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr,
                                nullptr, &svc);
        SysFreeString(ns);
        loc->Release();
        if (SUCCEEDED(hr) &&
            SUCCEEDED(CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT,
                                        RPC_C_AUTHZ_NONE, nullptr,
                                        RPC_C_AUTHN_LEVEL_CALL,
                                        RPC_C_IMP_LEVEL_IMPERSONATE,
                                        nullptr, EOAC_NONE))) {
            BSTR lang = SysAllocString(L"WQL");
            BSTR q = SysAllocString(L"SELECT ID FROM Win32_ShadowCopy");
            IEnumWbemClassObject* en = nullptr;
            hr = svc->ExecQuery(lang, q,
                                WBEM_FLAG_FORWARD_ONLY |
                                    WBEM_FLAG_RETURN_IMMEDIATELY,
                                nullptr, &en);
            SysFreeString(lang);
            SysFreeString(q);
            if (SUCCEEDED(hr) && en) {
                count = 0;
                for (;;) {
                    IWbemClassObject* row = nullptr;
                    ULONG got = 0;
                    hr = en->Next(WBEM_INFINITE, 1, &row, &got);
                    if (FAILED(hr)) { count = -1; break; }
                    if (got == 0) break;
                    row->Release();
                    ++count;
                }
                en->Release();
            }
            svc->Release();
        }
    }
    if (weInit) CoUninitialize();
    return count;
}

int runTests() {
    // Unique temp sandbox.
    g_tmp = fs::temp_directory_path() /
            (L"tsdev_cache_backup_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(g_tmp);
    fs::create_directories(g_tmp);

    // Fake source cache tree: 3 files, known bytes.
    const fs::path src = g_tmp / L"source_cache";
    writeFile(src / "a.bin", "AAAA");                    // 4
    writeFile(src / "sub" / "b.bin", "BBBBBB");          // 6
    writeFile(src / "sub" / "deep" / "c.bin", "CC");     // 2
    const uint64_t kBytes = 12;

    const fs::path backupRoot = g_tmp / L"backup_root";

    // -------------------------------------------------------------------
    // Group 1: enumerateTree counts and byte totals.
    // -------------------------------------------------------------------
    {
        tsdev::CacheEnumerateResult en = tsdev::enumerateTree(w(src));
        if (!en.ok) return fail("enumerateTree(source) failed", 10);
        if (en.files.size() != 3)
            return fail("enumerateTree file count != 3", 11);
        if (en.totalBytes != kBytes)
            return fail("enumerateTree totalBytes != 12", 12);
        tsdev::CacheEnumerateResult miss =
            tsdev::enumerateTree(w(g_tmp / L"no_such_dir"));
        if (miss.ok) return fail("enumerateTree(missing) succeeded", 13);
        std::fprintf(stdout, "    PASS: group 1 (enumerate)\n");
    }

    // -------------------------------------------------------------------
    // Group 2: backup produces exactly one complete backup; info round-trip.
    // -------------------------------------------------------------------
    {
        progressCalls = 0;
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(backupRoot), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("backupCache failed", 20);
        if (r.files != 3 || r.bytes != kBytes)
            return fail("backup result files/bytes wrong", 21);
        if (progressCalls < 3)
            return fail("progress callback not invoked per file", 22);
        if (readFile(backupRoot / "cache" / "sub" / "b.bin") != "BBBBBB")
            return fail("backup content mismatch", 23);
        if (fs::exists(backupRoot / ("staging-" +
                       std::to_string(GetCurrentProcessId()))))
            return fail("staging dir left behind", 24);

        tsdev::BackupInfo info = tsdev::readBackupInfo(w(backupRoot));
        if (!info.exists || !info.complete)
            return fail("readBackupInfo: not complete", 25);
        if (info.files != 3 || info.bytes != kBytes)
            return fail("readBackupInfo: files/bytes wrong", 26);
        if (info.source != src.wstring())
            return fail("readBackupInfo: source mismatch", 27);
        if (info.created.size() != 19 || info.created[4] != '-')
            return fail("readBackupInfo: created format wrong", 28);
        std::fprintf(stdout, "    PASS: group 2 (backup + info)\n");
    }

    // -------------------------------------------------------------------
    // Group 3: second backup replaces cleanly; source change reflected.
    // -------------------------------------------------------------------
    {
        writeFile(src / "a.bin", "AAAAAAAA");   // grows to 8; total 16
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(backupRoot), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("second backupCache failed", 30);
        tsdev::BackupInfo info = tsdev::readBackupInfo(w(backupRoot));
        if (!info.complete || info.bytes != 16 || info.files != 3)
            return fail("second backup info stale", 31);
        if (readFile(backupRoot / "cache" / "a.bin") != "AAAAAAAA")
            return fail("second backup content stale", 32);
        // Exactly one backup visible: no old-*/staging-* remnants.
        for (const auto& e : fs::directory_iterator(backupRoot)) {
            const std::wstring n = e.path().filename().wstring();
            if (n != L"cache" && n != L"backup.info")
                return fail("unexpected entry in backup root", 33);
        }
        std::fprintf(stdout, "    PASS: group 3 (re-backup swap)\n");
    }

    // -------------------------------------------------------------------
    // Group 4: locked source file — either a VSS-captured success (elevated
    // run, VSS available) or a clean failure that leaves the prior backup
    // intact and staging cleaned.
    // -------------------------------------------------------------------
    {
        // Lock one source file deny-all so CopyFileExW fails mid-copy.
        const std::wstring locked = (src / "sub" / "b.bin").wstring();
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not lock source file for injection", 40);
        const std::string before =
            readFile(backupRoot / "cache" / "a.bin");
        const std::string infoBefore =
            readFile(backupRoot / "backup.info");
        tsdev::CacheOpResult r = tsdev::backupCache(
            w(src), w(backupRoot), countProgress);
        CloseHandle(h);
        if (r.status == tsdev::CacheOpStatus::Ok) {
            // VSS path engaged — the locked file was captured via snapshot.
            if (!r.usedVss)
                return fail("locked-file backup succeeded without VSS", 45);
            if (readFile(backupRoot / "cache" / "sub" / "b.bin") != "BBBBBB")
                return fail("VSS-captured content mismatch", 46);
            if (readFile(backupRoot / "backup.info")
                    .find("live_source=true") == std::string::npos)
                return fail("live_source flag missing after VSS", 47);
            std::fprintf(stdout,
                         "    PASS: group 4 (locked file captured via "
                         "VSS)\n");
        } else {
            if (readFile(backupRoot / "cache" / "a.bin") != before)
                return fail("prior backup mutated by failed backup", 42);
            if (readFile(backupRoot / "backup.info") != infoBefore)
                return fail("prior backup.info mutated by failed backup",
                            43);
            if (fs::exists(backupRoot / ("staging-" +
                           std::to_string(GetCurrentProcessId()))))
                return fail("staging not cleaned after failure", 44);
            std::fprintf(stdout,
                         "    PASS: group 4 (failed backup, VSS "
                         "unavailable)\n");
        }
        // Either way the visible backup holds the same committed content
        // (the VSS capture copies identical bytes), so group 5 is unaffected.
    }

    // -------------------------------------------------------------------
    // Group 5: restore happy path — byte-identical content, old moved aside.
    // -------------------------------------------------------------------
    const fs::path dest = g_tmp / L"dest_cache";
    {
        writeFile(dest / "obsolete.bin", "OLD");
        writeFile(dest / "sub" / "b.bin", "STALE");
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("restoreCache failed", 50);
        if (r.files != 3 || r.bytes != 16)
            return fail("restore result files/bytes wrong", 51);
        if (readFile(dest / "a.bin") != "AAAAAAAA" ||
            readFile(dest / "sub" / "b.bin") != "BBBBBB" ||
            readFile(dest / "sub" / "deep" / "c.bin") != "CC")
            return fail("restored content mismatch", 52);
        if (fs::exists(dest / "obsolete.bin"))
            return fail("pre-existing file survived restore", 53);
        std::fprintf(stdout, "    PASS: group 5 (restore happy path)\n");
    }

    // -------------------------------------------------------------------
    // Group 6: restore with no backup -> NotFound.
    // -------------------------------------------------------------------
    {
        tsdev::CacheOpResult r = tsdev::restoreCache(
            w(g_tmp / L"empty_root"), w(g_tmp / L"dest2"), countProgress);
        if (r.status != tsdev::CacheOpStatus::NotFound)
            return fail("restore without backup did not return NotFound", 60);
        std::fprintf(stdout, "    PASS: group 6 (restore NotFound)\n");
    }

    // -------------------------------------------------------------------
    // Group 7: restore lock probe detects an open-handle destination file.
    // -------------------------------------------------------------------
    {
        const std::wstring locked = (dest / "a.bin").wstring();
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not lock dest file for probe test", 70);
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest), countProgress);
        CloseHandle(h);
        if (r.status != tsdev::CacheOpStatus::InUse)
            return fail("lock probe did not report InUse", 71);
        if (r.errorPath != locked)
            return fail("lock probe did not report the locked path", 72);
        if (readFile(dest / "a.bin") != "AAAAAAAA")
            return fail("destination mutated despite InUse", 73);
        std::fprintf(stdout, "    PASS: group 7 (restore InUse probe)\n");
    }

    // -------------------------------------------------------------------
    // Group 8: free-space pre-flight + process snapshot run cleanly.
    // -------------------------------------------------------------------
    {
        uint64_t avail = 0;
        // Path deliberately non-existent yet — ancestor walk must handle it.
        if (!tsdev::checkFreeSpace(w(g_tmp / L"future" / L"root"), 1, avail))
            return fail("checkFreeSpace(1 byte) failed", 80);
        if (avail == 0) return fail("checkFreeSpace reported 0 free", 81);
        uint64_t avail2 = 0;
        if (tsdev::checkFreeSpace(w(g_tmp), ~0ULL, avail2))
            return fail("checkFreeSpace(~0) succeeded", 82);
        // Just assert it runs and returns only known TS names.
        for (const auto& n : tsdev::runningTsProcessNames()) {
            if (n != L"orchart.exe" && n != L"tsdev.exe" &&
                n != L"whserver.exe")
                return fail("runningTsProcessNames returned foreign name", 83);
        }
        std::fprintf(stdout, "    PASS: group 8 (free space + snapshot)\n");
    }

    // -------------------------------------------------------------------
    // Group 9: corrupt/truncated/empty backup.info -> exists && !complete,
    //          and restore -> NotFound. Malformed keys must not crash.
    // -------------------------------------------------------------------
    {
        const fs::path badRoot = g_tmp / L"bad_info";
        writeFile(badRoot / "cache" / "x.bin", "X");
        // Truncated: no complete= line.
        writeFile(badRoot / "backup.info", "format=1\nfiles=1\nbytes=1\n");
        tsdev::BackupInfo i = tsdev::readBackupInfo(w(badRoot));
        if (!i.exists || i.complete)
            return fail("truncated info: expected exists && !complete", 90);
        if (tsdev::restoreCache(w(badRoot), w(g_tmp / L"d9"),
                                countProgress)
                .status != tsdev::CacheOpStatus::NotFound)
            return fail("restore with truncated info != NotFound", 91);
        // Empty file.
        writeFile(badRoot / "backup.info", "");
        i = tsdev::readBackupInfo(w(badRoot));
        if (!i.exists || i.complete)
            return fail("empty info: expected exists && !complete", 92);
        // Garbage / malformed keys.
        writeFile(badRoot / "backup.info",
                  "garbage-no-keys\n===\nfiles=zzz\ncomplete=yes\n");
        i = tsdev::readBackupInfo(w(badRoot));
        if (!i.exists || i.complete)
            return fail("garbage info: expected exists && !complete", 93);
        std::fprintf(stdout, "    PASS: group 9 (corrupt info)\n");
    }

    // -------------------------------------------------------------------
    // Group 10: backup sweeps staging-*/old-*/foreign residue at the root.
    // -------------------------------------------------------------------
    const fs::path sweepRoot = g_tmp / L"sweep_root";
    {
        writeFile(sweepRoot / "staging-junk" / "a", "x");
        writeFile(sweepRoot / "old-junk" / "cache" / "b", "y"); // no info
        writeFile(sweepRoot / "stray.txt", "z");
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(sweepRoot), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("backupCache over residue failed", 100);
        if (r.recovered)
            return fail("recovery flagged without parked backup", 101);
        for (const auto& e : fs::directory_iterator(sweepRoot)) {
            const std::wstring n = e.path().filename().wstring();
            if (n != L"cache" && n != L"backup.info")
                return fail("residue survived the sweep", 102);
        }
        std::fprintf(stdout, "    PASS: group 10 (residue sweep)\n");
    }

    // -------------------------------------------------------------------
    // Group 11: complete parked old-* + no visible backup -> recovery.
    //           Simulates a crash right after the park step.
    // -------------------------------------------------------------------
    {
        fs::create_directories(sweepRoot / "old-1");
        fs::rename(sweepRoot / "cache", sweepRoot / "old-1" / "cache");
        fs::rename(sweepRoot / "backup.info",
                   sweepRoot / "old-1" / "backup.info");
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(sweepRoot), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("backupCache after simulated crash failed", 110);
        if (!r.recovered)
            return fail("parked backup not recovered", 111);
        tsdev::BackupInfo i = tsdev::readBackupInfo(w(sweepRoot));
        if (!i.exists || !i.complete)
            return fail("no complete visible backup after recovery", 112);
        for (const auto& e : fs::directory_iterator(sweepRoot)) {
            const std::wstring n = e.path().filename().wstring();
            if (n != L"cache" && n != L"backup.info")
                return fail("residue after recovery", 113);
        }
        std::fprintf(stdout, "    PASS: group 11 (old-* recovery)\n");
    }

    // -------------------------------------------------------------------
    // Group 12: promote-move failure -> rollback restores prior pair.
    //   Seam (documented, least invasive): the progress callback fires while
    //   staging-<pid>\cache already exists. The test opens that directory
    //   WITHOUT FILE_SHARE_DELETE, which makes the MoveFileW of
    //   staging cache\ -> cache\ fail (ACCESS_DENIED) on NTFS, exercising
    //   the swap-rollback path without any engine changes.
    // -------------------------------------------------------------------
    const fs::path swapRoot = g_tmp / L"swap_root";
    {
        tsdev::CacheOpResult r0 =
            tsdev::backupCache(w(src), w(swapRoot), countProgress);
        if (r0.status != tsdev::CacheOpStatus::Ok)
            return fail("seed backup for swap test failed", 120);
        const std::string infoBefore =
            readFile(swapRoot / "backup.info");
        const std::string cacheBefore =
            readFile(swapRoot / "cache" / "a.bin");

        writeFile(src / "a.bin", "NEWDATANEWDATA"); // 12 bytes -> total 20
        const fs::path stagingCacheDir =
            swapRoot /
            ("staging-" + std::to_string(GetCurrentProcessId())) / "cache";
        HANDLE hold = INVALID_HANDLE_VALUE;
        auto inject = [&](uint64_t d, uint64_t t, const std::wstring& f) {
            countProgress(d, t, f);
            if (hold == INVALID_HANDLE_VALUE) {
                hold = CreateFileW(stagingCacheDir.wstring().c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            }
        };
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(swapRoot), inject);
        CloseHandle(hold);
        writeFile(src / "a.bin", "AAAAAAAA"); // restore source fixture
        if (r.status != tsdev::CacheOpStatus::Error)
            return fail("promote failure did not return Error", 121);
        // Prior complete pair must be fully restored.
        if (readFile(swapRoot / "backup.info") != infoBefore)
            return fail("rollback did not restore prior backup.info", 122);
        if (readFile(swapRoot / "cache" / "a.bin") != cacheBefore)
            return fail("rollback did not restore prior cache", 123);
        // Rollback succeeded fully -> old-* must be deleted.
        for (const auto& e : fs::directory_iterator(swapRoot)) {
            const std::wstring n = e.path().filename().wstring();
            if (n.rfind(L"old-", 0) == 0)
                return fail("old-* survived a fully restored rollback", 124);
        }
        std::fprintf(stdout,
                     "    PASS: group 12 (swap rollback restores pair)\n");
    }

    // -------------------------------------------------------------------
    // Group 13: park failure -> Error, prior backup untouched.
    //   Seam: a file inside cache\ held open with share-mode 0 blocks the
    //   NTFS rename of its parent dir, failing the park move.
    // -------------------------------------------------------------------
    {
        const fs::path parkRoot = g_tmp / L"park_root";
        tsdev::CacheOpResult r0 =
            tsdev::backupCache(w(src), w(parkRoot), countProgress);
        if (r0.status != tsdev::CacheOpStatus::Ok)
            return fail("seed backup for park test failed", 130);
        const std::string before =
            readFile(parkRoot / "backup.info");
        const std::wstring locked =
            (parkRoot / "cache" / "a.bin").wstring();
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not hold cache file for park injection", 131);
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(parkRoot), countProgress);
        CloseHandle(h);
        if (r.status != tsdev::CacheOpStatus::Error)
            return fail("park failure did not return Error", 132);
        if (readFile(parkRoot / "backup.info") != before)
            return fail("prior backup.info mutated by failed park", 133);
        tsdev::BackupInfo i = tsdev::readBackupInfo(w(parkRoot));
        if (!i.exists || !i.complete)
            return fail("prior backup not intact after failed park", 134);
        std::fprintf(stdout, "    PASS: group 13 (park failure)\n");
    }

    // -------------------------------------------------------------------
    // Group 14: restore copy failure -> honest "previous cache restored".
    //   Seam: the progress callback fires during the copy; the test plants a
    //   DIRECTORY at the next file's destination path so CopyFileExW fails.
    //   File order is taken from enumerateTree() — no guessing.
    // -------------------------------------------------------------------
    {
        tsdev::CacheEnumerateResult ben =
            tsdev::enumerateTree(w(backupRoot / "cache"));
        if (!ben.ok || ben.files.size() < 2)
            return fail("could not enumerate backup for injection", 140);
        const fs::path dest3 = g_tmp / L"dest3";
        writeFile(dest3 / "keep.bin", "K");
        bool armed = true;
        auto inject = [&](uint64_t d, uint64_t t, const std::wstring& f) {
            countProgress(d, t, f);
            if (armed) {
                armed = false;
                fs::create_directories(dest3 /
                    fs::path(ben.files[1].relativePath));
            }
        };
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest3), inject);
        if (r.status != tsdev::CacheOpStatus::Error)
            return fail("injected copy failure did not return Error", 141);
        if (r.message.find("previous cache restored") == std::string::npos)
            return fail("rollback message not honest (restored)", 142);
        if (readFile(dest3 / "keep.bin") != "K")
            return fail("previous cache not rolled back", 143);
        if (fs::exists(dest3 / "a.bin"))
            return fail("partial copy leaked into restored cache", 144);
        std::fprintf(stdout,
                     "    PASS: group 14 (restore rollback restored)\n");
    }

    // -------------------------------------------------------------------
    // Group 15: forced rollback failure -> parked dir kept, message says so.
    //   Same injection plus a held no-DELETE handle on a planted blocker dir
    //   inside the destination, so remove_all(cacheDir) cannot finish and the
    //   non-clobbering move back fails.
    // -------------------------------------------------------------------
    {
        tsdev::CacheEnumerateResult ben =
            tsdev::enumerateTree(w(backupRoot / "cache"));
        const fs::path dest4 = g_tmp / L"dest4";
        writeFile(dest4 / "keep.bin", "K");
        bool armed = true;
        HANDLE bh = INVALID_HANDLE_VALUE;
        auto inject = [&](uint64_t d, uint64_t t, const std::wstring& f) {
            countProgress(d, t, f);
            if (armed) {
                armed = false;
                fs::create_directories(dest4 /
                    fs::path(ben.files[1].relativePath));
                const fs::path blocker = dest4 / "blocker";
                fs::create_directories(blocker);
                bh = CreateFileW(blocker.wstring().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            }
        };
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest4), inject);
        CloseHandle(bh);
        if (r.status != tsdev::CacheOpStatus::Error)
            return fail("forced rollback failure did not return Error", 150);
        if (r.message.find("previous cache left at") == std::string::npos)
            return fail("rollback message not honest (left at)", 151);
        bool parkedFound = false;
        for (const auto& e : fs::directory_iterator(g_tmp)) {
            if (e.path().filename().wstring().rfind(
                    L"dest4.tsdev-restore-", 0) == 0)
                parkedFound = true;
        }
        if (!parkedFound)
            return fail("parked dir deleted despite failed rollback", 152);
        fs::remove_all(dest4); // blocked earlier; handle now closed
        for (const auto& e : fs::directory_iterator(g_tmp)) {
            if (e.path().filename().wstring().rfind(
                    L"dest4.tsdev-restore-", 0) == 0)
                fs::remove_all(e.path());
        }
        std::fprintf(stdout,
                     "    PASS: group 15 (rollback failure honest)\n");
    }

    // -------------------------------------------------------------------
    // Group 16: restore-side parked sweep — destination absent, a complete
    //           parked Cache.tsdev-restore-* is recovered first.
    // -------------------------------------------------------------------
    {
        const fs::path dest6 = g_tmp / L"dest6";
        writeFile(g_tmp / L"dest6.tsdev-restore-1" / "z.bin", "Z");
        fs::create_directories(g_tmp / L"dest6.tsdev-restore-0"); // empty junk
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest6), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("restore with parked residue failed", 160);
        if (!r.recovered)
            return fail("parked dir not recovered", 161);
        if (readFile(dest6 / "a.bin") != "AAAAAAAA")
            return fail("restored content wrong after recovery", 162);
        for (const auto& e : fs::directory_iterator(g_tmp)) {
            if (e.path().filename().wstring().rfind(
                    L"dest6.tsdev-restore-", 0) == 0)
                return fail("parked residue survived restore", 163);
        }
        std::fprintf(stdout,
                     "    PASS: group 16 (restore parked sweep)\n");
    }

    // -------------------------------------------------------------------
    // Group 17: integrity mismatch — a file deleted from the staged backup
    //           must fail restore with Error and leave destination intact.
    // -------------------------------------------------------------------
    {
        const fs::path dest7 = g_tmp / L"dest7";
        writeFile(dest7 / "keep.bin", "K");
        fs::remove(backupRoot / "cache" / "sub" / "deep" / "c.bin");
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(backupRoot), w(dest7), countProgress);
        if (r.status != tsdev::CacheOpStatus::Error)
            return fail("incomplete backup did not return Error", 170);
        if (r.message.find("backup is incomplete: expected") ==
            std::string::npos)
            return fail("missing 'backup is incomplete' message", 171);
        if (readFile(dest7 / "keep.bin") != "K" ||
            fs::exists(dest7 / "a.bin"))
            return fail("destination mutated by rejected restore", 172);
        std::fprintf(stdout, "    PASS: group 17 (integrity check)\n");
    }

    // -------------------------------------------------------------------
    // Group 18: VssSnapshot::buildShadowPath pure path mapping.
    // -------------------------------------------------------------------
    {
        const std::wstring dev =
            L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy99";
        // Normal nested path: volume root "C:\" stripped, rest grafted on.
        if (tsdev::VssSnapshot::buildShadowPath(
                dev, L"C:\\dir\\sub\\f.bin") !=
            dev + L"\\dir\\sub\\f.bin")
            return fail("buildShadowPath nested mapping wrong", 180);
        // File directly at the volume root (trailing-backslash root strip).
        if (tsdev::VssSnapshot::buildShadowPath(dev, L"C:\\f.bin") !=
            dev + L"\\f.bin")
            return fail("buildShadowPath root-file mapping wrong", 181);
        // Deeper nesting.
        if (tsdev::VssSnapshot::buildShadowPath(
                dev, L"C:\\a\\b\\c\\d\\e.bin") !=
            dev + L"\\a\\b\\c\\d\\e.bin")
            return fail("buildShadowPath deep mapping wrong", 182);
        // Empty member path when the snapshot device is empty.
        tsdev::VssSnapshot empty;
        if (!empty.shadowPath(L"C:\\f.bin").empty())
            return fail("shadowPath on inactive snapshot not empty", 183);
        std::fprintf(stdout, "    PASS: group 18 (shadowPath mapping)\n");
    }

    // -------------------------------------------------------------------
    // Group 19: locked-file integration — deny-all source file copied via
    //           a VSS snapshot (in-proc vssapi, or the WMI Win32_ShadowCopy
    //           fallback on 32-bit builds). If VSS is unavailable (not
    //           elevated / VSS service off) the graceful Error is asserted
    //           and the group SKIPs rather than fails. When it engages, a
    //           WMI shadow-count before/after verifies the snapshot was
    //           released (DeleteInstance on every exit path).
    // -------------------------------------------------------------------
    {
        const int shadowsBefore = wmiShadowCount();
        const fs::path lockSrc = g_tmp / L"vss_source";
        const fs::path lockRoot = g_tmp / L"vss_root";
        writeFile(lockSrc / "free.bin", "FREE");
        writeFile(lockSrc / "locked.bin", "LOCKED-DATA-1234567890");
        HANDLE h = CreateFileW((lockSrc / "locked.bin").wstring().c_str(),
                               GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not deny-all lock vss_source file", 190);
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(lockSrc), w(lockRoot), countProgress);
        CloseHandle(h);
        if (r.status == tsdev::CacheOpStatus::Ok) {
            if (!r.usedVss)
                return fail("locked-file backup succeeded without VSS", 191);
            if (readFile(lockRoot / "cache" / "locked.bin") !=
                "LOCKED-DATA-1234567890")
                return fail("VSS-captured bytes not identical", 192);
            if (readFile(lockRoot / "cache" / "free.bin") != "FREE")
                return fail("non-locked file wrong after VSS backup", 193);
            const std::string info =
                readFile(lockRoot / "backup.info");
            if (info.find("live_source=true") == std::string::npos)
                return fail("live_source=true missing after VSS backup",
                            194);
            // Leak check: the snapshot must be gone after backupCache
            // returned. WMI shadow deletion is synchronous, but poll
            // briefly anyway to stay robust against provider lag.
            if (shadowsBefore >= 0) {
                int after = wmiShadowCount();
                for (int i = 0; i < 50 && after > shadowsBefore; ++i) {
                    Sleep(100);
                    after = wmiShadowCount();
                }
                if (after < 0 || after > shadowsBefore)
                    return fail("VSS shadow leaked after backupCache", 196);
            }
            std::fprintf(stdout,
                         "    PASS: group 19 (VSS locked-file capture, "
                         "shadow released)\n");
        } else {
            if (r.status != tsdev::CacheOpStatus::Error ||
                r.message.find("volume snapshot unavailable") ==
                    std::string::npos ||
                r.message.find(
                    "close TradeStation Desktop and retry") ==
                    std::string::npos)
                return fail("locked-file backup failed without the VSS "
                            "graceful message", 195);
            std::fprintf(stdout,
                         "    SKIP: group 19 (VSS unavailable / not "
                         "elevated): %s\n",
                         r.message.c_str());
        }
    }

    // -------------------------------------------------------------------
    // Group 20: recovery-failed sweep keeps a complete parked old-*.
    //   A crashed swap can leave the only surviving backup under old-*. If
    //   recovery moves fail (here: no-DELETE handles on the parked pair),
    //   the sweep must NOT delete it and the result must note the residue.
    // -------------------------------------------------------------------
    {
        const fs::path rescueRoot = g_tmp / L"rescue_root";
        const fs::path parkedDir = rescueRoot / "old-1";
        writeFile(parkedDir / "cache" / "f.bin", "PARKED");
        writeFile(parkedDir / "backup.info",
                  "format=1\nfiles=1\nbytes=6\ncomplete=true\n");
        // No-DELETE handles make BOTH recovery moves fail, leaving old-1 a
        // still-complete parked backup when the sweep runs.
        HANDLE hc = CreateFileW(
            (parkedDir / "cache").wstring().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        HANDLE hi = CreateFileW(
            (parkedDir / "backup.info").wstring().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hc == INVALID_HANDLE_VALUE || hi == INVALID_HANDLE_VALUE)
            return fail("could not pin old-* pair for sweep test", 200);
        tsdev::CacheOpResult r =
            tsdev::backupCache(w(src), w(rescueRoot), countProgress);
        CloseHandle(hc);
        CloseHandle(hi);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("backup over unrecoverable old-* failed", 201);
        if (r.recovered)
            return fail("recovery flagged despite failed moves", 202);
        if (r.message.find("surviving backup kept at") == std::string::npos)
            return fail("sweep residue note missing from result message",
                        203);
        if (!fs::exists(parkedDir / "backup.info") ||
            readFile(parkedDir / "cache" / "f.bin") != "PARKED")
            return fail("complete parked backup destroyed by sweep", 204);
        std::fprintf(stdout,
                     "    PASS: group 20 (failed recovery keeps old-*)\n");
    }

    // -------------------------------------------------------------------
    // Group 21: a foreign/future backup format must not restore silently —
    //           NotFound with a message naming the format.
    // -------------------------------------------------------------------
    {
        const fs::path fmtRoot = g_tmp / L"fmt_root";
        writeFile(fmtRoot / "cache" / "x.bin", "X");
        writeFile(fmtRoot / "backup.info",
                  "format=2\ncreated=2024-01-01 00:00:00\nfiles=1\nbytes=1\n"
                  "complete=true\n");
        tsdev::BackupInfo i = tsdev::readBackupInfo(w(fmtRoot));
        if (!i.exists || !i.complete || i.format != 2)
            return fail("readBackupInfo did not parse format=2", 210);
        tsdev::CacheOpResult r = tsdev::restoreCache(
            w(fmtRoot), w(g_tmp / L"fmt_dest"), countProgress);
        if (r.status != tsdev::CacheOpStatus::NotFound)
            return fail("format=2 restore did not return NotFound", 211);
        if (r.message.find("unsupported backup format 2") ==
            std::string::npos)
            return fail("format=2 message does not name the format", 212);
        if (fs::exists(g_tmp / L"fmt_dest"))
            return fail("format=2 restore created a destination", 213);
        std::fprintf(stdout,
                     "    PASS: group 21 (unsupported format rejected)\n");
    }

    // -------------------------------------------------------------------
    // Group 22: lock probe semantics — a readonly destination file probes
    //           fine (DELETE access open succeeds), while a share-0-held
    //           file still reports InUse.
    // -------------------------------------------------------------------
    {
        // backupRoot was deliberately torn by group 17 — make a fresh one.
        const fs::path roBkup = g_tmp / L"ro_backup";
        if (tsdev::backupCache(w(src), w(roBkup), countProgress).status !=
            tsdev::CacheOpStatus::Ok)
            return fail("seed backup for readonly probe test failed", 219);
        const fs::path roDest = g_tmp / L"ro_dest";
        const fs::path roFile = roDest / "keep.bin";
        writeFile(roFile, "K");
        if (!SetFileAttributesW(roFile.wstring().c_str(),
                                FILE_ATTRIBUTE_READONLY))
            return fail("could not set readonly on dest file", 220);
        tsdev::CacheOpResult r =
            tsdev::restoreCache(w(roBkup), w(roDest), countProgress);
        if (r.status != tsdev::CacheOpStatus::Ok)
            return fail("restore with readonly dest file not Ok", 221);
        if (fs::exists(roFile))
            return fail("readonly file survived restore", 222);
        // Share-0 handle on a restored file must still report InUse.
        const std::wstring locked = (roDest / "a.bin").wstring();
        HANDLE h = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not lock restored file for probe", 223);
        tsdev::CacheOpResult r2 =
            tsdev::restoreCache(w(roBkup), w(roDest), countProgress);
        CloseHandle(h);
        if (r2.status != tsdev::CacheOpStatus::InUse)
            return fail("locked file not reported InUse", 224);
        // The parked readonly copy resists remove_all; clear attrs so the
        // sandbox cleanup at the end does not leave residue.
        for (const auto& e : fs::directory_iterator(g_tmp)) {
            if (e.path().filename().wstring().rfind(
                    L"ro_dest.tsdev-restore-", 0) == 0) {
                SetFileAttributesW(
                    (e.path() / "keep.bin").wstring().c_str(),
                    FILE_ATTRIBUTE_NORMAL);
                fs::remove_all(e.path());
            }
        }
        std::fprintf(stdout,
                     "    PASS: group 22 (probe: readonly ok, lock InUse)\n");
    }

    // -------------------------------------------------------------------
    // Group 23: queryVolumeFreeSpace — success on an existing path and
    //           ancestor-walk from a nonexistent path.
    // -------------------------------------------------------------------
    {
        uint64_t avail = 0;
        if (!tsdev::queryVolumeFreeSpace(w(g_tmp), avail))
            return fail("queryVolumeFreeSpace(existing) failed", 230);
        if (avail == 0)
            return fail("queryVolumeFreeSpace reported 0 free", 231);
        uint64_t avail2 = 0;
        if (!tsdev::queryVolumeFreeSpace(
                w(g_tmp / L"no_such" / L"deep" / L"path"), avail2))
            return fail("queryVolumeFreeSpace ancestor-walk failed", 232);
        if (avail2 == 0)
            return fail("ancestor-walk reported 0 free", 233);
        std::fprintf(stdout, "    PASS: group 23 (queryVolumeFreeSpace)\n");
    }

    // -------------------------------------------------------------------
    // Group 24: compressTreeInPlace — NTFS in-place compression.
    //   Compressible >64KB file gets FILE_ATTRIBUTE_COMPRESSED, reads back
    //   byte-identical, and its on-disk compressed size shrinks; readonly
    //   and <64KB files are skipped; shouldStop() ends the walk.
    // -------------------------------------------------------------------
    {
        const fs::path compRoot = g_tmp / L"comp_root";
        // 256 KB of a repeated byte: highly compressible under LZNT1.
        const std::string big(256 * 1024, 'A');
        const fs::path bigFile = compRoot / "sub" / "big.bin";
        const fs::path smallFile = compRoot / "small.bin";
        const fs::path roFile = compRoot / "readonly.bin";
        writeFile(bigFile, big);
        writeFile(smallFile, "SMALL");                 // < 64 KB
        writeFile(roFile, big);
        if (!SetFileAttributesW(roFile.wstring().c_str(),
                                FILE_ATTRIBUTE_READONLY))
            return fail("could not set readonly on comp fixture", 240);

        tsdev::CompressStats st{};
        tsdev::compressTreeInPlace(w(compRoot),
                                   [] { return false; }, &st);
        if (st.files != 3)
            return fail("compressTreeInPlace visited != 3 files", 241);
        if (st.compressed != 1 || st.skipped != 2)
            return fail("compressTreeInPlace compressed/skipped wrong",
                        242);
        const DWORD bigAttrs =
            GetFileAttributesW(bigFile.wstring().c_str());
        if (bigAttrs == INVALID_FILE_ATTRIBUTES ||
            !(bigAttrs & FILE_ATTRIBUTE_COMPRESSED))
            return fail("big file lacks FILE_ATTRIBUTE_COMPRESSED", 243);
        if (readFile(bigFile) != big)
            return fail("compressed file content not identical", 244);
        DWORD hi = 0;
        const DWORD lo =
            GetCompressedFileSizeW(bigFile.wstring().c_str(), &hi);
        const uint64_t onDisk =
            (static_cast<uint64_t>(hi) << 32) | lo;
        if (lo == INVALID_FILE_SIZE ||
            onDisk >= static_cast<uint64_t>(big.size()))
            return fail("compressed size not below original", 245);
        if (st.bytesAfter >= st.bytesBefore)
            return fail("stats bytesAfter >= bytesBefore", 246);
        const DWORD smallAttrs =
            GetFileAttributesW(smallFile.wstring().c_str());
        if (smallAttrs == INVALID_FILE_ATTRIBUTES ||
            (smallAttrs & FILE_ATTRIBUTE_COMPRESSED))
            return fail("<64KB file was compressed", 247);
        const DWORD roAttrs =
            GetFileAttributesW(roFile.wstring().c_str());
        if (roAttrs == INVALID_FILE_ATTRIBUTES ||
            (roAttrs & FILE_ATTRIBUTE_COMPRESSED))
            return fail("readonly file was compressed", 248);
        SetFileAttributesW(roFile.wstring().c_str(),
                           FILE_ATTRIBUTE_NORMAL); // allow cleanup

        // shouldStop honored: walk ends before any file is touched.
        const fs::path stopRoot = g_tmp / L"stop_root";
        writeFile(stopRoot / "big.bin", big);
        tsdev::CompressStats st2{};
        tsdev::compressTreeInPlace(w(stopRoot),
                                   [] { return true; }, &st2);
        if (st2.files != 0 || st2.compressed != 0)
            return fail("shouldStop did not abort the walk", 249);

        // startBackupCompression: detached worker actually compresses when
        // the process stays alive long enough (production runs race exit —
        // the unit test holds the process so the path is exercised for
        // real). Poll briefly to tolerate scheduling jitter.
        const fs::path bgRoot = g_tmp / L"bg_root";
        const fs::path bgFile = bgRoot / "big.bin";
        writeFile(bgFile, big);
        tsdev::startBackupCompression(w(bgRoot));
        bool bgDone = false;
        for (int i = 0; i < 100 && !bgDone; ++i) {
            Sleep(50);
            const DWORD a = GetFileAttributesW(bgFile.wstring().c_str());
            bgDone = (a != INVALID_FILE_ATTRIBUTES) &&
                     (a & FILE_ATTRIBUTE_COMPRESSED);
        }
        if (!bgDone)
            return fail("startBackupCompression thread never compressed",
                        250);
        if (readFile(bgFile) != big)
            return fail("background-compressed content not identical", 251);
        std::fprintf(stdout,
            "    PASS: group 24 (NTFS compress, %llu->%llu bytes)\n",
            static_cast<unsigned long long>(st.bytesBefore),
            static_cast<unsigned long long>(st.bytesAfter));
    }

    fs::remove_all(g_tmp);
    return 0;
}

} // namespace

int main() {
    std::fprintf(stdout, "test_cache_backup: running\n");
    int rc = runTests();
    if (rc == 0) {
        std::fprintf(stdout, "test_cache_backup: ALL PASS\n");
    }
    return rc;
}
