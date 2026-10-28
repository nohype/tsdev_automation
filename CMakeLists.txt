# CMakeLists.txt -- tsdev_automation
#
# Minimal C++ build setup. ALL builds target the Win32 (x86 / 32-bit) platform
# because the project loads 32-bit orlib20.dll from the TradeStation 9.5 install.
#
# Configure (from a shell with the VS environment sourced, or any shell):
#   cmake -S . -B build -A Win32
#   cmake --build build --config Release
#
# The -A Win32 flag forces the 32-bit toolchain regardless of host architecture.

cmake_minimum_required(VERSION 3.20)

project(tsdev_automation LANGUAGES CXX)

# C++17 standard, no compiler extensions.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Guard: refuse to configure for anything other than 32-bit (Win32).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "tsdev_automation MUST be built as 32-bit (x86) to load orlib20.dll. "
        "Configure with -A Win32 (Visual Studio generator) or source vcvarsall.bat x86 "
        "before configuring with Ninja. Detected sizeof(void*)=${CMAKE_SIZEOF_VOID_P}.")
endif()

# --- Workstream G: distribution output directories ---------------------------
# NOTE: These directories live in the SOURCE TREE (${CMAKE_SOURCE_DIR}), NOT the
# build tree. This is INTENTIONAL: the goal is a root-level gitignored dist/
# containing the assembled bundle (exe + Python wrapper) that can be zipped and
# distributed directly from the repo root. Do NOT "fix" this to write into the
# build directory -- that would break the distribution workflow. dist/,
# dist_diag/, and artifacts/ are gitignored via the root .gitignore.
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist/tsdev_automation)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/artifacts)
# Diagnostic analog of dist/ (parallel directory, same source-tree location):
# assembled by the tsdev_cli_diag POST_BUILD block below.
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist_diag)

# --- Python interpreter for build-time codegen (sigcodegen.py) ---------------
find_package(Python COMPONENTS Interpreter REQUIRED)

# --- Generated include directory (build-tree) --------------------------------
# signatures.gen.h is generated here by sigcodegen.py at build time.
set(GEN_INCLUDE_DIR ${CMAKE_BINARY_DIR}/gen)

# --- Signature codegen custom command + target (TVS-E.1 step 4) ---------------
# Runs tools/sigcodegen.py to generate signatures.gen.h from committed JSON
# source-of-truth files. The generated header replaces tmp/re_artifacts/b1_signatures.h.
add_custom_command(
    OUTPUT ${GEN_INCLUDE_DIR}/signatures.gen.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_INCLUDE_DIR}
    COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/sigcodegen.py
            --manifest ${CMAKE_SOURCE_DIR}/signatures/version_manifest.json
            --sigdir ${CMAKE_SOURCE_DIR}/signatures
            --output ${GEN_INCLUDE_DIR}/signatures.gen.h
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/sigcodegen.py
            ${CMAKE_SOURCE_DIR}/signatures/version_manifest.json
            ${CMAKE_SOURCE_DIR}/signatures/v9.50.01.3344.json
    COMMENT "Generating signatures.gen.h"
    VERBATIM)

add_custom_target(siggen DEPENDS ${GEN_INCLUDE_DIR}/signatures.gen.h)

# --- Shared orlib20 wrapper library -----------------------------------------
# Reusable engine for the CLI and tests. Encapsulates the confirmed orlib20
# operation set (init, lookup, create, save, compile/verify, close).
# TVS-L.2: com_apartment_guard.cpp implements the RAII STA COM apartment
# guard that TsDevLib constructs as its first effective action. Ole32 is
# linked PRIVATELY for the real CoInitializeEx / CoUninitialize referenced
# by the default ComApi in com_apartment_guard.cpp.
# TVS-L.9: src/persistence_integrity.cpp implements the SHARED
# persistence-integrity validator (validatePersistenceIntegrity) used by the
# CLI's doInsert/doModify post-verify check and by the
# test_persistence_integrity unit test. bcrypt (for its SHA-256 of the
# .ell/.xml artifacts) reaches it transitively via the PUBLIC
# orlib_resolver -> bcrypt chain.
# TVS-L.10: src/create_path_cleanup.cpp implements the pure
# requiresCreatePathCleanup() ownership decision used by the CLI's doInsert
# (the SINGLE create-path partial-entry cleanup owner) and by the
# test_create_path_cleanup unit test. It is side-effect free (no orlib20 /
# warehouse / filesystem access), so it adds no link dependencies.
# TVS-L.12: src/delete_disk_cleanup.cpp implements the pure
# deleteDiskCleanupErrorFailsResult() decision used by TsDevLib::deleteEntry
# (step 5, after each DeleteFileA on the .ell/.xml artifacts) and by the
# test_delete_disk_cleanup unit test. It is side-effect free (no orlib20 /
# warehouse / filesystem access), so it adds no link dependencies.
# TVS-1b: src/whserver_health.cpp (WHServer pid discovery + VirtualQueryEx
# VAS-headroom measurement, pure Win32 kernel32 + toolhelp — NO new .lib
# deps) ships in tsdev_lib so the production tsdev_cli's runBatch VAS guard
# and every unit test that #includes tsdev_cli.cpp resolve it transitively.
# The diag build already receives the same TU via tsdev_diag (below).
# TVS-3: src/dispatch_watchdog.cpp implements the production in-flight
# dispatch marker + bounded-wait watchdog (TerminateProcess(15) fire path).
# It is pure Win32 (kernel32 + toolhelp + whserver_health) — NO new .lib
# deps — and is compiled in BOTH production and diag builds (the marker is
# worthless if compiled out). Also compiled directly into
# test_dispatch_watchdog below.
add_library(tsdev_lib STATIC
    src/tsdev_lib.cpp
    src/com_apartment_guard.cpp
    src/persistence_integrity.cpp
    src/create_path_cleanup.cpp
    src/delete_disk_cleanup.cpp
    src/whserver_health.cpp
    src/dispatch_watchdog.cpp
    src/whserver_census.cpp
    src/whserver_recycle.cpp
    src/ts_install.cpp
    src/cache_backup.cpp
    src/vss_snapshot.cpp)
target_include_directories(tsdev_lib PUBLIC src)
target_link_libraries(tsdev_lib PUBLIC orlib_resolver)
target_link_libraries(tsdev_lib PRIVATE Ole32 OleAut32 shell32 vssapi wbemuuid)
# TVS (CLI log levels): the former `target_compile_definitions(tsdev_lib
# PRIVATE TSDEV_DEBUG)` is REMOVED — TSDEV_DBG diagnostics are now gated at
# runtime by the process-global log level (tsdev_log_set_level, default
# Warning) instead of at compile time. The CLI raises the level via the
# repeatable -v/--verbose flag (see src/tsdev_lib.h).

# --- §7.1 diagnostic build module (report 2026-09-02_v0.1.0 §7.1) ------------
# tsdev_diag: the diagnostic-only module (file sink + watchdog). Compiled ONLY
# with TSDEV_DIAG and linked ONLY into the diagnostic targets; the production
# build neither compiles the call-site macros to code nor links this library.
# dbghelp: StackWalk64 / SymGetModuleBase64 / MiniDumpWriteDump (watchdog).
# user32: EnumWindows / IsHungAppWindow (WHServer liveness samples).
# whserver_health.cpp (WHServer pid discovery + VirtualQueryEx VAS-headroom
# measurement) is pure Win32 (kernel32 + toolhelp only — NO new .lib deps) and
# is also compiled directly into the test_whserver_health unit test below.
add_library(tsdev_diag STATIC
    src/diag_log.cpp
    src/diag_watchdog.cpp
    src/whserver_health.cpp)
target_include_directories(tsdev_diag PUBLIC src)
target_compile_definitions(tsdev_diag PRIVATE TSDEV_DIAG)
target_link_libraries(tsdev_diag PUBLIC dbghelp user32)

# tsdev_lib_diag: the SAME sources as tsdev_lib compiled with TSDEV_DIAG so
# the §7.1 call-site instrumentation is active. Linked only by tsdev_cli_diag;
# the production tsdev_lib (above) stays untouched and behavior-identical.
add_library(tsdev_lib_diag STATIC
    src/tsdev_lib.cpp
    src/com_apartment_guard.cpp
    src/persistence_integrity.cpp
    src/create_path_cleanup.cpp
    src/delete_disk_cleanup.cpp
    src/dispatch_watchdog.cpp
    src/whserver_census.cpp
    src/whserver_recycle.cpp
    src/ts_install.cpp
    src/cache_backup.cpp
    src/vss_snapshot.cpp)
target_include_directories(tsdev_lib_diag PUBLIC src)
target_link_libraries(tsdev_lib_diag PUBLIC orlib_resolver tsdev_diag)
target_link_libraries(tsdev_lib_diag PRIVATE Ole32 OleAut32 shell32 vssapi wbemuuid)
target_compile_definitions(tsdev_lib_diag PRIVATE TSDEV_DIAG)

# --- INSERT integration test (historical TDD red-green) --------------------------------
add_executable(test_insert tests/test_insert.cpp)
target_link_libraries(test_insert tsdev_lib)

# --- DELETE integration test (historical TDD red-green) --------------------------------
add_executable(test_delete tests/test_delete.cpp)
target_link_libraries(test_delete tsdev_lib)

# --- TradingApp DELETE integration test (TVS-C.5, historical TDD red-green) ------------
# Validates the TradingApp delete operation end-to-end on a 64 MB-stack worker
# thread (compiler + thread-affine hidden EDIT control require it).
add_executable(test_delete_tradingapp tests/test_delete_tradingapp.cpp)
target_link_libraries(test_delete_tradingapp tsdev_lib)

# --- Quick check if deletions persist across restarts -----------------------
add_executable(test_delete_check tests/test_delete_check.cpp)
target_link_libraries(test_delete_check tsdev_lib)

# --- DELETE diagnostic -------------------------------------------------------
add_executable(test_delete_diag tests/test_delete_diag.cpp)
target_link_libraries(test_delete_diag tsdev_lib)

# --- TS install path discovery test (TVS-B.7) -------------------------------
add_executable(test_path_discovery tests/test_path_discovery.cpp)
target_link_libraries(test_path_discovery tsdev_lib)

