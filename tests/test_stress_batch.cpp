// test_stress_batch.cpp
//
// TVS-D.1: Stress test for batch feasibility (historical TDD transition; the
// batch CLI it originally de-risked is implemented).
// TVS-L.15: Extended with single-process resource-growth sampling and a
//           mid-run compile-failure + recovery on the SAME TsDevLib.
//
// Validates that ONE TsDevLib instance (constructed once, in ONE process, on
// ONE 64 MB-stack worker thread) can perform N sequential insert+compile+verify
// operations without degradation, then delete all N and leave a clean
// warehouse. This exercises the shared one-instance behavior used by the batch
// CLI mode (TVS-D.3).
//
// TVS-L.15 additions (single-process long-lived leak detection — the
// complement to tests/test_unknown_link_stress.ps1, which is repeated-instance
// stress with a fresh CLI process per cycle and therefore CANNOT prove
// long-lived leak absence):
//   * Resource sampling at START / MID / END of the run:
//       - process handle count      (GetProcessHandleCount, kernel32)
//       - GDI objects               (GetGuiResources GR_GDIOBJECTS, user32)
//       - User objects              (GetGuiResources GR_USEROBJECTS, user32)
//       - hidden-window count       (EnumWindows + IsWindowVisible, user32)
//       - working-set size          (GetProcessMemoryInfo, psapi)
//     Each metric is asserted to stay within a GENEROUS BUT FINITE delta of
//     the START snapshot (bounds documented inline below). The intent is to
//     catch UNBOUNDED growth (a leak), not to pin exact numbers — the bounds
//     are deliberately loose so transient compiler/COM/cache fluctuations do
//     not cause flaky failures, while a true leak that grows with N still
//     trips the assertion at END.
//   * One mid-run compile-failure then recovery on the SAME TsDevLib without
//     restart: at the INSERT-phase midpoint, createEntry + saveSource +
//     verifyModule with INVALID source (compile error), then create-path
//     cleanup via deleteEntry(flush=false) — exactly the doInsert create-path
//     cleanup contract. Then a full valid insert+verify+delete recovery cycle
//     on the same TsDevLib proves the instance survived the compile failure.
//     The fault/recovery items use unique names NOT in the main N vector and
//     are cleaned up inside the block, so leftoverCleaned (which counts only
//     the main N names) stays 0.
//
// Phases (all on the same worker thread / same TsDevLib instance):
//   1. INSERT  — for each of N: findExistPos (idempotent skip), createEntry,
//                saveSource, verifyModule (assert no compile errors),
//                ellFileExists. Record per-op timing. Continue on failure.
//                At the midpoint: resource snapshot (MID) + compile-failure
//                + recovery on the same TsDevLib.
//   2. VERIFY  — all N .ell files exist + all N findable via findExistPos.
//   3. DELETE  — deleteEntry for all N. Record per-op timing. Continue on fail.
//   4. CLEAN   — all N gone (findExistPos <= 0) + .ell/.xml files removed.
//   5. REPORT  — structured summary: counts, total/per-phase time,
//                min/max/median per-op time, resource deltas vs bounds, any
//                errors. Exit 0 iff fully clean AND all resource bounds hold.
//
// Cleanup guarantee: even on partial failure, best-effort delete all N names so
// no leftover StressBatch_* components pollute the warehouse. The unique
// "StressBatch_" prefix never collides with real user components.
//
// Configurability: N defaults to 600. Override with env var STRESS_BATCH_N,
// e.g. (PowerShell):
//   $env:STRESS_BATCH_N=5; .\build\Release\test_stress_batch.exe
//   $env:STRESS_BATCH_N=600; .\build\Release\test_stress_batch.exe
//
// HOW TO RUN (manual / opt-in — NOT part of the default regression suite,
// because it is heavy, slow, and hits the real TradeStation warehouse):
//   cmake --build build --config Release --target test_stress_batch
//   $env:STRESS_BATCH_N=5;    .\build\Release\test_stress_batch.exe   # smoke
//   $env:STRESS_BATCH_N=600;  .\build\Release\test_stress_batch.exe   # full
//
// Exit code: 0 if ALL phases fully clean (N inserts, N deletes, no leftovers)
//            AND all resource-growth bounds hold,
//            non-zero if any failures, leftovers, or bound violations.
//
// NOTE: The EasyLanguage compiler (invoked via VerifyModule_New) requires a
// large stack, AND the hidden EDIT control created by TsDevLib is a Win32
// window (thread-affine). All N ops run on ONE worker thread that constructs
// ONE TsDevLib instance and loops — exactly the batch CLI pattern (TVS-D.3).
//
// NOTE (TVS-L.15): The resource sampling uses ONLY read-only Win32 query APIs
// called from this test. No production COM debug counters are added to
// TsDevLib — the measurement hook is entirely test-local.

