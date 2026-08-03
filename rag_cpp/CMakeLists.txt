find_package(CURL REQUIRED)
find_package(BZip2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

if(MIMICRAG_ENABLE_LLAMA)
    if(MIMICRAG_LLAMA_GPU STREQUAL "auto")
        find_program(MIMICRAG_NVCC nvcc)
        find_package(Vulkan QUIET)
        if(MIMICRAG_NVCC)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            message(STATUS "MimicRAG local embeddings: CUDA with CPU fallback")
        elseif(Vulkan_FOUND)
            set(GGML_VULKAN ON CACHE BOOL "" FORCE)
            message(STATUS "MimicRAG local embeddings: Vulkan with CPU fallback")
        elseif(APPLE)
            set(GGML_METAL ON CACHE BOOL "" FORCE)
            message(STATUS "MimicRAG local embeddings: Metal with CPU fallback")
        else()
            message(STATUS "MimicRAG local embeddings: CPU")
        endif()
    elseif(MIMICRAG_LLAMA_GPU STREQUAL "cuda")
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
    elseif(MIMICRAG_LLAMA_GPU STREQUAL "vulkan")
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    elseif(MIMICRAG_LLAMA_GPU STREQUAL "metal")
        set(GGML_METAL ON CACHE BOOL "" FORCE)
    endif()
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/llama.cpp/CMakeLists.txt")
        find_package(Git REQUIRED)
        message(STATUS "Initializing llama.cpp submodule")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --depth 1 external/llama.cpp
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            RESULT_VARIABLE submodule_status)
        if(NOT submodule_status EQUAL 0)
            message(FATAL_ERROR "Unable to initialize llama.cpp submodule; run git submodule update --init --recursive")
        endif()
    endif()
    add_subdirectory("${PROJECT_SOURCE_DIR}/external/llama.cpp" "${CMAKE_BINARY_DIR}/llama.cpp" EXCLUDE_FROM_ALL)
endif()

add_executable(mimicrag_server
    src/main.cpp
    src/config.cpp
    src/catalog.cpp
    src/operations.cpp
    src/provider.cpp
    src/local_embeddings.cpp
    src/rag_engine.cpp
    src/wiki_importer.cpp
    src/http_server.cpp)
target_include_directories(mimicrag_server PRIVATE include)
target_link_libraries(mimicrag_server PRIVATE
    mimicdb_engine mimicdb_build_flags CURL::libcurl BZip2::BZip2 PkgConfig::ZSTD nlohmann_json::nlohmann_json Threads::Threads)
if(MIMICRAG_ENABLE_LLAMA)
    target_compile_definitions(mimicrag_server PRIVATE MIMICRAG_HAVE_LLAMA=1)
    target_link_libraries(mimicrag_server PRIVATE llama)
    add_executable(mimicrag_local_embedding_smoke
        tests/local_embedding_smoke.cpp src/local_embeddings.cpp)
    target_include_directories(mimicrag_local_embedding_smoke PRIVATE include)
    target_compile_definitions(mimicrag_local_embedding_smoke PRIVATE MIMICRAG_HAVE_LLAMA=1)
    target_link_libraries(mimicrag_local_embedding_smoke PRIVATE llama)
endif()
target_compile_options(mimicrag_server PRIVATE -Wall -Wextra -Wpedantic)

if(MIMICDB_BUILD_BENCHMARKS AND MIMICRAG_ENABLE_LLAMA)
    add_executable(mimicrag_benchmark
        ${PROJECT_SOURCE_DIR}/benchmarks/rag_cpp_benchmark.cpp
        src/config.cpp src/catalog.cpp src/provider.cpp src/local_embeddings.cpp src/rag_engine.cpp)
    target_include_directories(mimicrag_benchmark PRIVATE include)
    target_compile_definitions(mimicrag_benchmark PRIVATE MIMICRAG_HAVE_LLAMA=1)
    target_link_libraries(mimicrag_benchmark PRIVATE
        mimicdb_engine mimicdb_build_flags CURL::libcurl PkgConfig::ZSTD nlohmann_json::nlohmann_json Threads::Threads llama)
    target_compile_options(mimicrag_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MIMICDB_BUILD_TESTS)
    add_executable(mimicrag_catalog_smoke tests/catalog_smoke.cpp src/catalog.cpp)
    target_include_directories(mimicrag_catalog_smoke PRIVATE include)
    target_link_libraries(mimicrag_catalog_smoke PRIVATE PkgConfig::ZSTD nlohmann_json::nlohmann_json)
    add_test(NAME mimicrag_catalog_smoke COMMAND mimicrag_catalog_smoke)
    add_test(NAME mimicrag_wikipedia_importer
        COMMAND $<TARGET_FILE:mimicrag_server> wiki-ingest ${PROJECT_SOURCE_DIR}/tests/fixtures/enwiki_tiny.xml
                --config ${PROJECT_SOURCE_DIR}/tests/mimicrag_cpp_test.json --dry-run --no-resume --progress 0)
    set_tests_properties(mimicrag_wikipedia_importer PROPERTIES PASS_REGULAR_EXPRESSION "\"imported\": 2")
    add_test(NAME mimicrag_cpp_smoke
        COMMAND ${PROJECT_SOURCE_DIR}/scripts/mimicrag_cpp_smoke_test.sh
                $<TARGET_FILE:mimicrag_server> ${PROJECT_SOURCE_DIR}/tests/mimicrag_cpp_test.json)
    add_test(NAME mimicrag_v11_smoke
        COMMAND ${PROJECT_SOURCE_DIR}/scripts/mimicrag_v11_smoke_test.sh
                $<TARGET_FILE:mimicrag_server> ${PROJECT_SOURCE_DIR}/tests/mimicrag_v11_test.json)
    add_executable(mimicrag_provider_stream_smoke tests/provider_stream_smoke.cpp src/provider.cpp src/config.cpp)
    target_include_directories(mimicrag_provider_stream_smoke PRIVATE include)
    target_link_libraries(mimicrag_provider_stream_smoke PRIVATE CURL::libcurl nlohmann_json::nlohmann_json Threads::Threads)
    add_test(NAME mimicrag_provider_stream_smoke COMMAND mimicrag_provider_stream_smoke)
    add_test(NAME mimicrag_cpp_evaluate_cli
        COMMAND $<TARGET_FILE:mimicrag_server> evaluate ${PROJECT_SOURCE_DIR}/tests/mimicrag_cpp_golden.json
                --config ${PROJECT_SOURCE_DIR}/tests/mimicrag_cpp_test.json)
    set_tests_properties(mimicrag_cpp_evaluate_cli PROPERTIES DEPENDS mimicrag_cpp_smoke PASS_REGULAR_EXPRESSION "\"recall_at_k\": 1.0")
endif()
