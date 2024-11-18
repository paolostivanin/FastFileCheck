cmake_minimum_required(VERSION 3.10)

project(FastFileCheck VERSION 0.99.0 LANGUAGES C)

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

include_directories(${XXHASH_INCLUDE_DIRS} ${LMDB_INCLUDE_DIRS})

add_executable(FastFileCheck
        src/main.c
        src/config.c
        src/database.c
)

target_link_libraries(FastFileCheck ${XXHASH_LIBRARIES} ${LMDB_LIBRARIES})

target_compile_options(FastFileCheck PRIVATE -Wall -Wextra -O3)