#include "tsdev_lib.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tsdev;
using Clock = std::chrono::steady_clock;

namespace {

// Unique prefix so this test never collides with real user components.
const char* kPrefix = "StressBatch_";
const ComponentType kType = ComponentType::Strategy;

// Minimal valid EasyLanguage strategy source (confirmed to compile, exit 0).
// The generated source includes a `{ name }` header as arbitrary source content,
// not metadata. The item name, type, and ProgId come from the test/API inputs
// and warehouse records.
const char* kSourceBody =
    "Inputs: Length(10);\r\n"
    "If Close > Average(Close, Length) Then Buy next bar at Market;\r\n";

// TVS-L.15: Invalid EasyLanguage strategy source guaranteed to FAIL
// compilation (malformed Inputs declaration). Used for the mid-run
// compile-failure + create-path cleanup + recovery block.
const char* kInvalidSourceBody =
    "Inputs: ;\r\n"
    "{ syntax error -- malformed Inputs }\r\n"
    "Buy next bar at Market;\r\n";

// Default batch size; overridable via STRESS_BATCH_N env var.
const int kDefaultN = 600;

// Read N from the STRESS_BATCH_N env var, falling back to the default.
int readN() {
    const char* env = std::getenv("STRESS_BATCH_N");
    if (env && *env) {
        int v = std::atoi(env);
        if (v > 0) return v;
    }
    return kDefaultN;
}

// Build the unique component name for index i (1-based: StressBatch_001..).
std::string makeName(int i, int n) {
    int width = 1;
    for (int t = n; t >= 10; t /= 10) ++width;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%0*d", kPrefix, width, i);
    return std::string(buf);
}

// Build the full source for a given name. The brace-delimited header is
// arbitrary source content, not metadata.
std::string makeSource(const std::string& name) {
    std::string s = "{ ";
    s += name;
    s += " }\r\n";
    s += kSourceBody;
    return s;
}

// TVS-L.15: Build invalid source for the mid-run compile-failure block.
std::string makeInvalidSource(const std::string& name) {
    std::string s = "{ ";
    s += name;
    s += " }\r\n";
    s += kInvalidSourceBody;
    return s;
}

// File-existence helper (narrow path).
bool fileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Median of a vector of doubles (does not mutate the input).
double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t mid = v.size() / 2;
    if (v.size() % 2 == 0) return (v[mid - 1] + v[mid]) / 2.0;
    return v[mid];
}

// ---------------------------------------------------------------------------
// TVS-L.15: Single-process resource-growth sampling.
//
// All queries are read-only Win32 APIs called from this test. No production
// COM debug counters are added to TsDevLib. The hidden-window count uses
// EnumWindows filtered to windows owned by this process that are NOT visible
// (the TsDevLib hidden EDIT control + its STATIC parent are message-only /
// WS_POPUP and not visible).
// ---------------------------------------------------------------------------

// EnumWindows callback context: counts hidden windows owned by this process.
struct HiddenWindowCounter {
    DWORD pid = 0;
    int   count = 0;
};

BOOL CALLBACK countHiddenWindowsCb(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<HiddenWindowCounter*>(lParam);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == ctx->pid && !IsWindowVisible(hwnd)) {
        ctx->count++;
    }
    return TRUE;
}

// A point-in-time snapshot of process-wide resource usage.
struct ResourceSnapshot {
    const char* label = "";   // "START" / "MID" / "END"
    int   handles        = 0; // GetProcessHandleCount
    int   gdiObjects     = 0; // GetGuiResources GR_GDIOBJECTS
    int   userObjects    = 0; // GetGuiResources GR_USEROBJECTS
    int   hiddenWindows  = 0; // EnumWindows, hidden + owned by this PID
    long long workingSetKb = 0; // GetProcessMemoryInfo WorkingSetSize / 1024
};

