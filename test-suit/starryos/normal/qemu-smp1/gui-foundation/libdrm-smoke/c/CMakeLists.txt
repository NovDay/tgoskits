cmake_minimum_required(VERSION 3.20)
project(libdrm-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(libdrm-smoke src/main.c)
target_compile_options(libdrm-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(libdrm-smoke PRIVATE PkgConfig::LIBDRM)

install(TARGETS libdrm-smoke RUNTIME DESTINATION usr/bin)
