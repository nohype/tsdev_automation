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

# --- Executable target -----------------------------------------------------
# Minimal probe placeholder. The real CLI sources will be added here later.
add_executable(tsdev_probe src/tsdev_probe.cpp)

add_executable(live_probe src/live_probe.cpp)
add_executable(elevated_test src/elevated_test.cpp)
add_executable(elevated_test2 src/elevated_test2.cpp)
add_executable(elevated_test3 src/elevated_test3.cpp)
add_executable(elevated_test4 src/elevated_test4.cpp)
add_executable(elevated_test5 src/elevated_test5.cpp)
add_executable(elevated_test6 src/elevated_test6.cpp)
add_executable(elevated_test7 src/elevated_test7.cpp)
add_executable(elevated_test8 src/elevated_test8.cpp)
add_executable(elevated_test9 src/elevated_test9.cpp)
add_executable(elevated_test10 src/elevated_test10.cpp)
add_executable(elevated_test11 src/elevated_test11.cpp)
add_executable(elevated_test12 src/elevated_test12.cpp)
add_executable(elevated_test13 src/elevated_test13.cpp)
add_executable(elevated_test14 src/elevated_test14.cpp)
add_executable(elevated_test15 src/elevated_test15.cpp)
add_executable(elevated_test16 src/elevated_test16.cpp)
add_executable(elevated_test17 src/elevated_test17.cpp)
add_executable(elevated_test18 src/elevated_test18.cpp)
add_executable(elevated_test19 src/elevated_test19.cpp)
add_executable(elevated_test20 src/elevated_test20.cpp)
add_executable(elevated_test21 src/elevated_test21.cpp)
add_executable(elevated_test22 src/elevated_test21.cpp)
add_library(hook_dll SHARED src/hook_dll.cpp)
add_executable(inject src/inject.cpp)

# --- Shared orlib20 wrapper library -----------------------------------------
# Reusable engine for the CLI and tests. Encapsulates the confirmed orlib20
# operation set (init, lookup, create, save, compile/verify, close).
add_library(tsdev_lib STATIC src/tsdev_lib.cpp)
target_include_directories(tsdev_lib PUBLIC src)

# --- INSERT integration test (TDD red-green) --------------------------------
add_executable(test_insert src/test_insert.cpp)
target_link_libraries(test_insert tsdev_lib)

# --- DELETE integration test (TDD red-green) --------------------------------
add_executable(test_delete src/test_delete.cpp)
target_link_libraries(test_delete tsdev_lib)

# --- Quick check if deletions persist across restarts -----------------------
add_executable(test_delete_check src/test_delete_check.cpp)
target_link_libraries(test_delete_check tsdev_lib)

# --- DELETE diagnostic -------------------------------------------------------
add_executable(test_delete_diag src/test_delete_diag.cpp)
target_link_libraries(test_delete_diag tsdev_lib)

# --- MODIFY integration test (TDD red-green) --------------------------------
add_executable(test_modify src/test_modify.cpp)
target_link_libraries(test_modify tsdev_lib)

# --- Production CLI ---------------------------------------------------------
add_executable(tsdev_cli src/tsdev_cli.cpp)
target_link_libraries(tsdev_cli tsdev_lib)