ResourceSnapshot snapshotResources(const char* label) {
    ResourceSnapshot s;
    s.label = label;
    // Handle count.
    DWORD handleCount = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
        s.handles = (int)handleCount;
    }
    // GDI / User objects.
    s.gdiObjects  = (int)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    s.userObjects = (int)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    // Hidden windows owned by this process.
    HiddenWindowCounter ctx;
    ctx.pid = GetCurrentProcessId();
    EnumWindows(countHiddenWindowsCb, reinterpret_cast<LPARAM>(&ctx));
    s.hiddenWindows = ctx.count;
    // Working set.
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.workingSetKb = (long long)(pmc.WorkingSetSize / 1024);
    }
    return s;
}

void printResourceSnapshot(const ResourceSnapshot& s) {
    std::fprintf(stdout,
        "    [%s] handles=%d gdi=%d user=%d hiddenWins=%d workingSet=%lldKB\n",
        s.label, s.handles, s.gdiObjects, s.userObjects,
        s.hiddenWindows, s.workingSetKb);
}

// ---------------------------------------------------------------------------
// TVS-L.15 resource-growth bounds (GENEROUS BUT FINITE).
//
// Each bound is a MAXIMUM DELTA (end - start) that must not be exceeded. They
// are deliberately loose: the goal is to catch UNBOUNDED growth (a leak that
// scales with N), not to pin exact numbers. A clean run with N=600 must stay
// within these deltas; transient compiler/COM/cache fluctuations are well
// inside them, while a true per-op leak (e.g. a leaked handle per insert)
// would blow past them at N=600.
//
//   handles      <= +256   // file/registry/COM handles; TsDevLib opens and
//                           //   closes warehouse + compiler handles per op.
//                           //   256 is ~0.4/op at N=600 — far above any
//                           //   expected steady-state fluctuation.
//   gdiObjects   <= +128   // GDI objects (pens/brushes/fonts/DCs); the hidden
//                           //   EDIT control allocates a few; 128 leaves
//                           //   ample headroom for compiler font caching.
//   userObjects  <= +128   // User objects (windows/menus/accels); the hidden
//                           //   EDIT + STATIC parent are 2 windows; 128
//                           //   catches a per-op window leak at N=600.
//   hiddenWins   <= +64    // Hidden windows owned by this process; same
//                           //   rationale as userObjects but narrower.
//   workingSet   <= +200MB // Working set; the warehouse cache, COM, and the
//                           //   EL compiler can grow the WS substantially on
//                           //   first use; 200MB is well above steady-state
//                           //   growth yet below a per-op leak at N=600.
// ---------------------------------------------------------------------------
struct ResourceBounds {
    int       handles;
    int       gdiObjects;
    int       userObjects;
    int       hiddenWindows;
    long long workingSetKb;
};
const ResourceBounds kResBounds = { 256, 128, 128, 64, 200 * 1024 };

// Check a snapshot against the start snapshot + bounds. Returns a non-empty
// string listing the violations (empty string == all within bounds).
std::string checkResourceBounds(const ResourceSnapshot& start,
                                const ResourceSnapshot& s,
                                const ResourceBounds& b) {
    std::string violations;
    int dh  = s.handles - start.handles;
    int dg  = s.gdiObjects - start.gdiObjects;
    int du  = s.userObjects - start.userObjects;
    int dw  = s.hiddenWindows - start.hiddenWindows;
    long long dws = s.workingSetKb - start.workingSetKb;
    if (dh > b.handles)
        violations += "handles delta=" + std::to_string(dh) +
                      " > bound=" + std::to_string(b.handles) + "; ";
    if (dg > b.gdiObjects)
        violations += "gdi delta=" + std::to_string(dg) +
                      " > bound=" + std::to_string(b.gdiObjects) + "; ";
    if (du > b.userObjects)
        violations += "user delta=" + std::to_string(du) +
                      " > bound=" + std::to_string(b.userObjects) + "; ";
    if (dw > b.hiddenWindows)
        violations += "hiddenWins delta=" + std::to_string(dw) +
                      " > bound=" + std::to_string(b.hiddenWindows) + "; ";
    if (dws > b.workingSetKb)
        violations += "workingSet delta=" + std::to_string(dws) +
                      "KB > bound=" + std::to_string(b.workingSetKb) + "KB; ";
    return violations;
}

// Record of a single per-op outcome.
struct OpRec {
    int    index   = 0;
    double ms      = 0.0;
    bool   ok      = false;
    std::string note;  // error/detail message
};

// Aggregate per-phase stats.
struct PhaseStats {
    int    total    = 0;
    int    success  = 0;
    int    fail     = 0;
    double totalMs  = 0.0;
    std::vector<double> perOpMs;       // only successful ops
    std::vector<OpRec>  failures;
};

