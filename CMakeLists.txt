cmake_minimum_required(VERSION 3.10)
project(Telegarrm VERSION 0.1.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(include)

# Find threads (needed by cpp-httplib)
find_package(Threads REQUIRED)

# Executable
add_executable(telegarrm src/main.cpp)

# Link libraries
target_link_libraries(telegarrm PRIVATE Threads::Threads)

# Note: In the future, we will add TDLib and SQLite3 linking here.
