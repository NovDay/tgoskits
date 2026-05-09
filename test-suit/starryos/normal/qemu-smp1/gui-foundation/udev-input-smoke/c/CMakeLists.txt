cmake_minimum_required(VERSION 3.20)
project(udev-input-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

if(NOT DEFINED STARRY_STAGING_ROOT)
    message(FATAL_ERROR "STARRY_STAGING_ROOT must be provided by axbuild")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)

add_executable(udev-input-smoke src/main.c)
target_compile_options(udev-input-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(udev-input-smoke PRIVATE PkgConfig::UDEV PkgConfig::LIBINPUT)

install(TARGETS udev-input-smoke RUNTIME DESTINATION usr/bin)

set(LIBINPUT_SHARE_DIR "${STARRY_STAGING_ROOT}/usr/share/libinput")
if(NOT EXISTS "${LIBINPUT_SHARE_DIR}")
    message(FATAL_ERROR "required libinput data directory not found: ${LIBINPUT_SHARE_DIR}")
endif()
install(DIRECTORY "${LIBINPUT_SHARE_DIR}" DESTINATION usr/share)