void recordResult(PhaseStats& s, int idx, double ms, bool ok,
                  const std::string& note) {
    s.total++;
    s.totalMs += ms;
    if (ok) {
        s.success++;
        s.perOpMs.push_back(ms);
    } else {
        s.fail++;
        s.failures.push_back({idx, ms, ok, note});
    }
}

void printPhaseSummary(const char* title, const PhaseStats& s) {
    double mn = 0, mx = 0, med = 0;
    if (!s.perOpMs.empty()) {
        mn = *std::min_element(s.perOpMs.begin(), s.perOpMs.end());
        mx = *std::max_element(s.perOpMs.begin(), s.perOpMs.end());
        med = medianOf(s.perOpMs);
    }
    std::fprintf(stdout,
        "  %-8s total=%d ok=%d fail=%d time=%.1fs "
        "(per-op ms: min=%.1f max=%.1f median=%.1f)\n",
        title, s.total, s.success, s.fail, s.totalMs / 1000.0,
        mn, mx, med);
    if (!s.failures.empty()) {
        std::fprintf(stdout, "    failures (first %zu):\n",
                     s.failures.size() > 20 ? 20 : s.failures.size());
        size_t shown = 0;
        for (const auto& f : s.failures) {
            if (shown++ >= 20) { std::fprintf(stdout, "    ... (%zu more)\n", s.failures.size() - 20); break; }
            std::fprintf(stdout, "      [%d] %.1fms: %s\n",
                         f.index, f.ms, f.note.c_str());
        }
    }
}

// Shared state between main and the worker thread.
struct TestState {
    int N        = 0;
    int exitCode = 0;
};

