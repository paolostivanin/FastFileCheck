cmake_minimum_required(VERSION 3.20)

project(FastFileCheck VERSION 1.0.0 LANGUAGES C)

# Pre-release tag (can be empty)
set(PROJECT_VERSION_SUFFIX "")

# Compose full version (e.g., 1.0.0-beta3). If suffix is empty, it's just the version.
set(PROJECT_VERSION_FULL "${PROJECT_VERSION}${PROJECT_VERSION_SUFFIX}")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/src/version.h
        @ONLY
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

include(GNUInstallDirs)
include(CheckPIESupported)
check_pie_supported(LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED libxxhash)
pkg_check_modules(LMDB REQUIRED lmdb)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.68.0)

add_executable(${PROJECT_NAME}
        src/main.c
        src/config.c
        src/database.c
        src/process_directories.c
        src/queue.c
        src/process_file.c
        src/logging.c
        src/summary.c
        src/db_writer.c
        src/seen_set.c
        src/storage.c
        src/io_sched.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${XXHASH_INCLUDE_DIRS}
        ${LMDB_INCLUDE_DIRS}
        ${GLIB2_INCLUDE_DIRS}
        ${CMAKE_CURRENT_BINARY_DIR}/src
)

# O_NOATIME, FS_IOC_FIEMAP helpers, struct stat nanosecond fields, ... need GNU extensions
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)

# -fPIE at compile time and -pie at link time (CMake handles both)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE TRUE)

target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wformat=2 -Wmissing-format-attribute
        -fstack-protector-strong -Wundef -fdiagnostics-color=always
        -Wstrict-prototypes -Wunreachable-code -Wchar-subscripts
        -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align
        -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare
)

# _FORTIFY_SOURCE needs optimization; CMake's own per-config flags already provide -O3/-g
target_compile_options(${PROJECT_NAME} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>"
)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        ${XXHASH_LIBRARIES}
        ${LMDB_LIBRARIES}
        ${GLIB2_LIBRARIES}
)

install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
# Never clobber an admin's /etc/ffc.conf: ship the sample next to it
install(FILES example.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR} RENAME ffc.conf.example)

# Integration tests: `ctest --test-dir build --output-on-failure`
enable_testing()
add_test(NAME integration
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(integration PROPERTIES TIMEOUT 1200)
