cmake_minimum_required(VERSION 3.20)
project(dbus-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

if(NOT DEFINED STARRY_STAGING_ROOT)
    message(FATAL_ERROR "STARRY_STAGING_ROOT must be provided by axbuild")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

add_executable(dbus-smoke src/main.c)
target_compile_options(dbus-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dbus-smoke PRIVATE PkgConfig::DBUS)

install(TARGETS dbus-smoke RUNTIME DESTINATION usr/bin)

set(DBUS_DAEMON "${STARRY_STAGING_ROOT}/usr/bin/dbus-daemon")
if(NOT EXISTS "${DBUS_DAEMON}")
    message(FATAL_ERROR "required runtime binary not found: ${DBUS_DAEMON}")
endif()
install(PROGRAMS "${DBUS_DAEMON}" DESTINATION usr/bin)

set(DBUS_SHARE_DIR "${STARRY_STAGING_ROOT}/usr/share/dbus-1")
if(NOT EXISTS "${DBUS_SHARE_DIR}")
    message(FATAL_ERROR "required D-Bus config directory not found: ${DBUS_SHARE_DIR}")
endif()
install(DIRECTORY "${DBUS_SHARE_DIR}" DESTINATION usr/share)

set(DBUS_ETC_DIR "${STARRY_STAGING_ROOT}/etc/dbus-1")
if(EXISTS "${DBUS_ETC_DIR}")
    install(DIRECTORY "${DBUS_ETC_DIR}" DESTINATION etc)
endif()
