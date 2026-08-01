add_library(mimicdb_engine
    src/lib.cpp
    src/batch.cpp
    src/bitmap.cpp
    src/array_codec.cpp
    src/compression.cpp
    src/field_vector.cpp
    src/dataset.cpp
    src/schema.cpp
    src/segment.cpp
    src/segment_io.cpp
    src/hash.cpp
    src/mask.cpp
    src/scan.cpp
    src/simd_dispatch.cpp
    src/simd_predicate.cpp
    src/simd_aggregate.cpp
    src/predicate.cpp
    src/projection.cpp
    src/aggregate.cpp
    src/metrics.cpp
    src/dictionary.cpp
    src/output_compaction.cpp
    src/simd_output.cpp
    src/vector_search.cpp
    src/vector_gpu.cpp
)

target_include_directories(mimicdb_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(mimicdb_engine PUBLIC cxx_std_20)

target_compile_options(mimicdb_engine PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mimicdb_engine PUBLIC mimicdb_build_flags)

find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)
if(Vulkan_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(SHADERC QUIET IMPORTED_TARGET shaderc)
endif()
if(Vulkan_FOUND AND TARGET PkgConfig::SHADERC)
    target_compile_definitions(mimicdb_engine PRIVATE MIMICDB_HAS_VULKAN=1)
    target_link_libraries(mimicdb_engine PRIVATE Vulkan::Vulkan PkgConfig::SHADERC)
else()
    target_compile_definitions(mimicdb_engine PRIVATE MIMICDB_HAS_VULKAN=0)
    message(STATUS "MimicDB Vulkan vector backend disabled (Vulkan and shaderc required)")
endif()
