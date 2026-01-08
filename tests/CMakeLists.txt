add_executable(mimicdb_test_smoke
    test_smoke.cpp
)

target_link_libraries(mimicdb_test_smoke PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_smoke PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_smoke PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_smoke COMMAND mimicdb_test_smoke)

add_executable(mimicdb_test_hash
    test_hash.cpp
)

target_link_libraries(mimicdb_test_hash PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_hash PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_hash PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_hash COMMAND mimicdb_test_hash)

add_executable(mimicdb_test_mask
    test_mask.cpp
)

target_link_libraries(mimicdb_test_mask PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_mask PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_mask PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_mask COMMAND mimicdb_test_mask)

add_executable(mimicdb_test_queries
    test_queries.cpp
)

target_link_libraries(mimicdb_test_queries PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_queries PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_queries PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_queries COMMAND mimicdb_test_queries)

add_executable(mimicdb_test_field_vector
    test_field_vector.cpp
)

target_link_libraries(mimicdb_test_field_vector PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_field_vector PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_field_vector PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_field_vector COMMAND mimicdb_test_field_vector)

add_executable(mimicdb_test_segment_io
    test_segment_io.cpp
)

target_link_libraries(mimicdb_test_segment_io PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_segment_io PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_segment_io PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_segment_io COMMAND mimicdb_test_segment_io)

add_executable(mimicdb_test_scan
    test_scan.cpp
)

target_link_libraries(mimicdb_test_scan PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_scan PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_scan PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_scan COMMAND mimicdb_test_scan)

add_executable(mimicdb_test_simd
    test_simd.cpp
)

target_link_libraries(mimicdb_test_simd PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_simd PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_simd PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_simd COMMAND mimicdb_test_simd)

add_executable(mimicdb_test_compression
    test_compression.cpp
)

target_link_libraries(mimicdb_test_compression PRIVATE mimicdb_engine)

target_compile_features(mimicdb_test_compression PRIVATE cxx_std_20)

target_compile_options(mimicdb_test_compression PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME mimicdb_test_compression COMMAND mimicdb_test_compression)
