cmake_minimum_required(VERSION 3.20)
project(weston-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

if(NOT DEFINED STARRY_STAGING_ROOT)
    message(FATAL_ERROR "STARRY_STAGING_ROOT must be provided by axbuild")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput libudev)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(XDG_SHELL_XML "${STARRY_STAGING_ROOT}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
set(XDG_SHELL_CODE "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")

add_custom_command(
  OUTPUT "${XDG_SHELL_HEADER}"
  COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)

add_custom_command(
  OUTPUT "${XDG_SHELL_CODE}"
  COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_CODE}"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)

add_executable(weston-smoke src/main.c "${XDG_SHELL_CODE}" "${XDG_SHELL_HEADER}")
target_compile_options(weston-smoke PRIVATE -Wall -Wextra -Werror)
target_include_directories(weston-smoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(weston-smoke PRIVATE PkgConfig::WAYLAND PkgConfig::LIBINPUT)

install(TARGETS weston-smoke RUNTIME DESTINATION usr/bin)

function(install_flattened_files source_dir dest_dir)
    file(GLOB entries "${source_dir}/*")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${entry}")
            continue()
        endif()
        get_filename_component(name "${entry}" NAME)
        if(IS_SYMLINK "${entry}")
            get_filename_component(real_path "${entry}" REALPATH)
            install(FILES "${real_path}" DESTINATION "${dest_dir}" RENAME "${name}")
        else()
            install(FILES "${entry}" DESTINATION "${dest_dir}")
        endif()
    endforeach()
endfunction()

function(install_runtime_path source_path dest_dir)
    if(NOT EXISTS "${source_path}")
        return()
    endif()
    get_filename_component(name "${source_path}" NAME)
    if(IS_SYMLINK "${source_path}")
        get_filename_component(real_path "${source_path}" REALPATH)
        install(FILES "${real_path}" DESTINATION "${dest_dir}" RENAME "${name}")
    else()
        install(FILES "${source_path}" DESTINATION "${dest_dir}")
    endif()
endfunction()

set(WESTON_BIN "${STARRY_STAGING_ROOT}/usr/bin/weston")
if(NOT EXISTS "${WESTON_BIN}")
    message(FATAL_ERROR "required runtime binary not found: ${WESTON_BIN}")
endif()
install(PROGRAMS "${WESTON_BIN}" DESTINATION usr/bin)

set(LIBDW_SONAME "${STARRY_STAGING_ROOT}/usr/lib/libdw.so.1")
install_runtime_path("${LIBDW_SONAME}" usr/lib)

set(WESTON_LIBEXEC "${STARRY_STAGING_ROOT}/usr/libexec/weston")
if(EXISTS "${WESTON_LIBEXEC}")
    install_flattened_files("${WESTON_LIBEXEC}" usr/libexec/weston)
endif()

set(WESTON_LIB_DIR "${STARRY_STAGING_ROOT}/usr/lib/weston")
if(EXISTS "${WESTON_LIB_DIR}")
    install_flattened_files("${WESTON_LIB_DIR}" usr/lib/weston)
endif()

set(LIBWESTON_MODULE_DIR "${STARRY_STAGING_ROOT}/usr/lib/libweston-14")
if(EXISTS "${LIBWESTON_MODULE_DIR}")
    install_flattened_files("${LIBWESTON_MODULE_DIR}" usr/lib/libweston-14)
endif()

set(MESA_GBM_DIR "${STARRY_STAGING_ROOT}/usr/lib/gbm")
if(EXISTS "${MESA_GBM_DIR}")
    install_flattened_files("${MESA_GBM_DIR}" usr/lib/gbm)
endif()

set(MESA_DRI_DIR "${STARRY_STAGING_ROOT}/usr/lib/dri")
if(EXISTS "${MESA_DRI_DIR}")
    install_flattened_files("${MESA_DRI_DIR}" usr/lib/dri)
endif()

set(MESA_DRIRC_DIR "${STARRY_STAGING_ROOT}/usr/share/drirc.d")
if(EXISTS "${MESA_DRIRC_DIR}")
    install(DIRECTORY "${MESA_DRIRC_DIR}/" DESTINATION usr/share/drirc.d)
endif()

set(WESTON_SHARE_DIR "${STARRY_STAGING_ROOT}/usr/share/weston")
if(EXISTS "${WESTON_SHARE_DIR}")
    install_flattened_files("${WESTON_SHARE_DIR}" usr/share/weston)
endif()

set(XKB_CONFIG_DIR "${STARRY_STAGING_ROOT}/usr/share/X11/xkb")
if(EXISTS "${XKB_CONFIG_DIR}")
    install(DIRECTORY "${XKB_CONFIG_DIR}/" DESTINATION usr/share/xkeyboard-config-2)
endif()

set(LIBINPUT_DATA_DIR "${STARRY_STAGING_ROOT}/usr/share/libinput")
if(EXISTS "${LIBINPUT_DATA_DIR}")
    install(DIRECTORY "${LIBINPUT_DATA_DIR}/" DESTINATION usr/share/libinput)
endif()
