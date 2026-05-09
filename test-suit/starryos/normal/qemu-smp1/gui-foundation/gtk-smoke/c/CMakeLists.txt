cmake_minimum_required(VERSION 3.20)
project(gtk-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(XDG_SHELL_XML "${STARRY_STAGING_ROOT}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h")
set(XDG_SHELL_CODE "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")

add_custom_command(
  OUTPUT "${XDG_SHELL_HEADER}"
  COMMAND "${WAYLAND_SCANNER}" server-header "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)

add_custom_command(
  OUTPUT "${XDG_SHELL_CODE}"
  COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_CODE}"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)

add_executable(gtk-smoke src/main.c "${XDG_SHELL_CODE}" "${XDG_SHELL_HEADER}")
target_compile_options(gtk-smoke PRIVATE -Wall -Wextra -Werror)
target_include_directories(gtk-smoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gtk-smoke PRIVATE PkgConfig::GTK4 PkgConfig::WAYLAND_SERVER)

install(TARGETS gtk-smoke RUNTIME DESTINATION usr/bin)
