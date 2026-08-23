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
# build directory -- that would break the distribution workflow. Both dist/ and
# artifacts/ are gitignored via the root .gitignore.
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist/tsdev_automation)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/artifacts)

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
add_library(tsdev_lib STATIC
    src/tsdev_lib.cpp
    src/com_apartment_guard.cpp
    src/persistence_integrity.cpp
    src/create_path_cleanup.cpp
    src/delete_disk_cleanup.cpp)
target_include_directories(tsdev_lib PUBLIC src)
target_link_libraries(tsdev_lib PUBLIC orlib_resolver)
target_link_libraries(tsdev_lib PRIVATE Ole32)

# --- INSERT integration test (TDD red-green) --------------------------------
add_executable(test_insert tests/test_insert.cpp)
target_link_libraries(test_insert tsdev_lib)

# --- DELETE integration test (TDD red-green) --------------------------------
add_executable(test_delete tests/test_delete.cpp)
target_link_libraries(test_delete tsdev_lib)

# --- TradingApp DELETE integration test (TVS-C.5, TDD red-green) ------------
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

# --- MODIFY integration test (TDD red-green) --------------------------------
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

# --- No hardcoded RVA constants test (AC1 TDD red-green) ---------------------
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

# --- Elevation guard unit test (TVS-K.1, TDD RED) -----------------------------
# Pure unit test for the isElevated() helper and the errorCodeString(11) entry
# that the elevation guard in main() will rely on. isElevated() and
# errorCodeString() live inside an anonymous namespace in src/tsdev_cli.cpp
# (internal linkage), so the test #includes the .cpp directly (same TU) to
# reach them. This pulls in tsdev_cli.cpp's main(); the test #defines
# TSDEV_CLI_TEST to exclude it (main() is wrapped in #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access), so it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
# NOTE: isElevated() and case 11 in errorCodeString() do NOT exist yet — this
# is the TDD RED step and will FAIL TO COMPILE until the Green step adds them.
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

# --- Persistence-integrity validator unit test (TVS-L.9, TDD RED) -------------
# Pure unit test for the SHARED persistence-integrity validator that classifies
# the cross-store (.ell vs .xml) consistency of a modify/upsert operation. The
# validator is DECLARED in src/persistence_integrity.h but has NO implementation
# yet (no src/persistence_integrity.cpp). This is the TDD RED step: the test
# includes the header and calls tsdev::validatePersistenceIntegrity(), so the
# test executable FAILS TO LINK (unresolved external) until a later (GREEN) TVS
# provides the implementation.
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
# bcrypt is linked for the test's own SHA-256 of fixture bytes (pre-op hashes);
# tsdev_lib supplies the validatePersistenceIntegrity() definition (via
# src/persistence_integrity.cpp compiled into the static library). Because
# tsdev_lib is a static archive, the linker pulls in ONLY the
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

# --- Create-path cleanup ownership unit test (TVS-L.10, TDD RED->GREEN) -------
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
#   require cleanup for all six stages; for progId <= 0 it MUST NOT (no
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
# TDD: the RED step declared deleteDiskCleanupErrorFailsResult() in the header
# with no .cpp, so the test executable FAILED TO LINK (unresolved external) —
# that link failure was the RED signal. The GREEN step adds
# src/delete_disk_cleanup.cpp to tsdev_lib (above), so the symbol now resolves
# and the test links and runs; the assertions below pin the contract.
add_executable(test_delete_disk_cleanup tests/test_delete_disk_cleanup.cpp)
target_include_directories(test_delete_disk_cleanup PRIVATE src)
target_link_libraries(test_delete_disk_cleanup PRIVATE tsdev_lib)

# --- JSON serialize unit test (TVS-K.2, TDD RED) ------------------------------
# Pure unit test for the cliResultToJson() helper extracted from emitOutput()'s
# JSON branch. cliResultToJson() will live inside an anonymous namespace in
# src/tsdev_cli.cpp (internal linkage), so the test #includes the .cpp directly
# (same TU) to reach it. This pulls in tsdev_cli.cpp's main(); the test #defines
# TSDEV_CLI_TEST to exclude it (main() is wrapped in #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access), so it is NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs.
# NOTE: cliResultToJson() does NOT exist yet — this is the TDD RED step and
# will FAIL TO COMPILE until the Green step extracts it from emitOutput().
add_executable(test_json_serialize tests/test_json_serialize.cpp)
target_include_directories(test_json_serialize PRIVATE third_party)
target_link_libraries(test_json_serialize tsdev_lib)
add_dependencies(test_json_serialize siggen)