# --- MODIFY integration test (historical TDD red-green) --------------------------------
add_executable(test_modify tests/test_modify.cpp)
target_link_libraries(test_modify tsdev_lib)

# --- Finalization lifecycle regression test (TVS-L.3) -----------------------
# Repurposed from test_persistence_validation.cpp (TVS-B.1.5). The persistence
# patch (applyPersistencePatch / validatePersistencePatchBytes / FUN_10011230)
# was removed in TVS-L.3, so the old byte-validation test is obsolete. This
# behavior-based regression verifies the TsDevLib constructor/destructor
# lifecycle runs cleanly without the persistence patch (constructor succeeds ->
# 11 thread-check patches applied + OrlibInit; destructor restores patches +
# OrlibExit + CoUninitialize), and that the apply/restore cycle is repeatable.
# It does NOT call any removed persistence API, resolve FUN_10011230 by RVA,
# or duplicate a private test-only signature. Requires TS (orlib20.dll).
add_executable(test_finalization_lifecycle tests/test_finalization_lifecycle.cpp)
target_link_libraries(test_finalization_lifecycle tsdev_lib)

# --- No hardcoded RVA constants test (AC1 historical TDD red-green) ---------------------
# Verifies that tsdev_lib.cpp contains zero hardcoded RVA hex constants.
# Compiles against the generated header signatures.gen.h: the forbidden set is
# derived from it (JSON -> generated header -> test), per docs/archive/ARCHITECTURE_REDESIGN.md
# §7.4 Option A. The generated arrays are constexpr (header-only), so no library
# is linked. add_dependencies ensures the header exists before compilation.
add_executable(test_no_hardcoded_rvas tests/test_no_hardcoded_rvas.cpp)
target_include_directories(test_no_hardcoded_rvas PRIVATE ${GEN_INCLUDE_DIR} src)
add_dependencies(test_no_hardcoded_rvas siggen)

# --- TradingApp ComponentType + decomposition unit test (TVS-C.3) ------------
# Pure unit test for the TradingApp enum entry and getCategory/getSubType
# helpers. No orlib20 dependency; runs on a 64 MB-stack worker thread to match
# the project's test harness convention.
add_executable(test_tradingapp_type tests/test_tradingapp_type.cpp)
target_link_libraries(test_tradingapp_type tsdev_lib)

# --- ShowMe ComponentType + decomposition unit test (TVS-C.8) -----------------
# Pure unit test for the ShowMe enum entry and getCategory/getSubType helpers.
# No orlib20 dependency; runs on a 64 MB-stack worker thread to match the
# project's test harness convention.
add_executable(test_showme_type tests/test_showme_type.cpp)
target_link_libraries(test_showme_type tsdev_lib)

# --- Elevation guard unit test (TVS-K.1) -------------------------------------
# Pure unit test for the isElevated() helper and the errorCodeString(11) entry
# used by the elevation guard in main(). isElevated() and errorCodeString()
# live inside an anonymous namespace in src/tsdev_cli.cpp (internal linkage),
# so the test #includes the .cpp directly (same TU) to reach them. This pulls
# in tsdev_cli.cpp's main(); the test defines TSDEV_CLI_TEST to exclude it
# (main() is wrapped in #ifndef TSDEV_CLI_TEST). The test needs the third_party
# include dir (nlohmann/json) because the included tsdev_cli.cpp uses it, and
# links tsdev_lib for the TsDevLib symbols referenced by the included CLI code.
# It is a PURE UNIT TEST (no orlib20 access), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_elevation_guard tests/test_elevation_guard.cpp)
target_include_directories(test_elevation_guard PRIVATE third_party)
target_link_libraries(test_elevation_guard tsdev_lib)
add_dependencies(test_elevation_guard siggen)

# --- VEH crash handler unit test (TDD GREEN) ----------------------------------
# Pure unit test for the vehCrashHandler() helper added to src/tsdev_cli.cpp.
# vehCrashHandler() lives inside an anonymous namespace in src/tsdev_cli.cpp
# (internal linkage), so the test #includes the .cpp directly (same TU) to
# reach it. This pulls in tsdev_cli.cpp's main(); the test #defines
# TSDEV_CLI_TEST to exclude it (main() is wrapped in #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access), so it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
#
# The test has TWO modes: normal mode (no args, pure unit test wired into the
# regular test runners) and crash mode (--crash arg, deliberate access
# violation). The crash mode is NOT run by the unit test runners — it is run
# by the standalone PowerShell wrapper tests/test_crash_handler.ps1, which
# also verifies that no TSCrashReport.exe / WerFault.exe spawns.
add_executable(test_crash_handler tests/test_crash_handler.cpp)
target_include_directories(test_crash_handler PRIVATE third_party)
target_link_libraries(test_crash_handler tsdev_lib)
add_dependencies(test_crash_handler siggen)

# --- COM apartment guard unit test (TVS-L.2, TDD GREEN) -----------------------
# Pure unit test for the ComApartmentGuard RAII STA COM apartment guard declared
# in src/com_apartment_guard.h. The guard is exercised exclusively through its
# injectable ComApi seam (mock CoInitializeEx / CoUninitialize function
# pointers), so the test NEVER calls the real COM functions and does NOT touch
# the test process's real COM apartment state. It is a PURE UNIT TEST (no
# orlib20 access, no TsDevLib construction), so it is NOT in TSDEV_ADMIN_TARGETS
# and runs under -NoTs.
#
# Sources: tests/test_com_apartment_guard.cpp + src/com_apartment_guard.cpp.
# Ole32 is linked because com_apartment_guard.cpp's default ComApi takes the
# address of the real CoInitializeEx / CoUninitialize, which creates symbol
# references the linker must satisfy even though the test never calls them
# (it injects mock function pointers). The mock path is what executes at
# runtime; Ole32 is only needed to resolve the address-of references.
add_executable(test_com_apartment_guard
    tests/test_com_apartment_guard.cpp
    src/com_apartment_guard.cpp)
target_include_directories(test_com_apartment_guard PRIVATE src)
target_link_libraries(test_com_apartment_guard PRIVATE Ole32)

# --- TsDevLib COM-init injection seam unit test (TVS-L.13, TDD GREEN) ----------
# Pure unit test for the PRIVATE/test-only construction seam that lets a unit
# test drive TsDevLib initialization with injected COM HRESULTs (S_OK,
# S_FALSE, RPC_E_CHANGED_MODE, generic E_FAIL, and a simulated later-ctor
# exception after COM success) WITHOUT leaking COM configuration into the
# public user API.
#
# Seam surface (IMPLEMENTED — GREEN):
#   - src/tsdev_lib.h forward-declares `struct TsDevLibTestConfig;` and adds a
#     PRIVATE ctor TsDevLib::TsDevLib(const wchar_t*, const TsDevLibTestConfig&)
#     + a friend declaration for the test-only factory. No ComApi / <objbase.h>
#     is included by the public header, so no COM config leaks into the public
#     user API.
#   - src/tsdev_lib_test_hooks.h (INTERNAL, test-only, NOT a public header)
#     defines TsDevLibTestConfig { ComApi comApi; bool throwAfterCom; } and
#     declares tsdev::makeTsDevLibForTest(...).
#   - The private ctor and the factory ARE defined in src/tsdev_lib.cpp
#     (GREEN). The test references makeTsDevLibForTest and links cleanly
#     against tsdev_lib, so the test executable builds, links, and runs the
#     assertions — the GREEN signal.
#
# The test injects mock CoInitializeEx / CoUninitialize function pointers
# (the real COM functions are NEVER called by this test), never loads
# orlib20.dll, and never touches the TradeStation installation. It is a PURE
# UNIT TEST (no TSDev Library access at runtime), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
#
# It links tsdev_lib so the factory + private ctor (defined in
# src/tsdev_lib.cpp) resolve with no CMake change. The static-archive linker
# pulls in tsdev_lib.cpp because the test references makeTsDevLibForTest
# (defined there); the other tsdev_lib object files are discarded because the
# test references no symbols from them, so no orlib20 / bcrypt dependency is
# dragged into this pure unit test. Ole32 is linked because
# com_apartment_guard.h's default ComApi takes the address of the real
# CoInitializeEx / CoUninitialize, creating symbol references the linker must
# satisfy even though the test injects mocks and never calls the real
# functions.
add_executable(test_tsdev_com_init_seam
    tests/test_tsdev_com_init_seam.cpp)
target_include_directories(test_tsdev_com_init_seam PRIVATE src)
target_link_libraries(test_tsdev_com_init_seam PRIVATE tsdev_lib Ole32)

# --- Persistence-integrity validator unit test (TVS-L.9, GREEN) --------------
# Pure unit test for the SHARED persistence-integrity validator that classifies
# the cross-store (.ell vs .xml) consistency of a modify/upsert operation. The
# validator is declared in src/persistence_integrity.h and implemented in
# src/persistence_integrity.cpp. The test includes the header, calls
# tsdev::validatePersistenceIntegrity(), and links the implementation through
# tsdev_lib.
#
# The test does NOT load orlib20.dll, does NOT touch the TradeStation
# installation, does NOT construct TsDevLib, and does NOT need a 64 MB stack.
# It builds in-memory fixture bytes for the .ell and .xml artifacts, writes them
# to temp files under %TEMP%, and calls the validator. Three fixture scenarios:
#   (a) VerifiedRefresh  — new GUID present once (mixed-endian) in ELL, ELL+XML
#                          hashes changed.
#   (b) FaultA_StaleEll  — stale ELL (hash unchanged, new GUID absent) + XML
#                          GUID changed (Fault A from RE_L8_STALE_WRITE_FAULT_A).
#   (c) NoOpAllowed      — source-equal proven first; unchanged ELL+XML hashes
#                          accepted as a no-op, NOT a verified refresh.
# bcrypt is linked for the test's own SHA-256 of fixture bytes (pre-op hashes).
# Because tsdev_lib is a static archive, the linker pulls in ONLY the
# persistence_integrity.cpp object file (the only one defining a symbol the
# test references) — tsdev_lib.cpp / com_apartment_guard.cpp are NOT pulled
# in, so the test stays a pure unit test with no orlib20 / TsDevLib
# dependency at runtime. bcrypt is also transitive via tsdev_lib ->
# orlib_resolver -> bcrypt, but kept explicit for the test's own helper.
# This is a PURE UNIT TEST (no TSDev Library access), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_persistence_integrity tests/test_persistence_integrity.cpp)
target_include_directories(test_persistence_integrity PRIVATE src)
target_link_libraries(test_persistence_integrity PRIVATE bcrypt tsdev_lib)

