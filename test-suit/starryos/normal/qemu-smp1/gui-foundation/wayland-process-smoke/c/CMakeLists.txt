cmake_minimum_required(VERSION 3.20)
project(wayland-process-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-server)

add_executable(wayland-process-smoke src/main.c)
target_compile_options(wayland-process-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(wayland-process-smoke PRIVATE PkgConfig::WAYLAND)

install(TARGETS wayland-process-smoke RUNTIME DESTINATION usr/bin)
