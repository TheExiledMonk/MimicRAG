add_executable(pcdb_test_smoke
    test_smoke.cpp
)

target_link_libraries(pcdb_test_smoke PRIVATE pcdb_engine)

target_compile_features(pcdb_test_smoke PRIVATE cxx_std_20)

target_compile_options(pcdb_test_smoke PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_smoke COMMAND pcdb_test_smoke)

add_executable(pcdb_test_hash
    test_hash.cpp
)

target_link_libraries(pcdb_test_hash PRIVATE pcdb_engine)

target_compile_features(pcdb_test_hash PRIVATE cxx_std_20)

target_compile_options(pcdb_test_hash PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_hash COMMAND pcdb_test_hash)

add_executable(pcdb_test_mask
    test_mask.cpp
)

target_link_libraries(pcdb_test_mask PRIVATE pcdb_engine)

target_compile_features(pcdb_test_mask PRIVATE cxx_std_20)

target_compile_options(pcdb_test_mask PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_mask COMMAND pcdb_test_mask)

add_executable(pcdb_test_queries
    test_queries.cpp
)

target_link_libraries(pcdb_test_queries PRIVATE pcdb_engine)

target_compile_features(pcdb_test_queries PRIVATE cxx_std_20)

target_compile_options(pcdb_test_queries PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_queries COMMAND pcdb_test_queries)

add_executable(pcdb_test_field_vector
    test_field_vector.cpp
)

target_link_libraries(pcdb_test_field_vector PRIVATE pcdb_engine)

target_compile_features(pcdb_test_field_vector PRIVATE cxx_std_20)

target_compile_options(pcdb_test_field_vector PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_field_vector COMMAND pcdb_test_field_vector)

add_executable(pcdb_test_segment_io
    test_segment_io.cpp
)

target_link_libraries(pcdb_test_segment_io PRIVATE pcdb_engine)

target_compile_features(pcdb_test_segment_io PRIVATE cxx_std_20)

target_compile_options(pcdb_test_segment_io PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_segment_io COMMAND pcdb_test_segment_io)

add_executable(pcdb_test_scan
    test_scan.cpp
)

target_link_libraries(pcdb_test_scan PRIVATE pcdb_engine)

target_compile_features(pcdb_test_scan PRIVATE cxx_std_20)

target_compile_options(pcdb_test_scan PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_scan COMMAND pcdb_test_scan)

add_executable(pcdb_test_simd
    test_simd.cpp
)

target_link_libraries(pcdb_test_simd PRIVATE pcdb_engine)

target_compile_features(pcdb_test_simd PRIVATE cxx_std_20)

target_compile_options(pcdb_test_simd PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME pcdb_test_simd COMMAND pcdb_test_simd)