# --- Create-path cleanup ownership unit test (TVS-L.10, GREEN) ---------------
# Pure unit test for the SINGLE ownership decision that covers create-path
# partial-entry cleanup after a successful createEntry for EVERY failure stage
# (saveSource failure AND verifyModule early-return / SEH / OOM / compile
# errors). The owner is DECLARED in src/create_path_cleanup.h and IMPLEMENTED
# in src/create_path_cleanup.cpp (TVS-L.10 GREEN: requiresCreatePathCleanup
# is a pure function of (progId, stage) — true for every stage when progId >
# 0, false for every stage when progId <= 0).
#
# Why this test exists (the defect it targets):
#   Before TVS-L.10, after createEntry succeeded (ProgId > 0, partial
#   warehouse entry in the in-memory BST), cleanup was NOT owned by one
#   local path:
#     * saveSource failure (doInsert exit 7): NO cleanup — partial entry
#       orphaned.
#     * verifyModule early-return / SEH / OOM: cleanup only incidentally via
#       the !errors.empty() branch INSIDE verifyModule (written for the
#       compile-error case, TD-001), not at the create-path level.
#   The single owner implemented here is the fix: for progId > 0 it MUST
#   require cleanup for every listed stage; for progId <= 0 it MUST NOT (no
#   partial entry was created). The decision is a pure function of
#   (progId, stage) with no orlib20 / warehouse / filesystem access, so this
#   is a PURE UNIT TEST (no TSDev Library access). It is NOT in
#   TSDEV_ADMIN_TARGETS and runs under -NoTs.
#
# The test links tsdev_lib so the linker pulls in create_path_cleanup.cpp
# (the only object defining requiresCreatePathCleanup). The static-library
# linker discards the other tsdev_lib object files (tsdev_lib.cpp /
# com_apartment_guard.cpp / persistence_integrity.cpp) because the test
# references no symbols from them, so no orlib20 / Ole32 / bcrypt dependency
# is dragged into this pure unit test.
add_executable(test_create_path_cleanup tests/test_create_path_cleanup.cpp)
target_include_directories(test_create_path_cleanup PRIVATE src)
target_link_libraries(test_create_path_cleanup PRIVATE tsdev_lib)

# --- Delete-path disk-cleanup failure decision unit test (TVS-L.12, GREEN) ---
# Pure unit test for the SINGLE decision that classifies whether a DeleteFileA
# failure on a delete-path disk artifact (.ell / .xml) must FAIL the overall
# deleteEntry Result, instead of the prior behavior where every non-
# ERROR_FILE_NOT_FOUND DeleteFileA failure was silently demoted to a stderr
# WARNING while deleteEntry returned true (success). The decision is DECLARED
# in src/delete_disk_cleanup.h and IMPLEMENTED in src/delete_disk_cleanup.cpp
# (TVS-L.12 GREEN).
#
# Why this test exists (the defect it targets):
#   deleteEntry step 5 deletes elmodule_<whType>_<progId>.ell and .xml via
#   DeleteFileA. On failure it only emitted a stderr WARNING for any error
#   other than ERROR_FILE_NOT_FOUND, then returned true UNCONDITIONALLY. So a
#   real removal failure (e.g. ERROR_ACCESS_DENIED = 5) was treated as success
#   and a stale .ell/.xml was left on disk. The decision implemented in the
#   GREEN step is the fix: ERROR_FILE_NOT_FOUND (and ERROR_SUCCESS) MUST NOT
#   fail the delete Result; ERROR_ACCESS_DENIED (and any other nonzero error)
#   MUST fail it - for BOTH the .ell and the .xml artifact. The decision is a
#   pure function of (lastError, kind) with no orlib20 / warehouse / filesystem
#   access, so this is a PURE UNIT TEST (no TSDev Library access). It is NOT
#   in TSDEV_ADMIN_TARGETS and runs under -NoTs.
#
# Historical TDD chronology: the RED step declared
# deleteDiskCleanupErrorFailsResult() in the header with no .cpp, so the test
# executable FAILED TO LINK (unresolved external) — that link failure was the
# RED signal. The GREEN step adds src/delete_disk_cleanup.cpp to tsdev_lib
# (above), so the symbol now resolves and the test links and runs; the
# assertions below pin the contract.
add_executable(test_delete_disk_cleanup tests/test_delete_disk_cleanup.cpp)
target_include_directories(test_delete_disk_cleanup PRIVATE src)
target_link_libraries(test_delete_disk_cleanup PRIVATE tsdev_lib)

# --- Warehouse-close status unit test ----------------------------------------
# Pure unit test for the SEH-protected explicit close/flush call used by
# TsDevLib::closeWarehouse(). The test injects a no-op and an SEH-raising
# function through the internal test-only seam; it does not construct TsDevLib,
# load orlib20.dll, or touch the TradeStation installation. It is not an
# administrator/TS-dependent test and runs under -NoTs.
add_executable(test_close_warehouse tests/test_close_warehouse.cpp)
target_include_directories(test_close_warehouse PRIVATE src)
target_link_libraries(test_close_warehouse PRIVATE tsdev_lib)

# --- Diagnostic file-sink unit test (§7.1 diag build, TDD) --------------------
# Pure unit test for the §7.1 diagnostic file sink (src/diag_log.h/.cpp):
# per-write flush (record present in the file immediately after the log call,
# before any close), atomic interleaving under concurrent writers from
# multiple threads, and the TSDEV_DIAG_DIR directory override. The sink and
# the test are compiled with TSDEV_DIAG (the sink is a no-op without it).
# It is a PURE UNIT TEST (plain Win32 file I/O, no orlib20 access), so it is
# NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_diag_log
    tests/test_diag_log.cpp
    src/diag_log.cpp)
target_include_directories(test_diag_log PRIVATE src)
target_compile_definitions(test_diag_log PRIVATE TSDEV_DIAG)

# --- WHServer VAS-headroom unit test ------------------------------------------
# Pure unit test for src/whserver_health.cpp (Toolhelp32 WHServer pid
# discovery + VirtualQueryEx VAS-free measurement). The .cpp is compiled
# directly into the test (same pattern as test_diag_log above) so no library
# link is needed; the TU is pure Win32 (kernel32 + toolhelp only, NO new .lib
# deps). Read-only process inspection only — self, an invalid pid, and a
# spawned cmd.exe child; WHServer itself is never required to run. Runs under
# -NoTs.
add_executable(test_whserver_health
    tests/test_whserver_health.cpp
    src/whserver_health.cpp)
target_include_directories(test_whserver_health PRIVATE src)

# --- WHServer census gate unit test (R1 census core) ---------------------------
# Pure unit test for src/whserver_census.cpp: decideCensus() is exercised over
# scripted CensusPoll streams (every contract branch — sustained-zero Pass,
# Indeterminate on listed-pid open/rpm errors, re-enumerated absent arm,
# appearing-pid window reset, failed-snapshot inconclusiveness, multi-pid);
# readWhLockSample() runs a REAL OpenProcess+ReadProcessMemory against the
# test process's own statics (zero TS contact). src/whserver_health.cpp is
# compiled in for the findWhServerPids snapshotOk variant. The .cpps are
# compiled directly into the test (same pattern as test_whserver_health) —
# pure Win32, NO new .lib deps; WHServer is never required to run. -NoTs.
add_executable(test_whserver_census
    tests/test_whserver_census.cpp
    src/whserver_census.cpp
    src/whserver_health.cpp)
target_include_directories(test_whserver_census PRIVATE src)

# --- WHServer recycle engine unit test (R3 engine skeleton) --------------------
# Pure unit test for src/whserver_recycle.cpp: runWhserverRecycle (design §2
# steps 1-6) is driven by injected fakes — scripted census/death poll streams
# through the IRecycleSampler seam and scripted results through the
# IRecycleAttach seam — covering stage ordering (close -> census -> attach ->
# verify -> release) and fail-closed coverage at every stage boundary
# (quiesce / close_failed -> terminateSelf / census lock+indeterminate /
# attach_failed / forceshutdown_failed / zombie / inert / absent arm /
# multi-pid / verify bound). The production ComRecycleAttach is compiled in
# and exercised ONLY on the empty-MyWork refuse path, which must fail before
# any COM call. dispatch_watchdog.cpp is compiled in for the in-flight
# marker (DispatchInFlightGuard inside ComRecycleAttach) and
# findCrashReportParentPids (used by WhRecycleSampler::deathPoll).
# Ole32/OleAut32 resolve the raw-COM attach TU — never invoked under -NoTs.
add_executable(test_whserver_recycle
    tests/test_whserver_recycle.cpp
    src/whserver_recycle.cpp
    src/whserver_census.cpp
    src/whserver_health.cpp
    src/dispatch_watchdog.cpp)
target_include_directories(test_whserver_recycle PRIVATE src)
target_link_libraries(test_whserver_recycle PRIVATE Ole32 OleAut32)

# --- Dispatch watchdog unit test (TVS-3, TDD GREEN) ----------------------------
# Pure unit test for src/dispatch_watchdog.cpp: in-flight marker
# enter/leave/snapshot semantics, DispatchInFlightGuard RAII on exception
# paths, and every branch of the pure fire decision decideDispatchFire()
# (timeout / whserver_crashed / whserver_gone / disabled / not-in-flight).
# The TerminateProcess(15) side effect is NOT unit-testable by design — the
# decision is pure; the fire path is covered by manual/E2E verification.
# src/whserver_health.cpp is compiled in for findWhServerPids (referenced by
# the watchdog TU); src/dispatch_watchdog.cpp is compiled directly so the
# test never links tsdev_lib. PURE UNIT TEST (no orlib20, no TsDevLib), so
# it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
# The exe also has a manual "--fire" mode (like test_crash_handler --crash):
# it raises a real in-flight marker + real 1 s watchdog and dies by
# TerminateProcess(15) — used by manual E2E, never by the test runners.
add_executable(test_dispatch_watchdog
    tests/test_dispatch_watchdog.cpp
    src/dispatch_watchdog.cpp
    src/whserver_health.cpp)
target_include_directories(test_dispatch_watchdog PRIVATE src)