# --- Serve request dispatch unit test (TVS-K.3, TDD RED) ----------------------
# Pure unit test for parseServeRequest() — the JSON request -> ServeRequest
# parser used by the serve/broker layer. parseServeRequest() and the
# ServeRequest struct will live inside an anonymous namespace in
# src/tsdev_cli.cpp (internal linkage), so the test #includes the .cpp directly
# (same TU) to reach it. This pulls in tsdev_cli.cpp's main(); the test #defines
# TSDEV_CLI_TEST to exclude it (main() is wrapped in #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. It is a PURE UNIT TEST (no orlib20
# access, no TsDevLib construction — parseServeRequest is pure logic), so it is
# NOT in TSDEV_ADMIN_TARGETS and runs under -NoTs. dispatchServeRequest() is
# NOT unit-tested here (it needs a real TsDevLib; validated via K.5/K.6a E2E).
# NOTE: parseServeRequest() and ServeRequest do NOT exist yet — this is the TDD
# RED step and will FAIL TO COMPILE until the Green step adds them.
add_executable(test_serve_dispatch tests/test_serve_dispatch.cpp)
target_include_directories(test_serve_dispatch PRIVATE third_party)
target_link_libraries(test_serve_dispatch tsdev_lib)
add_dependencies(test_serve_dispatch siggen)

# --- Serve framing helpers unit test (TVS-K.4, TDD RED) -----------------------
# Pure unit test for writeFramed()/readFramed() — the length-prefixed framing
# I/O helpers used by the serve/broker layer. writeFramed() and readFramed()
# will live inside an anonymous namespace in src/tsdev_cli.cpp (internal
# linkage), so the test #includes the .cpp directly (same TU) to reach them.
# This pulls in tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST to
# exclude it (main() is wrapped in #ifndef TSDEV_CLI_TEST). The test needs the
# third_party include dir (nlohmann/json) because the included tsdev_cli.cpp
# uses it, and links tsdev_lib for the TsDevLib symbols referenced by the
# included CLI code. It is a PURE UNIT TEST (no orlib20 access, no TsDevLib
# construction — the framing helpers are pure Windows pipe I/O), so it is NOT
# in TSDEV_ADMIN_TARGETS and runs under -NoTs. The framing helpers use Windows
# HANDLEs (from CreatePipe), not FILE* or std::istream. The full pipe server
# (cmdServe/runServe) is validated via E2E in K.6a, NOT here.
# NOTE: writeFramed() and readFramed() do NOT exist yet — this is the TDD RED
# step and will FAIL TO COMPILE until the Green step adds them.
add_executable(test_serve_framing tests/test_serve_framing.cpp)
target_include_directories(test_serve_framing PRIVATE third_party)
target_link_libraries(test_serve_framing tsdev_lib)
add_dependencies(test_serve_framing siggen)

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

# --- EXISTS integration test (TVS-F.1, TDD red-green) ------------------------
# Validates the library-level existence-query primitive (findExistPos) that the
# future `exists` CLI subcommand will wrap. Inserts a probe strategy, queries
# findExistPos for the existing name (expects ProgId > 0) and for a nonexistent
# name (expects 0), then cleans up. Runs on a 64 MB-stack worker thread (same
# pattern as test_insert).
add_executable(test_exists tests/test_exists.cpp)
target_link_libraries(test_exists tsdev_lib)

# --- ENUMERATE integration test (TVS-F.5, TDD RED) ---------------------------
# Validates the library-level case-insensitive existence-query primitive
# (enumerateRecords) that the future `exists --case-insensitive` CLI subcommand
# will wrap. Inserts a probe strategy, queries enumerateRecords with the name
# in UPPERCASE (expects exists=true, verbatim stored name, ProgId > 0) and for
# a nonexistent name (expects exists=false, empty stored name, ProgId == 0),
# then cleans up. Runs on a 64 MB-stack worker thread (same pattern as
# test_exists). NOTE: enumerateRecords and EnumerateResult do NOT exist yet in
# tsdev_lib.h — this test is the TDD RED step (F5-S2) and will FAIL TO COMPILE
# until F5-S4 implements them.
add_executable(test_enumerate tests/test_enumerate.cpp)
target_link_libraries(test_enumerate tsdev_lib)

# --- UPSERT integration test (TVS-F.3, TDD red-green) ------------------------
# Validates the library-level primitives that the future `upsert` CLI subcommand
# will compose (findExistPos + createEntry + saveSource + verifyModule +
# modifyEntry + deleteEntry). Inserts a probe strategy, verifies the upsert
# composition (insert then modify-in-place), asserts the KEY upsert invariant
# (ProgId preserved after modify), then cleans up. Runs on a 64 MB-stack worker
# thread (same pattern as test_exists).
add_executable(test_upsert tests/test_upsert.cpp)
target_link_libraries(test_upsert tsdev_lib)

# --- BATCH UPSERT integration test (TVS-I.1, TDD red-green) -------------------
# Validates the library-level composition that the future `batch-upsert` CLI
# subcommand will rely on: multiple upsert items on a SINGLE shared TsDevLib
# instance, mixing insert-path and modify-path, with NO per-item
# closeWarehouse() (only one at the very end — the batch flushPerItem=false
# "flush once" pattern). Asserts the KEY invariant: each item's ProgId is
# preserved across the insert->modify transition. Runs on a 64 MB-stack
# worker thread (same pattern as test_upsert).
add_executable(test_batch_upsert tests/test_batch_upsert.cpp)
target_link_libraries(test_batch_upsert tsdev_lib)

