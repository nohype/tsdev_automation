cmake_minimum_required(VERSION 3.20)
project(tsdev_automation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /EHsc)
endif()

add_library(tsdevlib STATIC
    src/tsdevlib/args.cpp
    src/tsdevlib/busy_guard.cpp
    src/tsdevlib/dialog_check.cpp
    src/tsdevlib/doc_read.cpp
    src/tsdevlib/elevation.cpp
    src/tsdevlib/json_writer.cpp
    src/tsdevlib/pipe_client.cpp
    src/tsdevlib/type_table.cpp
    src/tsdevlib/tsdev_process.cpp
    src/tsdevlib/window_watch.cpp
)
target_include_directories(tsdevlib PUBLIC src)

add_executable(tsdevctl src/tsdevctl/main.cpp)
target_link_libraries(tsdevctl PRIVATE tsdevlib)

add_executable(test_tsdevctl_units tests/test_tsdevctl_units.cpp)
target_link_libraries(test_tsdevctl_units PRIVATE tsdevlib)