# --- Cache backup unit test ---------------------------------------------------
# Pure unit test for src/cache_backup.cpp (Program\Cache backup/restore
# engine: enumerate, staged backup + swap, backup.info round-trip,
# transactional restore, lock probe, Toolhelp32 TS-process snapshot). The .cpp
# is compiled directly into the test (same pattern as test_whserver_health) so
# no library link is needed; the TU is pure Win32 + std::filesystem and all
# paths point at %TEMP%. PURE UNIT TEST (no orlib20, no TsDevLib), so it is
# NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_cache_backup
    tests/test_cache_backup.cpp
    src/cache_backup.cpp
    src/vss_snapshot.cpp)
target_include_directories(test_cache_backup PRIVATE src)
target_link_libraries(test_cache_backup PRIVATE shell32 ole32 oleaut32 vssapi wbemuuid)

# --- JSON serialize unit test (TVS-K.2, GREEN) -------------------------------
# Pure unit test for the implemented cliResultToJson() helper extracted from
# emitOutput()'s JSON branch. cliResultToJson() lives inside an anonymous
# namespace in src/tsdev_cli.cpp (internal linkage), so the test #includes the
# .cpp directly (same TU) to reach it. This pulls in tsdev_cli.cpp's main(); the
# test #defines TSDEV_CLI_TEST to exclude it (main() is wrapped in
# #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access), so it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_json_serialize tests/test_json_serialize.cpp)
target_include_directories(test_json_serialize PRIVATE third_party)
target_link_libraries(test_json_serialize tsdev_lib)
add_dependencies(test_json_serialize siggen)

# --- Log-level unit test (TVS: CLI log levels, GREEN) ------------------------
# Pure unit test for the runtime log-level gate: tsdev_log_set_level() /
# tsdev_log_get_level() / tsdev_log_message() (exported by src/tsdev_lib.h,
# implemented in src/tsdev_lib.cpp) and the CLI's file-local
# LogLevelFromVerbosityCount() mapping helper (-v occurrence count -> level,
# clamped at Debug). LogLevelFromVerbosityCount() lives inside an anonymous
# namespace in src/tsdev_cli.cpp (internal linkage), so the test #includes the
# .cpp directly (same TU) and #defines TSDEV_CLI_TEST to exclude its main().
# The test needs the third_party include dir (nlohmann/json via the included
# tsdev_cli.cpp) and links tsdev_lib for the log-level symbols. It captures
# stderr via the CRT _dup/_dup2 redirection idiom to assert level gating. It
# is a PURE UNIT TEST (no orlib20 access), so it is NOT in TSDEV_ADMIN_TARGETS
# and runs under -NoTs.
#
# The same test also covers the debug log-file sink
# (tsdev_log_open_file/close_file in src/tsdev_lib.cpp): the file records the
# full stream at every level independent of the console level and of
# tsdev_log_set_sink swaps (--pbar), one line per record, and the file-local
# resolveLogFilePath() --log-file/TSDEV_LOG_FILE precedence helper.
add_executable(test_log_levels tests/test_log_levels.cpp)
target_include_directories(test_log_levels PRIVATE third_party)
target_link_libraries(test_log_levels tsdev_lib)
add_dependencies(test_log_levels siggen)

# --- Serve request dispatch unit test (TVS-K.3, GREEN) ------------------------
# Pure unit test for the implemented parseServeRequest() helper — the JSON
# request -> ServeRequest parser used by the serve/broker layer.
# parseServeRequest() and the ServeRequest struct live inside an anonymous
# namespace in src/tsdev_cli.cpp (internal linkage), so the test #includes the
# .cpp directly (same TU) to reach them. This pulls in tsdev_cli.cpp's main();
# the test #defines TSDEV_CLI_TEST to exclude it (main() is wrapped in
# #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access, no TsDevLib construction — parseServeRequest is pure logic), so it is
# NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs. dispatchServeRequest() is
# NOT unit-tested here (it needs a real TsDevLib; validated via K.5/K.6a E2E).
add_executable(test_serve_dispatch tests/test_serve_dispatch.cpp)
target_include_directories(test_serve_dispatch PRIVATE third_party)
target_link_libraries(test_serve_dispatch tsdev_lib)
add_dependencies(test_serve_dispatch siggen)

# --- Serve framing helpers unit test (TVS-K.4, GREEN) -------------------------
# Pure unit test for the implemented writeFramed()/readFramed() length-prefixed
# I/O helpers used by the serve/broker layer. The helpers live inside an
# anonymous namespace in src/tsdev_cli.cpp (internal linkage), so the test
# #includes the .cpp directly (same TU) to reach them. This pulls in
# tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST to exclude it
# (main() is wrapped in #ifndef TSDEV_CLI_TEST). The test needs the third_party
# include dir (nlohmann/json) because the included tsdev_cli.cpp uses it, and
# links tsdev_lib for the TsDevLib symbols referenced by the included CLI code.
# It is a PURE UNIT TEST (no orlib20 access, no TsDevLib construction — the
# framing helpers are pure Windows pipe I/O), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs. The framing helpers use Windows
# HANDLEs (from CreatePipe), not FILE* or std::istream. The full pipe server
# (cmdServe/runServe) is validated via E2E in K.6a, NOT here.
add_executable(test_serve_framing tests/test_serve_framing.cpp)
target_include_directories(test_serve_framing PRIVATE third_party)
target_link_libraries(test_serve_framing tsdev_lib)
add_dependencies(test_serve_framing siggen)

# --- Batch progress renderers + aggregate exit unit test (TVS A) --------------
# Pure unit test for the batch-progress helpers in src/tsdev_cli.cpp:
# batchItemStartedLine / batchItemCompletedLine / batchCompletedLine (human +
# JSON single-line event renderers) and batchAggregateExitCode. The helpers
# live inside the anonymous namespace in src/tsdev_cli.cpp (internal linkage),
# so the test #includes the .cpp directly (same TU) and #defines TSDEV_CLI_TEST
# to exclude its main() — the same mechanism used by test_json_serialize /
# test_serve_dispatch. The test needs the third_party include dir
# (nlohmann/json) because the included tsdev_cli.cpp uses it, and links
# tsdev_lib for the TsDevLib symbols referenced by the included CLI code.
# It is a PURE UNIT TEST (no orlib20 access), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_batch_progress tests/test_batch_progress.cpp)
target_include_directories(test_batch_progress PRIVATE third_party)
target_link_libraries(test_batch_progress tsdev_lib)
add_dependencies(test_batch_progress siggen)

# --- --pbar progress-bar renderer unit test -----------------------------------
# Pure unit test for the pbar renderer core in src/tsdev_cli.cpp: pbarLine /
# pbarEraseSeq / pbarDrawSeq (PURE line/erase/draw byte builders) and the
# PbarRenderer emitter (start/itemStarted/itemCompleted/noteFailure/
# eventLine/finish). The helpers live inside the anonymous namespace in
# src/tsdev_cli.cpp (internal linkage), so the test #includes the .cpp
# directly (same TU) and #defines TSDEV_CLI_TEST to exclude its main() — the
# same mechanism used by test_batch_progress. The test needs the third_party
# include dir (nlohmann/json) because the included tsdev_cli.cpp uses it, and
# links tsdev_lib for the TsDevLib symbols referenced by the included CLI
# code. It is a PURE UNIT TEST (no orlib20 access — the renderer's stderr
# writes are captured via a dup2'd temp file), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_pbar tests/test_pbar.cpp)
target_include_directories(test_pbar PRIVATE third_party)
target_link_libraries(test_pbar tsdev_lib)
add_dependencies(test_pbar siggen)

# --- Cache-phase (--backup-cache/--restore-cache) helper unit test ----------
# Pure unit test for the cache-phase helpers in src/tsdev_cli.cpp:
# parseCacheBackupPolicy (flag value validation), cacheElapsedFormat /
# cacheProgressLine / cacheSpinnerLine / cachePipeProgressLine (PURE line
# builders used by CacheProgressRenderer), and formatBytesHuman. The helpers
# live inside the anonymous namespace in src/tsdev_cli.cpp (internal
# linkage), so the test #includes the .cpp directly (same TU) and #defines
# TSDEV_CLI_TEST to exclude its main() — the same mechanism used by
# test_pbar. The test needs the third_party include dir (nlohmann/json)
# because the included tsdev_cli.cpp uses it, and links tsdev_lib for the
# TsDevLib/cache_backup/ts_install symbols referenced by the included CLI
# code. It is a PURE UNIT TEST (no orlib20 access, no filesystem mutation —
# only the pure builders are exercised), so it is NOT in TSDEV_ADMIN_TARGETS
# and runs under -NoTs.
add_executable(test_cache_cli_units tests/test_cache_cli_units.cpp)
target_include_directories(test_cache_cli_units PRIVATE third_party)
target_link_libraries(test_cache_cli_units tsdev_lib)
add_dependencies(test_cache_cli_units siggen)

# --- --recycle-whserver flag surface unit test (R2 flag plumbing) -------------
# Pure unit test for the --recycle-whserver helpers in src/tsdev_cli.cpp:
# isRecycleWhserverSubcommand (the §6.1 batch-*/serve set),
# validateRecycleWhserver (the §6.1 usage-error refusals),
# envPresenceEnabled (TSDEV_RECYCLE_WHSERVER presence semantics), and
# emitRecycleWhserverWarning + kRecycleWhserverWarning (the §6.3 verbatim
# warning, emitted once per process lifetime — stderr captured via a dup2'd
# tmpfile like test_pbar check 16). The helpers live inside the anonymous
# namespace in src/tsdev_cli.cpp (internal linkage), so the test #includes
# the .cpp directly (same TU) and #defines TSDEV_CLI_TEST to exclude its
# main() — the same mechanism used by test_pbar. The test needs the
# third_party include dir (nlohmann/json) because the included tsdev_cli.cpp
# uses it, and links tsdev_lib for the TsDevLib symbols referenced by the
# included CLI code. It is a PURE UNIT TEST (no orlib20 access, no TS
# contact — only pure helpers + an env-var probe), so it is NOT in
# TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_recycle_whserver_flag tests/test_recycle_whserver_flag.cpp)
target_include_directories(test_recycle_whserver_flag PRIVATE third_party)
target_link_libraries(test_recycle_whserver_flag tsdev_lib)
add_dependencies(test_recycle_whserver_flag siggen)

