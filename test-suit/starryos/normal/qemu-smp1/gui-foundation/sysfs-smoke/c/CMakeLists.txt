cmake_minimum_required(VERSION 3.10)
project(sysfs-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(sysfs-smoke src/main.c)
target_compile_options(sysfs-smoke PRIVATE -Wall -Wextra -Werror)
install(TARGETS sysfs-smoke DESTINATION usr/bin)
