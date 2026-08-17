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
# derived from it (JSON -> generated header -> test), per ARCHITECTURE_REDESIGN
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

# --- Production CLI ---------------------------------------------------------
add_executable(tsdev_cli src/tsdev_cli.cpp)
target_link_libraries(tsdev_cli tsdev_lib)

# --- sigtool: standalone orlib20.dll signature verification tool --------------
# CLI for verifying signatures, validating patch sites, inspecting DLL info,
# ad-hoc pattern scanning, and version-manifest lookup.
add_executable(sigtool src/sigtool.cpp)
target_include_directories(sigtool PRIVATE third_party)
target_link_libraries(sigtool PRIVATE orlib_resolver)
add_dependencies(sigtool siggen)

# --- OrlibResolver library (TVS-B.4 scaffolding) ----------------------------
# Static library encapsulating all version-specific orlib20.dll knowledge.
# Stubs only at this stage; real implementation in subsequent TDD cycles.
add_library(orlib_resolver STATIC src/orlib_resolver.cpp)
target_include_directories(orlib_resolver PUBLIC src)
# Generated header include dir is PUBLIC so consumers (tsdev_lib, tests) that
# include orlib_resolver.h can also find signatures.gen.h (which defines the
# SignatureSetId enum before including orlib_resolver.h).
target_include_directories(orlib_resolver PUBLIC ${GEN_INCLUDE_DIR})
add_dependencies(orlib_resolver siggen)
target_link_libraries(orlib_resolver PUBLIC bcrypt)

# --- OrlibResolver test executable (TVS-B.4 scaffolding) ---------------------
# Placeholder main(); real tests added in B.4.3.
add_executable(test_orlib_resolver tests/test_orlib_resolver.cpp)
target_link_libraries(test_orlib_resolver PRIVATE orlib_resolver)

# --- Master regression build dependency target (TVS-B.6.3) -------------------
# This is a build-only dependency target: it ensures ALL test executables,
# the production CLI, and sigtool are built when invoked via:
#   cmake --build build --config Release --target test_regression
# The actual regression test execution is performed by the PowerShell
# orchestrator script tests/run_regression.ps1 (which runs sigtool verify,
# all 13 C++ unit tests, and the 34 E2E CLI tests). This target adds NO
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
    test_insert_tradingapp
    test_modify_tradingapp
    test_orlib_resolver
    tsdev_cli
    sigtool
)