# --- DIRECTORY BATCH unit test (TVS-I.2, TDD RED) -----------------------------
# Unit test for parseDirectory() — the directory-enumeration logic that the
# future batch-<op>-dir CLI subcommands will rely on. parseDirectory() and
# BatchItem live inside an anonymous namespace in src/tsdev_cli.cpp (internal
# linkage), so the test #includes the .cpp directly (same TU) to reach them.
# This pulls in tsdev_cli.cpp's main(); the test #defines TSDEV_CLI_TEST to
# exclude it (the implementer must wrap main() in #ifndef TSDEV_CLI_TEST).
# The test needs the third_party include dir (nlohmann/json) because the
# included tsdev_cli.cpp uses it, and links tsdev_lib for the TsDevLib symbols
# referenced by the included CLI code. NOTE: parseDirectory() does NOT exist
# yet — this is the TDD RED step and will FAIL TO COMPILE until subtask 3
# implements it (and adds the TSDEV_CLI_TEST guard around main()).
add_executable(test_directory_batch tests/test_directory_batch.cpp)
target_include_directories(test_directory_batch PRIVATE third_party)
target_link_libraries(test_directory_batch tsdev_lib)
add_dependencies(test_directory_batch siggen)

# --- GET-SOURCE integration test (TVS-F.4, TDD red-green) ---------------------
# Validates the library-level source-retrieval primitive (getSource) that the
# future `get-source` CLI subcommand will wrap. Inserts a probe strategy and a
# probe function with KNOWN source text, calls getSource to retrieve the source
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

# --- TVS-D.1: Batch feasibility stress test (opt-in, NOT in regression suite) -
# Validates ONE TsDevLib instance can do N (default 600) sequential
# insert+compile+verify ops then delete all N, leaving a clean warehouse, all
# in ONE process on ONE 64 MB-stack worker thread. De-risks batch CLI (TVS-D.3).
# Heavy/slow and hits the real warehouse — run manually, NOT via run_regression:
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

# --- Admin-privilege manifest for TS-accessing executables -------------------
# When ON, every executable that accesses the TradeStation TSDev Library (which
# requires admin privileges, because TSDev.exe and TradeStation Desktop always
# run elevated) embeds requestedExecutionLevel="requireAdministrator" in its
# application manifest. Windows then refuses to start these executables without
# an elevated (administrator) token. When the PARENT process is already elevated
# (elevated Python wrapper / test runner / shell), child processes inherit that
# token silently with NO per-run UAC prompt — one UAC prompt per session.
# When OFF, manifests default to asInvoker (no elevation requirement).
#
# CI builds with the default ON; this is safe because CI tests never invoke
# tsdev_cli.exe or any TS-dependent test executable (the -NoTs suites use only
# non-TS unit tests + sigtool; non-e2e Python tests mock subprocess.run).
# Developers who need a non-elevated build pass -DTSDEV_REQUIRE_ADMIN=OFF.
option(TSDEV_REQUIRE_ADMIN
    "Embed requireAdministrator execution level in TS-accessing executables" ON)

# All executables that access the TSDev Library / orlib20.dll at runtime and
# therefore require admin privileges. Excludes: sigtool (static DLL file
# analysis, no runtime orlib20 access), test_no_hardcoded_rvas / test_tradingapp_type
# / test_showme_type / test_orlib_resolver / test_elevation_guard
# / test_crash_handler / test_json_serialize / test_serve_dispatch
# / test_serve_framing / test_com_apartment_guard / test_tsdev_com_init_seam
# (pure unit tests, no TSDev Library access, run in CI under -NoTs from a
# non-elevated runner).
set(TSDEV_ADMIN_TARGETS
    tsdev_cli
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
    test_directory_batch
    test_get_source
    test_auto_cleanup
    test_stress_batch
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
# 184 assertions across 6 TDD cycles.
add_executable(test_orlib_resolver tests/test_orlib_resolver.cpp)
target_link_libraries(test_orlib_resolver PRIVATE orlib_resolver)

# --- Master regression build dependency target (TVS-B.6.3) -------------------
# This is a build-only dependency target: it ensures ALL test executables,
# the production CLI, and sigtool are built when invoked via:
#   cmake --build build --config Release --target test_regression
# The actual regression test execution is performed by the PowerShell
# orchestrator script tests/run_regression.ps1 (which runs sigtool verify,
# all 27 C++ unit tests, and the 34 E2E CLI tests). This target adds NO
# build commands of its own - it merely aggregates the dependencies.
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
    test_serve_dispatch
    test_serve_framing
    test_com_apartment_guard
    test_persistence_integrity
    test_create_path_cleanup
    test_delete_disk_cleanup
    test_insert_tradingapp
    test_modify_tradingapp
    test_exists
    test_enumerate
    test_upsert
    test_batch_upsert
    test_directory_batch
    test_get_source
    test_auto_cleanup
    test_orlib_resolver
    tsdev_cli
    sigtool
)
