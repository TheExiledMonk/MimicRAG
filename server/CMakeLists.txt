add_executable(mimicdb_server
    mimicdb_server.cpp
)

target_link_libraries(mimicdb_server PRIVATE mimicdb_engine)
target_compile_options(mimicdb_server PRIVATE -Wall -Wextra -Wpedantic)
