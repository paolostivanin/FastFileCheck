cmake_minimum_required(VERSION 3.10)

project(FastFileCheck VERSION 0.99.2 LANGUAGES C)

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/src/version.h
        @ONLY
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(COMMON_C_FLAGS
        "-Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong"
        "-Wundef -Wmissing-format-attribute -fdiagnostics-color=always"
        "-Wstrict-prototypes -Wunreachable-code -Wchar-subscripts"
        "-Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align"
        "-Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare"
        "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3"
)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    list(APPEND COMMON_C_FLAGS "-pie -fPIE")
endif()

string(REPLACE ";" " " COMMON_C_FLAGS "${COMMON_C_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_C_FLAGS}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED libxxhash)
pkg_check_modules(LMDB REQUIRED lmdb)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.68.0)
pkg_check_modules(GIO REQUIRED gio-2.0>=2.68.0)

include_directories(${XXHASH_INCLUDE_DIRS} ${LMDB_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS} ${GIO_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}/src)

add_executable(${PROJECT_NAME}
        src/main.c
        src/config.c
        src/database.c
        src/process_directories.c
        src/queue.c
        src/process_file.c
)

target_link_libraries(${PROJECT_NAME} ${XXHASH_LIBRARIES} ${LMDB_LIBRARIES} ${GLIB2_LIBRARIES} ${GIO_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O3)
