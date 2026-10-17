cmake_minimum_required(VERSION 3.16)
project(pod042 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download dependencies when CMake configures (this is the Maven-dependency part).
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0)
FetchContent_MakeAvailable(json ftxui)

add_executable(pod042
    main.cpp meminfo.cpp cpu.cpp disk.cpp net.cpp log.cpp chips.cpp loadout.cpp profile.cpp)

# Common warnings, so mistakes show up at build time.
target_compile_options(pod042 PRIVATE -Wall -Wextra)

# We use std::thread for the background refresher.
find_package(Threads REQUIRED)

# FTXUI is split into three libraries: screen (drawing), dom (layout), component (input).
target_link_libraries(pod042 PRIVATE
    nlohmann_json::nlohmann_json
    ftxui::screen ftxui::dom ftxui::component
    Threads::Threads)