cmake_minimum_required(VERSION 3.20)
project(pcdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PCDB_BUILD_TESTS "Build PCDB tests" ON)

add_subdirectory(engine)
add_subdirectory(server)

if(PCDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
