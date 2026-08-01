add_executable(mimicdb_server
    mimicdb_server.cpp
)

find_package(OpenSSL REQUIRED)

target_link_libraries(mimicdb_server PRIVATE mimicdb_engine mimicdb_build_flags OpenSSL::Crypto)
target_compile_options(mimicdb_server PRIVATE -Wall -Wextra -Wpedantic)