# --- R4 batch resume contract unit test ---------------------------------------
# Pure unit test for the §4.1 resume contract in src/tsdev_cli.cpp:
# whserverRecycleEventLine full field set (JSON + human),
# writeResumeManifest <-> parseManifest round-trip (needsSource + delete
# ops, tail slicing, pre-validation-failure reproduction), the recycle
# not-run marker shape, and the aborted batch_completed summary. Same
# include-the-.cpp mechanism as test_recycle_whserver_flag (TSDEV_CLI_TEST
# excludes main()); scratch files live under %TEMP% only. PURE UNIT TEST —
# no orlib20, no TS contact — so it is NOT in TSDEV_ADMIN_TARGETS and runs
# under -NoTs.
add_executable(test_recycle_resume tests/test_recycle_resume.cpp)
target_include_directories(test_recycle_resume PRIVATE third_party)
target_link_libraries(test_recycle_resume tsdev_lib)
add_dependencies(test_recycle_resume siggen)

# --- Pre-op hash classification unit test -------------------------------------
# Pure unit test for the sha256FileCli() helper and its Sha256FileResult /
# Sha256FileStatus (Hashed/Absent/Error) classification. The helper lives inside
# an anonymous namespace in src/tsdev_cli.cpp (internal linkage), so the test
# #includes the .cpp directly (same TU) to reach it. This pulls in
# tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST to exclude it.
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access, no TsDevLib construction — sha256FileCli is pure Win32+bcrypt file
# I/O), so it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
add_executable(test_preop_hash_classification tests/test_preop_hash_classification.cpp)
target_include_directories(test_preop_hash_classification PRIVATE third_party)
target_link_libraries(test_preop_hash_classification tsdev_lib)
add_dependencies(test_preop_hash_classification siggen)

# --- TradingApp INSERT integration test (TVS-C.4) ----------------------------
# End-to-end TDD test for the TradingApp INSERT flow: createEntry, saveSource,
# verifyModule, .ell persistence + RNAM magic check, and self-cleaning delete.
# Runs on a 64 MB-stack worker thread (same pattern as test_insert).
add_executable(test_insert_tradingapp tests/test_insert_tradingapp.cpp)
target_link_libraries(test_insert_tradingapp tsdev_lib)

# --- TradingApp MODIFY integration test (TVS-C.6) ----------------------------
# End-to-end TDD test for the TradingApp MODIFY flow: INSERT a test entry,
# modifyEntry (findExistPos + saveSource + verifyModule on existing ProgId),
# verify ProgId preserved + .ell timestamp changed, negative + compile-error
# tests, and self-cleaning delete. Runs on a 64 MB-stack worker thread.
add_executable(test_modify_tradingapp tests/test_modify_tradingapp.cpp)
target_link_libraries(test_modify_tradingapp tsdev_lib)

# --- EXISTS integration test (TVS-F.1) ---------------------------------------
# Validates the library-level existence-query primitive (findExistPos) used by
# the implemented `exists` CLI subcommand. Inserts a probe strategy, queries
# findExistPos for the existing name (expects ProgId > 0) and for a nonexistent
# name (expects 0), then cleans up. Runs on a 64 MB-stack worker thread (same
# pattern as test_insert).
add_executable(test_exists tests/test_exists.cpp)
target_link_libraries(test_exists tsdev_lib)

# --- ENUMERATE integration test (TVS-F.5) ------------------------------------
# Validates the implemented library-level case-insensitive existence-query
# primitive (enumerateRecords) used by the `exists --case-insensitive` CLI
# mode. Inserts a probe strategy, queries enumerateRecords with the name in
# UPPERCASE (expects exists=true, verbatim stored name, ProgId > 0) and for a
# nonexistent name (expects exists=false, empty stored name, ProgId == 0), then
# cleans up. Runs on a 64 MB-stack worker thread (same pattern as test_exists).
add_executable(test_enumerate tests/test_enumerate.cpp)
target_link_libraries(test_enumerate tsdev_lib)

# --- UPSERT integration test (TVS-F.3) ---------------------------------------
# Validates the library-level primitives composed by the implemented `upsert`
# CLI subcommand (findExistPos + createEntry + saveSource + verifyModule +
# modifyEntry + deleteEntry). Inserts a probe strategy, verifies the upsert
# composition (insert then modify-in-place), asserts the KEY upsert invariant
# (ProgId preserved after modify), then cleans up. Runs on a 64 MB-stack worker
# thread (same pattern as test_exists).
add_executable(test_upsert tests/test_upsert.cpp)
target_link_libraries(test_upsert tsdev_lib)

# --- BATCH UPSERT integration test (TVS-I.1) ---------------------------------
# Validates the library-level composition used by the implemented
# `batch-upsert` CLI subcommand: multiple upsert items on a SINGLE shared
# TsDevLib instance, mixing insert-path and modify-path, with NO per-item
# closeWarehouse() (only one at the very end — the batch flushPerItem=false
# "flush once" pattern). Asserts the KEY invariant: each item's ProgId is
# preserved across the insert->modify transition. Runs on a 64 MB-stack
# worker thread (same pattern as test_upsert).
add_executable(test_batch_upsert tests/test_batch_upsert.cpp)
target_link_libraries(test_batch_upsert tsdev_lib)

# --- DIRECTORY BATCH unit test (TVS-I.2, GREEN) -------------------------------
# Pure parser/filesystem unit test for the implemented parseDirectory() helper
# used by the batch-<op>-dir CLI subcommands. parseDirectory() and BatchItem
# live inside an anonymous namespace in src/tsdev_cli.cpp (internal linkage),
# so the test #includes the .cpp directly (same TU) to reach them. This pulls
# in tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST to exclude it
# (main() is wrapped in #ifndef TSDEV_CLI_TEST). Although the included CLI
# source references TsDevLib symbols, this test only calls parseDirectory(): it
# does not construct TsDevLib or access orlib20 at runtime. It is therefore a
# non-TS test and must not receive the administrator manifest.
add_executable(test_directory_batch tests/test_directory_batch.cpp)
target_include_directories(test_directory_batch PRIVATE third_party)
target_link_libraries(test_directory_batch tsdev_lib)
add_dependencies(test_directory_batch siggen)

# --- GET-SOURCE integration test (TVS-F.4) -----------------------------------
# Validates the library-level source-retrieval primitive (getSource) used by the
# implemented `get-source` CLI subcommand. Inserts a probe strategy and a probe
# function with KNOWN source text, calls getSource to retrieve the source
# (asserts key substrings present), tests getSource for a nonexistent name
# (asserts empty string), then cleans up. Runs on a 64 MB-stack worker thread
# (same pattern as test_exists / test_upsert).
add_executable(test_get_source tests/test_get_source.cpp)
target_link_libraries(test_get_source tsdev_lib)

# --- Auto-cleanup on compile failure integration test (TD-001, TDD GREEN) -----
# Verifies that a partial warehouse entry created by createEntry is cleaned up
# after verifyModule fails compilation. The auto-cleanup is implemented in
# tsdev_lib.cpp, so this test PASSES (GREEN). Runs on a 64 MB-stack worker
# thread (same pattern as test_insert).
add_executable(test_auto_cleanup tests/test_auto_cleanup.cpp)
target_link_libraries(test_auto_cleanup tsdev_lib)

# --- Workstream M slice M.1: CELCompiler field-layout parity integration test -
# Historical TDD RED->GREEN transition test for the CELCompiler allocation /
# setup / destruction parity with TSDev.exe. The M.1 implementation and test
# are retained as current integration coverage. Uses the onCompilerReady test hook
# (populated via makeTsDevLibForTest) to capture the live 0x58-byte CELCompiler
# object state immediately before _VerifyModule_New and assert the TSDev-parity
# field layout (+0x00, +0x04, +0x08, +0x0C, +0x10, +0x18, +0x28). Two cases:
# Strategy (category=0) and Indicator (category=2, catches the +0x08 divergence
# that Strategy alone would miss). Inserts + verifies + deletes a disposable
# item per case. REQUIRES TradeStation (constructs a real TsDevLib via the
# test-only ctor with the real ComApi). Runs on a 64 MB-stack worker thread.
# Desktop/TSDev running state is not a prerequisite; this test exercises the
# library-level path and does not use the Desktop UI.
add_executable(test_compiler_layout tests/test_compiler_layout.cpp)
target_include_directories(test_compiler_layout PRIVATE src)
target_link_libraries(test_compiler_layout PRIVATE tsdev_lib Ole32)

# --- TVS-M slice M.2: _SaveModule_New flags/password parity integration test -
# Asserts the flags word (arg3) and password buffer (arg5) passed to
# _SaveModule_New@20 by saveSource() match the TSDev.exe values computed by
# FUN_00452290 (see docs/RE/RE_M2_SAVEMODULE_NEW.md, UPDATE 2026-08-25
# Corrections 1 and 2). Uses the onSaveModuleNewReady test hook (populated
# via makeTsDevLibForTest) to capture the flags pointer and password buffer
# immediately before _SaveModule_New and assert:
#   flags = (getCategory(type) & 0x7) | 0x60  (pre-verify; bits 5,6 forced on)
#   password = 64-byte buffer, zeroed then "!p$Kn@aSus" (10 bytes) copied in
# Three cases: Strategy (0x60), Indicator (0x62), Function (0x64). Inserts +
# saves + verifies + round-trips + deletes a disposable item per case.
# REQUIRES TradeStation (constructs a real TsDevLib via the test-only ctor
# with the real ComApi). Runs on a 64 MB-stack worker thread. Per the
# Desktop/TSDev running state is not a prerequisite; this test exercises the
# library-level path and does not use the Desktop UI.
add_executable(test_save_module tests/test_save_module.cpp)
target_include_directories(test_save_module PRIVATE src)
target_link_libraries(test_save_module PRIVATE tsdev_lib Ole32)

