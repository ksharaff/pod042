cmake_minimum_required(VERSION 3.16)
project(pod042 CXX)

set(CMAKE_CXX_STANDARD 20)

# Download nlohmann/json (header-only JSON library) when CMake configures.
include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(pod042 main.cpp meminfo.cpp cpu.cpp disk.cpp net.cpp log.cpp chips.cpp loadout.cpp)

# Common warnings, so mistakes show up at build time.
target_compile_options(pod042 PRIVATE -Wall -Wextra)

# Makes "#include <nlohmann/json.hpp>" work for this target.
target_link_libraries(pod042 PRIVATE nlohmann_json::nlohmann_json)