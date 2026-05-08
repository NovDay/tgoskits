cmake_minimum_required(VERSION 3.10)
project(inotify-smoke C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(inotify-smoke src/main.c)
install(TARGETS inotify-smoke DESTINATION usr/bin)