# --- TVS-M slice M.3: 0x40A post-success notification parity integration test -
# Asserts the 0x40A library-change notification is sent after verify-success and
# delete-success, and NOT sent after failed verify. The 0x40A message packing
# formula (msg/wParam/lParam layout) is the SSOT in
# docs/RE/RE_M3_0x40A_NOTIFICATION.md. Uses the onNotifyReady
# test hook (populated via makeTsDevLibForTest) to capture the would-be
# send arguments immediately before the 0x40A post (the same point at
# which TSDev calls orlib20 Ordinal_57; ordinal details are the SSOT in
# docs/SIGNATURE_CATALOG.md). See
# docs/RE/RE_M3_0x40A_NOTIFICATION.md (R-M.3.1, R-M.3.2, R-M.3.3).
# Six cases: 3 verify-success (Strat/Ind/Func) + failed-verify (negative)
# + delete-success + batch-reuse (3 verify-success cycles on one TsDevLib).
# REQUIRES TradeStation (constructs a real TsDevLib via the test-only ctor
# with the real ComApi). Runs on a 64 MB-stack worker thread. Per the
# Desktop/TSDev running state is not a prerequisite; this test exercises the
# library-level path and does not use the Desktop UI.
# Historical RED state (pre-M.3): no 0x40A send existed and positive hook
# cases failed while the negative case passed trivially. The current M.3
# implementation sends via ordinal 57 and invokes the hook on verify-success
# and delete-success (category != 4); the receiver-aware cases are retained
# for regression coverage.
add_executable(test_0x40a_notification tests/test_0x40a_notification.cpp)
target_include_directories(test_0x40a_notification PRIVATE src)
target_link_libraries(test_0x40a_notification PRIVATE tsdev_lib Ole32)

# --- DIAGNOSTIC ONLY (M.4 enumeration speed) — NOT in regression suite ---
# Historical/rejected-path diagnostics below are not production M.4 logic. The
# current reverse-dependent implementation uses the linker (ordinal 85) and
# linked per-element dep-lists; GetDependencies/Option B2 probes are retained
# only as provenance. Constructs a real TsDevLib (test-only ctor) + a separate
# OrlibResolver to measure the historical _LoadMasterRecsEX@16 /
# GetDependencies path. Requires TradeStation. Run manually; do NOT add to
# test_regression or run_regression.
add_executable(diag_m4_enum_speed tests/diag_m4_enum_speed.cpp)
target_include_directories(diag_m4_enum_speed PRIVATE src)
target_link_libraries(diag_m4_enum_speed PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 historical GetDependencies probe) — NOT in
# regression suite. Rejected-path diagnostic that inserts a Function + a
# Strategy that calls it, then probes the forward-dependency export
# GetDependencies with several whType values (0, 0x200, 4) and elementIndex
# 0..3. Current reverse-dependent enumeration uses the linker (ordinal 85) and
# linked per-element dep-lists; this probe is retained for historical evidence.
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_deps tests/diag_m4_deps.cpp)
target_include_directories(diag_m4_deps PRIVATE src)
target_link_libraries(diag_m4_deps PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 historical GetDependencies probes) — NOT in
# regression suite. Rejected-path diagnostic that tests historical hypotheses
# for the forward-dependency exports GetDependencies and
# _GetStaticDependencyList@12: parenthesized calls, alternate APIs, and
# warehouse reload. Current reverse-dependent enumeration uses the linker
# (ordinal 85) and linked per-element dep-lists; this probe is retained for
# historical evidence. Requires TradeStation. Run manually; do NOT add to
# test_regression or run_regression.
add_executable(diag_m4_deps2 tests/diag_m4_deps2.cpp)
target_include_directories(diag_m4_deps2 PRIVATE src)
target_link_libraries(diag_m4_deps2 PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 historical raw-record probe) — NOT in regression
# suite. Rejected-path diagnostic that dumps the raw 0x10A-byte master-record
# copy returned by _GetMasterRecByType@16 and records its element/dependency
# fields alongside the current linker-based layout. The original probe was
# intended to explain empty GetDependencies results; current evidence identifies
# GetDependencies as a forward API and production reverse enumeration uses the
# linker (ordinal 85) with linked per-element dep-lists. Requires TradeStation.
# Run manually; do NOT add to test_regression or run_regression.
add_executable(diag_m4_raw_rec tests/diag_m4_raw_rec.cpp)
target_include_directories(diag_m4_raw_rec PRIVATE src)
target_link_libraries(diag_m4_raw_rec PRIVATE tsdev_lib orlib_resolver Ole32)

# --- M.4 historical Ordinal_69 live-record diagnostic (manual, NOT in
# regression) ---------------------------------------------------------------
# Historical probe of Ordinal_69 (orlib20 ord 69, RVA 0x1A3E00) and its live
# record/element arrays. It compares that data with the forward-dependency
# GetDependencies export (ord 487) and the _GetMasterRecByType@16 copy (ord
# 568). The current reverse-dependent implementation uses the linker (ordinal
# 85) and linked per-element dep-lists, not this diagnostic path. Also probes
# Ordinal_70, Ordinal_28, and GetDependencies for historical comparison.
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_ord69 tests/diag_m4_ord69.cpp)
target_include_directories(diag_m4_ord69 PRIVATE src)
target_link_libraries(diag_m4_ord69 PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 historical forward-dependency probe) — NOT in
# regression suite. Rejected-path diagnostic that tests whether the forward
# APIs GetDependencies (ord 487) and _GetStaticDependencyList@12 (ord 531)
# return a Function's ProgId for a Strategy that calls it. It also dumps the
# 0x10A partial copy via Ordinal_28, calls Ordinal_69 to read live fields, and
# probes GetDependencies with multiple calling conventions. Current
# reverse-dependent enumeration uses the linker (ordinal 85) and linked
# per-element dep-lists; this probe is retained for historical ABI/evidence
# comparison. Requires TradeStation. Run manually; do NOT add to
# test_regression or run_regression.
add_executable(diag_m4_fwd_deps tests/diag_m4_fwd_deps.cpp)
target_include_directories(diag_m4_fwd_deps PRIVATE src)
target_link_libraries(diag_m4_fwd_deps PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 subtype-byte +0xe9 probe) — NOT in regression suite.
# Standalone diagnostic that reads master_record[+0xe9] (the subtype byte) for
# CLI-created items (Function + Strategy) and existing items (Strategies,
# Functions, Indicators) via Ordinal_28(category, progId, 0, buf), to verify
# the FUN_101d4280(category, +0xe9) -> version theory (Strategy: 0x200 when
# +0xe9 is 2 or 3, 0 when +0xe9 is 0 or 1). Enumerates existing records via
# _LoadMasterRecsEX@16 (read-only — does NOT modify any existing item). Prints
# a hex dump of 0xe0-0xf5 for one existing Strategy and one existing Function
# for comparison with CLI-created items. Cleans up the CLI-created items.
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_subtype tests/diag_m4_subtype.cpp)
target_include_directories(diag_m4_subtype PRIVATE src)
target_link_libraries(diag_m4_subtype PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 historical record-layout comparison) — NOT in
# regression suite. Standalone READ-ONLY diagnostic that enumerates existing
# Strategies/Functions/Indicators via _LoadMasterRecsEX@16, calls the linker
# (ordinal 85) on each, and compares record+0x6f/+0x73 with the linked
# per-element dep-list at element+0x7b/+0x73. The original interpretation that
# GetDependencies read the linker-populated record+0x6f/+0x73 location was
# superseded by M.4 evidence; production scans linked per-element lists.
# Does NOT insert/modify/delete any items. Requires TradeStation. Run manually;
# do NOT add to test_regression or run_regression.
add_executable(diag_m4_existing_link tests/diag_m4_existing_link.cpp)
target_include_directories(diag_m4_existing_link PRIVATE src)
target_link_libraries(diag_m4_existing_link PRIVATE tsdev_lib orlib_resolver Ole32)

# --- M.4 historical full dep-list dump (manual, NOT in regression) -----------
# Dumps the FULL dep-list content (up to 4096 bytes per element) for existing
# Strategies (first 10), Indicators (first 10), and Functions (first 5) by
# calling the linker (ordinal 85) on each existing item, walking the element
# array (elem size 0xb5), and reading element+0x73 (dep-list size) and
# element+0x7b (dep-list ptr). It retains the historical 0xfa4 function-ref
# expectation for comparison; M.4 evidence identifies compiler-written
# function references as 0xfa3. READ-ONLY — does NOT insert/modify/delete any
# items. Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_deplist_dump tests/diag_m4_deplist_dump.cpp)
target_include_directories(diag_m4_deplist_dump PRIVATE src)
target_link_libraries(diag_m4_deplist_dump PRIVATE tsdev_lib orlib_resolver Ole32)

# --- DIAGNOSTIC ONLY (M.4 reverse-dependent enumeration verification) ---
# Standalone READ-ONLY diagnostic that calls the ACTUAL production
# enumerateReverseDependents method (via a behavior-preserving friend seam in
# src/tsdev_lib.h) and verifies it finds Indicators 3 and 8 as reverse
# dependents of Functions 516/517/21/458, plus a negative control (999999).
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_reverse_enum tests/diag_m4_reverse_enum.cpp)
target_include_directories(diag_m4_reverse_enum PRIVATE src)
target_link_libraries(diag_m4_reverse_enum PRIVATE tsdev_lib Ole32)

