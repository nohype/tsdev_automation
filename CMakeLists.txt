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
add_library(tsdev_lib STATIC src/tsdev_lib.cpp)
target_include_directories(tsdev_lib PUBLIC src)
target_link_libraries(tsdev_lib PUBLIC orlib_resolver)

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

# --- Persistence patch byte validation test (TVS-B.1.5) ---------------------
add_executable(test_persistence_validation tests/test_persistence_validation.cpp)
target_link_libraries(test_persistence_validation tsdev_lib)

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
target_link_libraries(test_stress_batch tsdev_lib)

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
# / test_serve_framing (pure unit tests, no TSDev Library access, run in CI
# under -NoTs from a non-elevated runner).
set(TSDEV_ADMIN_TARGETS
    tsdev_cli
    test_insert
    test_delete
    test_delete_tradingapp
    test_delete_check
    test_delete_diag
    test_path_discovery
    test_modify
    test_persistence_validation
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
# all 26 C++ unit tests, and the 34 E2E CLI tests). This target adds NO
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
    test_persistence_validation
    test_no_hardcoded_rvas
    test_tradingapp_type
    test_showme_type
    test_elevation_guard
    test_crash_handler
    test_json_serialize
    test_serve_dispatch
    test_serve_framing
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
