cmake_minimum_required(VERSION 3.10)
project(runtime-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(runtime-smoke src/main.c)
target_compile_options(runtime-smoke PRIVATE -Wall -Wextra -Werror)
install(TARGETS runtime-smoke DESTINATION usr/bin)