# --- TVS-M.4: Ordinal_71 (InitGlobalLookup) + cmd 0x0D diagnostic (manual) -
# Verifies the ABI of Ordinal_71 (orlib20 ord 71, RVA 0x1A9460,
# FUN_101a9460 — "InitGlobalLookup"). Ghidra-confirmed: Ordinal_71 is a
# lazy BST-global initializer (ignores its 2 pushed args, returns 0x2c-byte
# BST control block DAT_1023ced4, does NOT trigger the WHServer worker, does
# NOT set warehouse+0xC2, does NOT return a module record). Also attempts the
# direct worker-thread command 0x0D replication (FUN_101cfce0 pattern:
# warehouse+0x12=type, +0x16=progId, +0x04=0x0D, SetEvent(+0x6e),
# WaitForSingleObject(+0x6a), check +0x4e, read +0x26 SafeArray) and probes
# Ordinal_10/69/70/68/28 for comparison. Requires TradeStation. Run manually;
# do NOT add to test_regression or run_regression.
add_executable(diag_m4_ord71 tests/diag_m4_ord71.cpp)
target_include_directories(diag_m4_ord71 PRIVATE src)
target_link_libraries(diag_m4_ord71 PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: Ordinal_69 linker-path diagnostic (manual, NOT in regression) -
# Tests whether Ordinal_69 with param_3 != 0 (the short-circuit / linker
# path) returns a record with populated element arrays, vs. param_3 == 0,
# and whether calling the linker (ordinal 85, FUN_101a3340) directly
# produces populated arrays. It also performs a historical GetDependencies
# comparison after the linker path and inspects the SafeArray at +0x26 right
# after verify (via Ordinal_28); GetDependencies is not used by production
# reverse-dependent enumeration. Requires TradeStation. Run manually; do NOT
# add to test_regression or run_regression.
add_executable(diag_m4_linker tests/diag_m4_linker.cpp)
target_include_directories(diag_m4_linker PRIVATE src)
target_link_libraries(diag_m4_linker PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: Frida linker-internal trace driver (manual, NOT in regression) -
# Drives the orlib20 linker (FUN_101a3340, ordinal 85) from our own CLI
# process while a Frida script (tmp/frida_m4_linker_trace.js) traces the
# linker's internals: object-code buffer (FUN_101c2a30), decompressor
# (FUN_101dd260), 0xBBC magic check, three populators (FUN_101a25a0/2430/22c0),
# and commit (FUN_101c0990). Answers WHY the linker returns a non-NULL record
# with EMPTY element arrays. Prints PID + "READY FOR FRIDA" and sleeps 8s to
# allow Frida attach. All orlib20 calls are SEH-protected. Read-only
# instrumentation; no production code is modified. Requires TradeStation. Run
# manually; do NOT add to test_regression or run_regression.
add_executable(diag_m4_frida_linker tests/diag_m4_frida_linker.cpp)
target_include_directories(diag_m4_frida_linker PRIVATE src)
target_link_libraries(diag_m4_frida_linker PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: linker internals diagnostic (manual, NOT in regression) -
# Investigates why the linker FUN_101a3340 (ordinal 85) returns a non-NULL
# record with empty element arrays (+0x29=0, +0x4f=NULL, +0x26=NULL) even
# though warehouse state 0x38 succeeds. Reads warehouse+0x23d (cached linked
# record) before/after the linker call, dumps the raw compiled object code
# from the SafeArray at warehouse+0x26 (scans for 0xBBC/0xBB9 magic), dumps
# the .ell file (first 256 bytes, scans for magic), and compares Ordinal_69
# with param_3=0 (normal) vs param_3=1 (short-circuit). All orlib20 calls
# are SEH-protected. The SafeArray is NOT destroyed (owned by the warehouse).
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_linker_internals tests/diag_m4_linker_internals.cpp)
target_include_directories(diag_m4_linker_internals PRIVATE src)
target_link_libraries(diag_m4_linker_internals PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: decompressed object code buffer dump + warehouse indirection ---
# Dumps the DECOMPRESSED object code buffer the linker uses (at wrapper+0x1c1),
# resolves whether Ordinal_10(0) returns the per-module wrapper or the
# CELWHouse (Ghidra showed state handlers access *(param_1+0xc)+0x26, NOT
# param_1+0x26), checks offset 0x26 of the decompressed buffer (0x2000000 in
# TSDev, NULL in our CLI), scans for 0xBBC/0xBB9 magic in the decompressed
# data (NOT the compressed .ell file), inspects the SafeArray at the correct
# indirection, and compares the decompressed objcode with the raw .ell file
# (which contains COMPRESSED object code with a 0x44-byte header).
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_objcode tests/diag_m4_objcode.cpp)
target_include_directories(diag_m4_objcode PRIVATE src)
target_link_libraries(diag_m4_objcode PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: WHServer state 0x38 failure diagnostic (manual, NOT in regression) -
# Investigates why the linker's state 0x38 (load compiled code from .ell) fails
# in our CLI process while verify (state 0xe) works. Reads the warehouse object
# live via Ordinal_10(0), inspects all key warehouse fields (+0x04 state, +0x12
# type, +0x16 moduleId, +0x1e key, +0x26 SafeArray, +0x4e HRESULT, +0x5e
# per-module COM, +0x6a done event, +0x6e wake event, +0x72 mutex, +0x76 owner
# thread, +0xAE main WHServer COM, +0xB6 SA cache), checks the .ell file
# existence/size, calls the linker (ordinal 85) directly with BOTH __stdcall
# and __fastcall conventions and errorFlag 0/1, calls Ordinal_69 with
# param_3=progId (linker path), and tails WH_ERRS.LOG / EL_ERRS.LOG.
# Requires TradeStation. Run manually; do NOT add to test_regression or
# run_regression.
add_executable(diag_m4_whserver tests/diag_m4_whserver.cpp)
target_include_directories(diag_m4_whserver PRIVATE src)
target_link_libraries(diag_m4_whserver PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-M.4: Reverse-dependent revalidation integration test (manual, blocked) ---
# Validates reverse-dependent revalidation parity with TSDev.exe FUN_004D91E0.
# Requires TradeStation (real orlib20.dll, real warehouse). M.4 remains blocked
# by the CLI-created object-code defect; run this test manually only. Do NOT
# add it to the test_regression or run_regression aggregates.
add_executable(test_reverse_dependent_revalidation tests/test_reverse_dependent_revalidation.cpp)
target_include_directories(test_reverse_dependent_revalidation PRIVATE src)
target_link_libraries(test_reverse_dependent_revalidation PRIVATE tsdev_lib Ole32)

# --- TVS-M.3b.2c: Manual admin diagnostic for CLI object-code version/count ---
# Manual admin diagnostic (NOT in test_regression / $TestDefinitions /
# run_regression). The M.3b "defect" was a TEST ARTIFACT (M.3b.2b finding):
# the original RED test used a declaration-free Function source which
# legitimately has elemCount=0 because there are no Inputs:/Variables:/Array:
# declarations to trigger the Record* count-populators. This test is NO LONGER
# expected to fail: it now uses declaration-rich sources for 4 types
# (Strategy, Indicator, Function, ShowMe) and asserts elemCount > 0; TradingApp
# with `using elsystem;` (no declarations) asserts elemCount == 0 as the
# expected no-declarations pattern. The test remains a manual admin diagnostic,
# NOT in test_regression or $TestDefinitions, because it tests orlib20 internal
# buffer layouts at the linker/ordinal-85 abstraction level (no higher-level
# API for the "element arrays populated" property); M.4b will abandon this
# linker-based approach in favor of warehouse-level APIs. Requires TradeStation
# (real orlib20.dll, real warehouse). Run manually from an elevated process
# only. Do NOT add to test_regression or run_regression.
# Reference: docs/RE/RE_M3B_OBJECT_CODE_VERSION_DEFECT.md §12-14
add_executable(test_m3b_object_code_version tests/test_m3b_object_code_version.cpp)
target_include_directories(test_m3b_object_code_version PRIVATE src)
target_link_libraries(test_m3b_object_code_version PRIVATE tsdev_lib orlib_resolver Ole32)

# --- M.4b.1: Warehouse-level reverse-dependent (Callers) enumeration test ---
# MANUAL ADMIN DIAGNOSTIC for Workstream M slice M.4b.1. Verifies
# TsDevLib::enumerateCallers() returns the correct reverse dependents
# (callers) of a library item via the orlib20 warehouse worker-thread
# command protocol (cmd 0x0D → cmd 0x3B → SafeArray parse).
#
# This test is NOT part of the regression suite and is intentionally NOT
# wired into `test_regression` / `$TestDefinitions` / `run_regression.ps1`.
# It is a standalone, self-contained diagnostic-style executable run
# manually from an elevated process against live TradeStation. It follows
# the same pattern as `test_m3b_object_code_version` (manual admin target,
# requires TradeStation, real orlib20.dll, real warehouse).
#
# Build:
#   cmake --build build --config Release --target test_callers_enumeration
# Run (must be elevated):
#   .\build\Release\test_callers_enumeration.exe
#
# Reference: docs/RE/RE_M4_REVERSE_DEPENDENT_REVALIDATION.md §9 (R-M.4a.1
# through R-M.4a.6, §9.7 SafeArray entry format).
add_executable(test_callers_enumeration tests/test_callers_enumeration.cpp)
target_include_directories(test_callers_enumeration PRIVATE src)
target_link_libraries(test_callers_enumeration PRIVATE tsdev_lib orlib_resolver Ole32)

# --- TVS-D.1: Batch feasibility stress test (opt-in, NOT in regression suite) -
# Validates ONE TsDevLib instance can do N (default 600) sequential
# insert+compile+verify ops then delete all N, leaving a clean warehouse, all
# in ONE process on ONE 64 MB-stack worker thread. It exercises the shared
# one-instance behavior used by the implemented batch CLI (TVS-D.3).
# Heavy/slow and hits the real warehouse. Run manually, NOT via
# run_regression:
#   cmake --build build --config Release --target test_stress_batch
#   $env:STRESS_BATCH_N=5;   .\build\Release\test_stress_batch.exe   # smoke
#   $env:STRESS_BATCH_N=600; .\build\Release\test_stress_batch.exe   # full
add_executable(test_stress_batch tests/test_stress_batch.cpp)
# TVS-L.15: test_stress_batch samples process resource usage via read-only
# Win32 query APIs (GetProcessHandleCount, GetGuiResources, EnumWindows,
# GetProcessMemoryInfo). psapi provides GetProcessMemoryInfo; user32 provides
# GetGuiResources/EnumWindows. These are test-local measurement hooks only —
# no production COM debug counters are added to tsdev_lib.
target_link_libraries(test_stress_batch tsdev_lib psapi user32)

# --- Production CLI ---------------------------------------------------------
# TVS-D.2: tsdev_cli includes nlohmann/json.hpp (vendored under third_party/)
# for the --json structured output flag. The include mechanism mirrors sigtool
# (target_include_directories(... PRIVATE third_party)).
add_executable(tsdev_cli src/tsdev_cli.cpp)
target_include_directories(tsdev_cli PRIVATE third_party)
target_link_libraries(tsdev_cli tsdev_lib)

# --- §7.1 diagnostic CLI (tsdev_cli_diag) ------------------------------------
# Field-diagnostic build of the CLI: same sources + TSDEV_DIAG. Every diag
# call-site macro compiles out of the production tsdev_cli above, so the
# production binary stays behavior-identical. The diag build adds the file
# sink + watchdog (tsdev_diag) and links dbghelp/user32 transitively via
# tsdev_lib_diag -> tsdev_diag. NOT copied into dist/ (that stays the
# production bundle); its POST_BUILD block below assembles it into the
# parallel dist_diag/ instead.
add_executable(tsdev_cli_diag src/tsdev_cli.cpp)
target_include_directories(tsdev_cli_diag PRIVATE third_party)
target_link_libraries(tsdev_cli_diag tsdev_lib_diag)
target_compile_definitions(tsdev_cli_diag PRIVATE TSDEV_DIAG)

# --- Admin-privilege manifest for TS-accessing executables -------------------
# When ON, every executable that accesses the TradeStation TSDev Library (which
# requires admin privileges, because TSDev.exe and TradeStation Desktop always
# run elevated) embeds requestedExecutionLevel="requireAdministrator" in its
# application manifest. Windows then refuses to start these executables without
# an elevated (administrator) token. When the PARENT process is already elevated
# (elevated Python wrapper / test runner / shell), child processes inherit that
# token silently with NO per-run UAC prompt — one UAC prompt per session.
# When OFF, manifests default to asInvoker (no automatic UAC prompt), but the
# direct tsdev_cli runtime elevation guard remains active and returns exit 11
# when it executes non-elevated.
#
# CI builds with the default ON; this is safe because CI tests never invoke
# tsdev_cli.exe or any TS-dependent test executable (the -NoTs suites use only
# non-TS unit tests + sigtool; non-e2e Python tests mock subprocess.run).
# Developers who need an asInvoker build variant pass -DTSDEV_REQUIRE_ADMIN=OFF.
option(TSDEV_REQUIRE_ADMIN
    "Embed requireAdministrator execution level in TS-accessing executables" ON)

# All executables that access the TSDev Library / orlib20.dll at runtime and
# therefore require admin privileges. Excludes: sigtool (static DLL file
# analysis, no runtime orlib20 access), test_no_hardcoded_rvas / test_tradingapp_type
# / test_showme_type / test_orlib_resolver / test_elevation_guard
# / test_crash_handler / test_json_serialize / test_serve_dispatch
# / test_serve_framing / test_batch_progress / test_pbar / test_com_apartment_guard
# / test_tsdev_com_init_seam
# / test_persistence_integrity / test_create_path_cleanup
# / test_preop_hash_classification / test_delete_disk_cleanup
# / test_close_warehouse / test_directory_batch
# (pure unit tests, no TSDev Library access, run in CI under -NoTs from a
# non-elevated runner).
set(TSDEV_ADMIN_TARGETS
    tsdev_cli
    tsdev_cli_diag
    test_insert
    test_delete
    test_delete_tradingapp
    test_delete_check
    test_delete_diag
    test_path_discovery
    test_modify
    test_finalization_lifecycle
    test_insert_tradingapp
    test_modify_tradingapp
    test_exists
    test_enumerate
    test_upsert
    test_batch_upsert
    test_get_source
    test_auto_cleanup
    test_compiler_layout
    test_save_module
    test_0x40a_notification
    test_reverse_dependent_revalidation
    test_m3b_object_code_version
    test_callers_enumeration
    test_stress_batch
    diag_m4_enum_speed
    diag_m4_deps
    diag_m4_deps2
    diag_m4_raw_rec
    diag_m4_ord69
    diag_m4_fwd_deps
    diag_m4_subtype
    diag_m4_existing_link
    diag_m4_deplist_dump
    diag_m4_reverse_enum
    diag_m4_ord71
    diag_m4_linker
    diag_m4_frida_linker
    diag_m4_linker_internals
    diag_m4_objcode
    diag_m4_whserver
)

if(TSDEV_REQUIRE_ADMIN)
    foreach(tgt ${TSDEV_ADMIN_TARGETS})
        target_link_options(${tgt} PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endforeach()
endif()

# --- Workstream G (TVS-G.1): POST_BUILD bundle assembly ----------------------
# Glob the Python wrapper sources with CONFIGURE_DEPENDS so newly added .py
# files trigger a re-configure automatically.
file(GLOB TSDEV_PY_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/tsdev_automation/*.py)

# Build the per-file copy COMMAND lines at configure time. A foreach loop cannot
# be embedded directly inside add_custom_command's COMMAND section (COMMAND lines
# are not imperative CMake), so we expand the GLOB'd list here into a list of
# COMMAND arguments that are spliced into the POST_BUILD custom command below.
set(TSDEV_PY_COPY_COMMANDS "")
foreach(py ${TSDEV_PY_SOURCES})
    get_filename_component(py_name ${py} NAME)
    list(APPEND TSDEV_PY_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${py} ${CMAKE_SOURCE_DIR}/dist/tsdev_automation/${py_name})
endforeach()

# NOTE: This POST_BUILD command writes into ${CMAKE_SOURCE_DIR}/dist (the source
# tree), NOT the build tree. This is INTENTIONAL -- see the comment near the
# file(MAKE_DIRECTORY) calls above. The goal is a root-level gitignored dist/
# for distribution. copy_if_different avoids touching timestamps on unchanged
# files. The remove_directory + make_directory pair ensures dist/tsdev_automation
# is a clean mirror of src/tsdev_automation/*.py on every build (copy_if_different
# alone does NOT remove files deleted/renamed in source). The README copy was
# added by TVS-G.3. No zip command here (that is TVS-G.5) -- this block is
# atomic per-TVS.
add_custom_command(TARGET tsdev_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:tsdev_cli> ${CMAKE_SOURCE_DIR}/dist/tsdev_cli.exe
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${CMAKE_SOURCE_DIR}/dist/tsdev_automation
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_SOURCE_DIR}/dist/tsdev_automation
    ${TSDEV_PY_COPY_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/docs/BUNDLE_README.md
            ${CMAKE_SOURCE_DIR}/dist/README.md
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/create_bundle_zip.ps1 -SourceDir ${CMAKE_SOURCE_DIR}
    COMMENT "Assembling dist/ bundle (tsdev_cli.exe + Python wrapper)"
    VERBATIM
)

# --- Diagnostic dist assembly (dist_diag/, analog of the dist/ block above) --
# Same mechanism and copy style as the production POST_BUILD above
# (copy_if_different into a ${CMAKE_SOURCE_DIR} directory), reduced to the
# diagnostic artifact set: the diag executable only. The production-only steps
# (Python wrapper refresh, README, create_bundle_zip.ps1) are deliberately NOT
# replicated. No PDB copy, matching dist/ (which ships no PDB). Kept as a
# separate block rather than a shared helper because the production block is
# not parameterized; extracting one would restructure it for no gain.
add_custom_command(TARGET tsdev_cli_diag POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:tsdev_cli_diag> ${CMAKE_SOURCE_DIR}/dist_diag/tsdev_cli_diag.exe
    COMMENT "Assembling dist_diag/ (tsdev_cli_diag.exe)"
    VERBATIM
)

# --- sigtool: standalone orlib20.dll signature verification tool --------------
# CLI for verifying signatures, validating patch sites, inspecting DLL info,
# ad-hoc pattern scanning, and version-manifest lookup.
add_executable(sigtool src/sigtool.cpp)
target_include_directories(sigtool PRIVATE third_party)
target_link_libraries(sigtool PRIVATE orlib_resolver)
add_dependencies(sigtool siggen)

# --- OrlibResolver library (TVS-B.4) -----------------------------------------
# Static library encapsulating all version-specific orlib20.dll knowledge.
# Fully implemented: 45 methods, zero stubs.
add_library(orlib_resolver STATIC src/orlib_resolver.cpp)
target_include_directories(orlib_resolver PUBLIC src)
# Generated header include dir is PUBLIC so consumers (tsdev_lib, tests) that
# include orlib_resolver.h can also find signatures.gen.h (which defines the
# SignatureSetId enum before including orlib_resolver.h).
target_include_directories(orlib_resolver PUBLIC ${GEN_INCLUDE_DIR})
add_dependencies(orlib_resolver siggen)
target_link_libraries(orlib_resolver PUBLIC bcrypt)

# --- OrlibResolver test executable (TVS-B.4) ---------------------------------
# Historical/transient verification record (not current run evidence): 184 assertions across 6 TDD cycles.
add_executable(test_orlib_resolver tests/test_orlib_resolver.cpp)
target_link_libraries(test_orlib_resolver PRIVATE orlib_resolver)

# --- Master regression build dependency target (TVS-B.6.3) -------------------
# This is a build-only dependency target: it ensures ALL regression test
# executables, the production CLI, and sigtool are built when invoked via:
#   cmake --build build --config Release --target test_regression
# The actual regression test execution is performed by the PowerShell
# orchestrator script tests/run_regression.ps1 (which runs sigtool verify,
# the C++ unit tests, and the E2E CLI tests). The C++ unit test inventory
# (list + count) is the SSOT in tests/run_unit_tests.ps1 ($TestDefinitions);
# the E2E CLI case list is the SSOT in docs/E2E_TEST_SPEC.md. This target
# adds NO build commands of its own - it merely aggregates the dependencies.
add_custom_target(test_regression)
add_dependencies(test_regression
    test_insert
    test_delete
    test_delete_tradingapp
    test_delete_check
    test_delete_diag
    test_path_discovery
    test_modify
    test_finalization_lifecycle
    test_no_hardcoded_rvas
    test_tradingapp_type
    test_showme_type
    test_elevation_guard
    test_crash_handler
    test_json_serialize
    test_log_levels
    test_serve_dispatch
    test_serve_framing
    test_batch_progress
    test_pbar
    test_com_apartment_guard
    test_tsdev_com_init_seam
    test_persistence_integrity
    test_create_path_cleanup
    test_preop_hash_classification
    test_delete_disk_cleanup
    test_close_warehouse
    test_diag_log
    test_whserver_health
    test_whserver_census
    test_whserver_recycle
    test_dispatch_watchdog
    test_cache_backup
    test_cache_cli_units
    test_recycle_whserver_flag
    test_recycle_resume
    test_insert_tradingapp
    test_modify_tradingapp
    test_exists
    test_enumerate
    test_upsert
    test_batch_upsert
    test_directory_batch
    test_get_source
    test_auto_cleanup
    test_compiler_layout
    test_save_module
    test_0x40a_notification
    test_orlib_resolver
    tsdev_cli
    tsdev_cli_diag
    sigtool
)