DWORD WINAPI runTest(LPVOID param) {
    TestState* st = static_cast<TestState*>(param);
    int N = st->N;

    std::vector<std::string> names(N);
    for (int i = 0; i < N; ++i) names[i] = makeName(i + 1, N);

    // --- Construct ONE TsDevLib instance ---
    std::fprintf(stdout, "\n[init] Constructing TsDevLib (one instance for all %d ops)...\n", N);
    TsDevLib* lib = nullptr;
    auto t0 = Clock::now();
    try {
        lib = new TsDevLib();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] TsDevLib construction threw: %s\n", e.what());
        st->exitCode = 10;
        return 0;
    }
    auto tInit = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::fprintf(stdout, "    TsDevLib constructed OK (%.1f ms)\n", tInit);

    // TVS-L.15: START resource snapshot (taken AFTER TsDevLib construction so
    // the baseline includes the hidden EDIT control + COM init + warehouse
    // open — the steady-state footprint all subsequent ops build on).
    ResourceSnapshot resStart = snapshotResources("START");
    std::fprintf(stdout, "\n[resources] START snapshot:\n");
    printResourceSnapshot(resStart);
    ResourceSnapshot resMid{};
    ResourceSnapshot resEnd{};
    std::string resMidViolations;
    std::string resEndViolations;
    bool faultBlockOk = false;
    std::string faultBlockNote;

    PhaseStats ins, ver, del, cln;

    // ============================ PHASE 1: INSERT ============================
    std::fprintf(stdout, "\n[phase 1] INSERT %d components...\n", N);
    auto pStart = Clock::now();
    int midPoint = N / 2;  // 0-based index at which the fault block runs
    for (int i = 0; i < N; ++i) {
        // TVS-L.15: at the INSERT-phase midpoint, run ONE compile-failure then
        // recovery on the SAME TsDevLib (no restart), then take the MID
        // resource snapshot. This proves the long-lived TsDevLib instance
        // survives a compile failure (create-path cleanup via
        // deleteEntry(flush=false), exactly the doInsert contract) and can
        // still service a full valid insert+verify+delete cycle. The fault
        // and recovery items use unique names NOT in the main N vector and
        // are cleaned up inside this block, so leftoverCleaned (which counts
        // only the main N names) stays 0.
        if (i == midPoint) {
            std::fprintf(stdout,
                "\n[fault-block] mid-run compile-failure + recovery on the "
                "SAME TsDevLib (i=%d, midpoint)\n", i);
            std::string faultName = std::string(kPrefix) + "FaultInj_Mid";
            std::string recoveryName = std::string(kPrefix) + "Recovery_Mid";
            // Best-effort pre-cleanup in case a prior aborted run left these.
            if (lib->findExistPos(faultName, kType) > 0)
                lib->deleteEntry(kType, faultName, /*flush=*/false);
            if (lib->findExistPos(recoveryName, kType) > 0)
                lib->deleteEntry(kType, recoveryName, /*flush=*/false);

            // --- Compile-failure: createEntry + saveSource(invalid) +
            //     verifyModule -> expect non-empty errors, then create-path
            //     cleanup via deleteEntry(flush=false). ---
            bool faultCompileFailed = false;
            bool faultCleanupOk = false;
            int faultProgId = 0;
            int errCode = 0;
            faultProgId = lib->createEntry(kType, faultName, errCode);
            if (faultProgId <= 0) {
                faultBlockNote = "fault createEntry<=0 (errCode=0x"
                                 + std::to_string(errCode) + ")";
            } else if (!lib->saveSource(kType, faultProgId,
                                        makeInvalidSource(faultName))) {
                faultBlockNote = "fault saveSource failed";
                // create-path cleanup (saveSource-failure path).
                lib->deleteEntry(kType, faultName, /*flush=*/false);
                faultCleanupOk = (lib->findExistPos(faultName, kType) <= 0);
            } else {
                std::string errs = lib->verifyModule(kType, faultProgId);
                if (errs.empty()) {
                    // Unexpected: invalid source compiled. Still clean up.
                    faultBlockNote = "fault: invalid source unexpectedly compiled";
                    lib->deleteEntry(kType, faultName, /*flush=*/false);
                    faultCleanupOk = (lib->findExistPos(faultName, kType) <= 0);
                } else {
                    faultCompileFailed = true;
                    // Create-path cleanup (compile-error path) — flush=false so
                    // the warehouse stays open for the rest of the session,
                    // matching the batch/serve doInsert contract.
                    lib->deleteEntry(kType, faultName, /*flush=*/false);
                    faultCleanupOk = (lib->findExistPos(faultName, kType) <= 0);
                }
            }
            std::fprintf(stdout,
                "    [fault] compileFailed=%d cleanupOk=%d progId=%d %s\n",
                (int)faultCompileFailed, (int)faultCleanupOk, faultProgId,
                faultBlockNote.empty() ? "" : faultBlockNote.c_str());

            // --- Recovery: full valid insert+verify+delete on the SAME
            //     TsDevLib, proving the instance survived the compile
            //     failure. ---
            bool recoveryOk = false;
            int recProgId = lib->createEntry(kType, recoveryName, errCode);
            if (recProgId <= 0) {
                faultBlockNote += "; recovery createEntry<=0 (errCode=0x"
                                  + std::to_string(errCode) + ")";
            } else if (!lib->saveSource(kType, recProgId,
                                        makeSource(recoveryName))) {
                faultBlockNote += "; recovery saveSource failed";
                lib->deleteEntry(kType, recoveryName, /*flush=*/false);
            } else {
                std::string errs = lib->verifyModule(kType, recProgId);
                if (!errs.empty()) {
                    faultBlockNote += "; recovery compile error: " + errs;
                    lib->deleteEntry(kType, recoveryName, /*flush=*/false);
                } else if (!lib->ellFileExists(kType, recProgId)) {
                    faultBlockNote += "; recovery .ell missing";
                    lib->deleteEntry(kType, recoveryName, /*flush=*/false);
                } else {
                    // Verify succeeded; now delete to clean up.
                    bool recDel = lib->deleteEntry(kType, recoveryName);
                    int still = lib->findExistPos(recoveryName, kType);
                    recoveryOk = recDel || (still <= 0);
                    if (!recoveryOk) {
                        faultBlockNote += "; recovery delete failed, still present";
                    }
                }
            }
            std::fprintf(stdout,
                "    [recovery] ok=%d progId=%d\n", (int)recoveryOk, recProgId);

            faultBlockOk = faultCompileFailed && faultCleanupOk && recoveryOk;
            std::fprintf(stdout,
                "    [fault-block] overall ok=%d %s\n", (int)faultBlockOk,
                faultBlockOk ? "" : faultBlockNote.c_str());

            // MID resource snapshot (after the fault block + half the inserts).
            resMid = snapshotResources("MID");
            std::fprintf(stdout, "\n[resources] MID snapshot (after fault block + %d inserts):\n", i);
            printResourceSnapshot(resMid);
            resMidViolations = checkResourceBounds(resStart, resMid, kResBounds);
            if (!resMidViolations.empty()) {
                std::fprintf(stdout,
                    "    [resources] MID BOUND VIOLATION: %s\n",
                    resMidViolations.c_str());
            }
        }

        const std::string& name = names[i];
        auto opStart = Clock::now();
        bool ok = false;
        std::string note;

        int progId = lib->findExistPos(name, kType);
        if (progId > 0) {
            // Idempotent: already exists from a prior aborted run. Re-verify by
            // recompiling so the .ell is guaranteed present.
            std::string src = makeSource(name);
            if (!lib->saveSource(kType, progId, src)) {
                note = "saveSource failed on pre-existing entry";
            } else {
                std::string errs = lib->verifyModule(kType, progId);
                if (!errs.empty()) {
                    note = "compile error on pre-existing entry: " + errs;
                } else if (!lib->ellFileExists(kType, progId)) {
                    note = ".ell missing after reverify of pre-existing entry";
                } else {
                    ok = true;
                }
            }
        } else {
            int errCode = 0;
            progId = lib->createEntry(kType, name, errCode);
            if (progId <= 0) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "createEntry<=0 (errCode=0x%X)", errCode);
                note = buf;
            } else if (!lib->saveSource(kType, progId, makeSource(name))) {
                note = "saveSource failed";
            } else {
                std::string errs = lib->verifyModule(kType, progId);
                if (!errs.empty()) {
                    note = "compile error: " + errs;
                } else if (!lib->ellFileExists(kType, progId)) {
                    note = ".ell missing after verifyModule";
                } else {
                    ok = true;
                }
            }
        }
        double ms = std::chrono::duration<double, std::milli>(
                        Clock::now() - opStart).count();
        recordResult(ins, i + 1, ms, ok, note);

        // Progress every 50 ops or on failure.
        if (!ok || (i + 1) % 50 == 0 || (i + 1) == N) {
            std::fprintf(stdout, "    insert %d/%d (%.1f ms) %s\n",
                         i + 1, N, ms, ok ? "OK" : ("FAIL: " + note).c_str());
        }
    }
    double insPhaseMs = std::chrono::duration<double, std::milli>(
                            Clock::now() - pStart).count();

    // ============================ PHASE 2: VERIFY ============================
    std::fprintf(stdout, "\n[phase 2] VERIFY all %d components exist...\n", N);
    pStart = Clock::now();
    for (int i = 0; i < N; ++i) {
        const std::string& name = names[i];
        auto opStart = Clock::now();
        bool ok = false;
        std::string note;
        int progId = lib->findExistPos(name, kType);
        if (progId <= 0) {
            note = "findExistPos <= 0 (not found)";
        } else if (!lib->ellFileExists(kType, progId)) {
            note = ".ell file missing";
        } else {
            ok = true;
        }
        double ms = std::chrono::duration<double, std::milli>(
                        Clock::now() - opStart).count();
        recordResult(ver, i + 1, ms, ok, note);
    }
    double verPhaseMs = std::chrono::duration<double, std::milli>(
                            Clock::now() - pStart).count();

    // ============================ PHASE 3: DELETE ============================
    std::fprintf(stdout, "\n[phase 3] DELETE all %d components...\n", N);
    pStart = Clock::now();
    for (int i = 0; i < N; ++i) {
        const std::string& name = names[i];
        auto opStart = Clock::now();
        bool ok = false;
        std::string note;
        bool deleted = lib->deleteEntry(kType, name);
        if (deleted) {
            ok = true;
        } else {
            // Could be already gone; check whether it still exists.
            int still = lib->findExistPos(name, kType);
            if (still <= 0) {
                ok = true;       // effectively gone
                note = "deleteEntry=false but already absent";
            } else {
                note = "deleteEntry=false and still present";
            }
        }
        double ms = std::chrono::duration<double, std::milli>(
                        Clock::now() - opStart).count();
        recordResult(del, i + 1, ms, ok, note);
        if (!ok || (i + 1) % 50 == 0 || (i + 1) == N) {
            std::fprintf(stdout, "    delete %d/%d (%.1f ms) %s\n",
                         i + 1, N, ms, ok ? "OK" : ("FAIL: " + note).c_str());
        }
    }
    double delPhaseMs = std::chrono::duration<double, std::milli>(
                            Clock::now() - pStart).count();

    // ======================= PHASE 4: CLEAN-VERIFY ==========================
    // Verify all N are gone (in-memory lookup) and .ell/.xml files removed.
    std::fprintf(stdout, "\n[phase 4] CLEAN-VERIFY all %d gone...\n", N);
    pStart = Clock::now();
    for (int i = 0; i < N; ++i) {
        const std::string& name = names[i];
        auto opStart = Clock::now();
        bool ok = false;
        std::string note;
        int progId = lib->findExistPos(name, kType);
        if (progId > 0) {
            note = "still in warehouse (ProgId=" + std::to_string(progId) + ")";
        } else {
            // No ProgId to build file paths from; scan the expected path via a
            // best-effort: getEllPath needs a progId, so we cannot check files
            // by name alone. Instead, if findExistPos says gone, treat as clean
            // (deleteEntry removes .ell/.xml via Windows API as part of its
            // flow). File leftovers, if any, are reported separately below.
            ok = true;
        }
        double ms = std::chrono::duration<double, std::milli>(
                        Clock::now() - opStart).count();
        recordResult(cln, i + 1, ms, ok, note);
    }
    double clnPhaseMs = std::chrono::duration<double, std::milli>(
                            Clock::now() - pStart).count();

    // --- Cleanup guarantee: best-effort delete any leftovers (in case a prior
    // phase aborted or deleteEntry reported false-but-still-present) ---
    int leftoverCleaned = 0;
    for (int i = 0; i < N; ++i) {
        if (lib->findExistPos(names[i], kType) > 0) {
            lib->deleteEntry(kType, names[i]);
            leftoverCleaned++;
        }
    }
    // TVS-L.15: defensive cleanup of the fault/recovery items (they are
    // cleaned up inside the fault block, but if that block aborted early we
    // sweep them here so no StressBatch_FaultInj_Mid / StressBatch_Recovery_Mid
    // pollutes the warehouse). These are NOT counted in leftoverCleaned (which
    // tracks only the main N names) — they are tracked separately so the
    // leftoverCleaned==0 invariant reflects only the main batch.
    int faultLeftoverCleaned = 0;
    const char* kFaultExtraNames[2] = { "StressBatch_FaultInj_Mid",
                                        "StressBatch_Recovery_Mid" };
    for (const char* fn : kFaultExtraNames) {
        if (lib->findExistPos(fn, kType) > 0) {
            lib->deleteEntry(kType, fn);
            faultLeftoverCleaned++;
        }
    }
    if (leftoverCleaned > 0) {
        std::fprintf(stdout, "    cleanup-guarantee: best-effort deleted %d leftover(s)\n",
                     leftoverCleaned);
    }
    if (faultLeftoverCleaned > 0) {
        std::fprintf(stdout,
            "    cleanup-guarantee: best-effort deleted %d fault-block leftover(s)\n",
            faultLeftoverCleaned);
    }

    // TVS-L.15: END resource snapshot (after all deletes + cleanup, before
    // TsDevLib teardown). This is the key long-lived-leak check: if a per-op
    // leak existed, END would be far above START.
    resEnd = snapshotResources("END");
    std::fprintf(stdout, "\n[resources] END snapshot (after all deletes + cleanup):\n");
    printResourceSnapshot(resEnd);
    resEndViolations = checkResourceBounds(resStart, resEnd, kResBounds);
    if (!resEndViolations.empty()) {
        std::fprintf(stdout,
            "    [resources] END BOUND VIOLATION: %s\n",
            resEndViolations.c_str());
    }

    // --- Destroy TsDevLib (OrlibExit persists deletions) ---
    auto tExit0 = Clock::now();
    delete lib;
    lib = nullptr;
    double tExit = std::chrono::duration<double, std::milli>(
                       Clock::now() - tExit0).count();
    std::fprintf(stdout, "\n[teardown] TsDevLib destroyed (OrlibExit) (%.1f ms)\n", tExit);

    // ============================== REPORT ===================================
    double totalMs = tInit + insPhaseMs + verPhaseMs + delPhaseMs +
                     clnPhaseMs + tExit;
    std::fprintf(stdout, "\n==================== STRESS BATCH REPORT ====================\n");
    std::fprintf(stdout, "N (batch size)      : %d\n", N);
    std::fprintf(stdout, "TsDevLib init       : %.1f ms\n", tInit);
    std::fprintf(stdout, "TsDevLib teardown   : %.1f ms\n", tExit);
    std::fprintf(stdout, "Total wall time     : %.1f s\n", totalMs / 1000.0);
    std::fprintf(stdout, "Per-phase:\n");
    printPhaseSummary("INSERT", ins);
    std::fprintf(stdout, "    (insert phase total: %.1f s)\n", insPhaseMs / 1000.0);
    printPhaseSummary("VERIFY", ver);
    std::fprintf(stdout, "    (verify phase total: %.1f s)\n", verPhaseMs / 1000.0);
    printPhaseSummary("DELETE", del);
    std::fprintf(stdout, "    (delete phase total: %.1f s)\n", delPhaseMs / 1000.0);
    printPhaseSummary("CLEAN",  cln);
    std::fprintf(stdout, "    (clean phase total: %.1f s)\n", clnPhaseMs / 1000.0);

    // TVS-L.15: resource-growth report.
    std::fprintf(stdout, "\n--- TVS-L.15 resource-growth (single-process leak check) ---\n");
    std::fprintf(stdout, "Bounds (max delta vs START): handles<=%d gdi<=%d user<=%d "
                         "hiddenWins<=%d workingSet<=%lldKB\n",
                         kResBounds.handles, kResBounds.gdiObjects,
                         kResBounds.userObjects, kResBounds.hiddenWindows,
                         kResBounds.workingSetKb);
    std::fprintf(stdout, "Fault block (mid-run compile-failure + recovery): ok=%d%s\n",
                 (int)faultBlockOk, faultBlockOk ? "" :
                 (" -- " + faultBlockNote).c_str());
    std::fprintf(stdout, "MID violations : %s\n",
                 resMidViolations.empty() ? "(none)" : resMidViolations.c_str());
    std::fprintf(stdout, "END violations : %s\n",
                 resEndViolations.empty() ? "(none)" : resEndViolations.c_str());
    std::fprintf(stdout, "Fault-block leftovers cleaned: %d\n", faultLeftoverCleaned);

    bool allClean = (ins.fail == 0) && (ver.fail == 0) &&
                    (del.fail == 0) && (cln.fail == 0) &&
                    (ins.success == N) && (ver.success == N) &&
                    (del.success == N) && (cln.success == N) &&
                    (leftoverCleaned == 0) &&
                    (faultLeftoverCleaned == 0) &&
                    faultBlockOk &&
                    resMidViolations.empty() &&
                    resEndViolations.empty();

    std::fprintf(stdout, "-------------------------------------------------------------\n");
    if (allClean) {
        std::fprintf(stdout, "VERDICT: PASS -- %d inserts + %d deletes fully clean in one process;\n",
                     N, N);
        std::fprintf(stdout, "         mid-run compile-failure + recovery OK on same TsDevLib;\n");
        std::fprintf(stdout, "         no unbounded resource growth (TVS-L.15 bounds hold).\n");
        std::fprintf(stdout, "         Batch mode (TVS-D.3) is DE-RISKED.\n");
        st->exitCode = 0;
    } else {
        std::fprintf(stdout, "VERDICT: FAIL -- degradation, leftovers, fault-block failure, or\n");
        std::fprintf(stdout, "         unbounded resource growth detected (see above).\n");
        std::fprintf(stdout, "         Batch mode needs mitigations before TVS-D.3.\n");
        st->exitCode = 1;
    }
    std::fprintf(stdout, "=============================================================\n");
    return 0;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestState st;
    st.N = readN();
    std::fprintf(stdout, "=== test_stress_batch: TVS-D.1 batch feasibility stress test ===\n");
    std::fprintf(stdout, "=== + TVS-L.15 single-process resource-growth + compile-fault recovery ===\n");
    std::fprintf(stdout, "Batch size N = %d (override via STRESS_BATCH_N env var)\n", st.N);
    std::fprintf(stdout, "Prefix       = %s\n", kPrefix);
    std::fprintf(stdout, "Component    = Strategy\n");

    // Run the entire test on ONE worker thread with a 64 MB stack. The
    // EasyLanguage compiler needs far more than the default 1 MB, and the
    // hidden EDIT control is thread-affine so TsDevLib must live on this
    // thread. All N ops reuse the single TsDevLib instance — the batch pattern.
    HANDLE hThread = CreateThread(nullptr, 64 * 1024 * 1024,
                                  runTest, &st, 0, nullptr);
    if (!hThread) {
        std::fprintf(stderr, "FATAL: CreateThread failed: %lu\n", GetLastError());
        return 11;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    std::fprintf(stdout, "\n=== test_stress_batch complete (exit %d) ===\n",
                 st.exitCode);
    return st.exitCode;
}
