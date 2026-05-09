cmake_minimum_required(VERSION 3.20)
project(wayland-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-server)

add_executable(wayland-smoke src/main.c)
target_compile_options(wayland-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(wayland-smoke PRIVATE PkgConfig::WAYLAND)

install(TARGETS wayland-smoke RUNTIME DESTINATION usr/bin)
