cmake_minimum_required(VERSION 3.16)
project(pod042 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Writes build/compile_commands.json, which VS Code reads for IntelliSense.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Switches you can set on the command line, e.g.:  cmake -B build-asan -DPOD042_SANITIZE=ON
option(POD042_BUILD_TESTS "Build the unit tests" ON)
option(POD042_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Our core code starts threads (std::thread), so link the system thread library.
find_package(Threads REQUIRED)

# ---- Dependencies, downloaded when CMake configures ----
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0)
FetchContent_MakeAvailable(json ftxui)

# GoogleTest is only downloaded if tests are switched on (they are by default).
if(POD042_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---- Settings shared by all OUR targets (not by downloaded libraries) ----
# A function is a reusable block of CMake code. Call it once per target.
function(pod042_configure target)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(POD042_SANITIZE)
        # -fsanitize=address:   catches out-of-bounds access, use-after-free, leaks.
        # -fsanitize=undefined: catches undefined behavior, including a missing return.
        # -fno-omit-frame-pointer: gives readable stack traces in the reports.
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# ---- The core library: everything except main() and the UI ----
add_library(pod042_core STATIC
    meminfo.cpp cpu.cpp disk.cpp net.cpp log.cpp chips.cpp
    loadout.cpp profile.cpp connections.cpp process.cpp
    snapshot.cpp pod.cpp changes.cpp programs.cpp history.cpp host.cpp config.cpp protocol.cpp comms.cpp)

# PUBLIC = anything that links this library also gets these settings.
# The headers sit in the project folder, so the tests can write #include "cpu.h".
target_include_directories(pod042_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pod042_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
pod042_configure(pod042_core)

# ---- The program: just main.cpp + the library + the UI library ----
add_executable(pod042 main.cpp)
target_link_libraries(pod042 PRIVATE
    pod042_core ftxui::screen ftxui::dom ftxui::component)
pod042_configure(pod042)

# Tests are built only if they are switched on AND the test file exists.
if(POD042_HAVE_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
endif()

# ---- The tests ----
if(POD042_HAVE_TESTS)
    enable_testing()   # lets the "ctest" command find and run the tests

    add_executable(pod042_tests tests/test_core.cpp)
    target_link_libraries(pod042_tests PRIVATE pod042_core GTest::gtest_main)
    pod042_configure(pod042_tests)

    include(GoogleTest)
    gtest_discover_tests(pod042_tests)   # registers every TEST() with ctest
endif()

# ---- The tests ----
if(POD042_BUILD_TESTS)
    enable_testing()   # lets the "ctest" command find and run the tests

    add_executable(pod042_tests tests/test_core.cpp)
    target_link_libraries(pod042_tests PRIVATE pod042_core GTest::gtest_main)
    pod042_configure(pod042_tests)

    include(GoogleTest)
    gtest_discover_tests(pod042_tests)   # registers every TEST() with ctest
endif()