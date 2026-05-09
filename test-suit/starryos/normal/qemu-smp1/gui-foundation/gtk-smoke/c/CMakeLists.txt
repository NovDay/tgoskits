cmake_minimum_required(VERSION 3.20)
project(gtk-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(gtk-smoke src/main.c)
target_compile_options(gtk-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gtk-smoke PRIVATE PkgConfig::GTK4)

install(TARGETS gtk-smoke RUNTIME DESTINATION usr/bin)
