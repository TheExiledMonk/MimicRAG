if(MIMICDB_ENABLE_LICENSE_CHECK)
    if(NOT MIMICDB_LICENSE_URL)
        message(FATAL_ERROR "MIMICDB_LICENSE_URL must be set when MIMICDB_ENABLE_LICENSE_CHECK=ON")
    endif()
    find_package(CURL REQUIRED)

    # This is informational: the runtime check remains authoritative because
    # the binary may be built on one machine and deployed on another.
    if(APPLE)
        execute_process(COMMAND sysctl -n kern.uuid
                        OUTPUT_VARIABLE MIMICDB_BUILD_MACHINE_MATERIAL
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    elseif(UNIX)
        foreach(machine_id_path /etc/machine-id /var/lib/dbus/machine-id)
            if(NOT MIMICDB_BUILD_MACHINE_MATERIAL AND EXISTS "${machine_id_path}")
                file(READ "${machine_id_path}" MIMICDB_BUILD_MACHINE_MATERIAL)
                string(STRIP "${MIMICDB_BUILD_MACHINE_MATERIAL}" MIMICDB_BUILD_MACHINE_MATERIAL)
            endif()
        endforeach()
    endif()
    if(MIMICDB_BUILD_MACHINE_MATERIAL)
        string(SHA256 MIMICDB_BUILD_LICENSE_ID
               "mimicdb-license-v1:${MIMICDB_BUILD_MACHINE_MATERIAL}")
        message(STATUS "MimicDB license code for this build machine: ${MIMICDB_BUILD_LICENSE_ID}")
    else()
        message(WARNING "Could not determine the build machine license code; use --print-license-id on the target host")
    endif()
endif()

add_executable(mimicdb_server
    mimicdb_server.cpp
)

find_package(OpenSSL REQUIRED)

target_link_libraries(mimicdb_server PRIVATE mimicdb_engine mimicdb_build_flags OpenSSL::Crypto)
if(MIMICDB_ENABLE_LICENSE_CHECK)
    target_sources(mimicdb_server PRIVATE license_check.cpp)
    target_compile_definitions(mimicdb_server PRIVATE
        MIMICDB_ENABLE_LICENSE_CHECK=1
        MIMICDB_LICENSE_URL="${MIMICDB_LICENSE_URL}")
    target_link_libraries(mimicdb_server PRIVATE CURL::libcurl)
    if(MIMICDB_BUILD_LICENSE_ID)
        add_custom_command(TARGET mimicdb_server PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                "MimicDB license code for this build machine: ${MIMICDB_BUILD_LICENSE_ID}")
    endif()
endif()
target_compile_options(mimicdb_server PRIVATE -Wall -Wextra -Wpedantic)
