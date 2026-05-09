cmake_minimum_required(VERSION 3.10)
project(fd-ipc-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(fd-ipc-smoke src/main.c)
install(TARGETS fd-ipc-smoke DESTINATION usr/bin)
