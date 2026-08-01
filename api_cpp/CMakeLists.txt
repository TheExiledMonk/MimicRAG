add_library(mimicapi_core STATIC
    src/mimicapi_core.cpp
)

target_include_directories(mimicapi_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/engine/include
)

target_link_libraries(mimicapi_core
    PUBLIC
        mimicdb_engine
        mimicdb_build_flags
)
