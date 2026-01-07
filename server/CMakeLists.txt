add_executable(pcdb_server
    pcdb_server.cpp
)

target_link_libraries(pcdb_server PRIVATE pcdb_engine)
target_compile_options(pcdb_server PRIVATE -Wall -Wextra -Wpedantic)
