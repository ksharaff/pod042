cmake_minimum_required(VERSION 3.16)
project(pod042 CXX)

set(CMAKE_CXX_STANDARD 20)

# The executable keeps the short name "pod042" so you type less.
add_executable(pod042 main.cpp meminfo.cpp cpu.cpp disk.cpp net.cpp log.cpp)