cmake_minimum_required(VERSION 3.20)
project(mimicdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MIMICDB_BUILD_TESTS "Build MimicDB tests" ON)

add_subdirectory(engine)
add_subdirectory(server)
add_subdirectory(api_cpp)

if(MIMICDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
