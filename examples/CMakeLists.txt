# Standalone installed-library example
# Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

cmake_minimum_required(VERSION 3.16)
project(n148i_consumer_example LANGUAGES C)

find_package(n148i 1 CONFIG REQUIRED)

add_executable(n148i_memory_roundtrip memory_roundtrip.c)
target_link_libraries(n148i_memory_roundtrip PRIVATE n148i::n148i)
