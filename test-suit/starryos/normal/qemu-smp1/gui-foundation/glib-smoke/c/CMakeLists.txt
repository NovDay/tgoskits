cmake_minimum_required(VERSION 3.20)
project(glib-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)

if(NOT DEFINED STARRY_STAGING_ROOT)
    message(FATAL_ERROR "STARRY_STAGING_ROOT must be provided by axbuild")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)

add_executable(glib-smoke src/main.c)
target_compile_options(glib-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(glib-smoke PRIVATE PkgConfig::GLIB)

install(TARGETS glib-smoke RUNTIME DESTINATION usr/bin)

function(install_runtime_soname soname)
    set(runtime_lib "${STARRY_STAGING_ROOT}/usr/lib/${soname}")
    if(NOT EXISTS "${runtime_lib}")
        message(FATAL_ERROR "required runtime library not found: ${runtime_lib}")
    endif()
    get_filename_component(runtime_realpath "${runtime_lib}" REALPATH)
    install(FILES "${runtime_realpath}" DESTINATION usr/lib RENAME "${soname}")
endfunction()

install_runtime_soname("libglib-2.0.so.0")
install_runtime_soname("libgobject-2.0.so.0")
install_runtime_soname("libgio-2.0.so.0")
install_runtime_soname("libgmodule-2.0.so.0")
install_runtime_soname("libffi.so.8")
install_runtime_soname("libpcre2-8.so.0")
install_runtime_soname("libz.so.1")
install_runtime_soname("libmount.so.1")
install_runtime_soname("libblkid.so.1")
install_runtime_soname("libintl.so.8")
