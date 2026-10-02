# N.148i portable library build
# Copyright (c) 2026 Micilini Roll. Licensed under the MIT License.

cmake_minimum_required(VERSION 3.16)

# V2 is the product name; CMake requires a numeric package/ABI version.
project(n148i VERSION 2.0.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(N148I_BUILD_CLI "Build the N.148i command-line program" ON)
option(N148I_BUILD_EXAMPLE "Build the memory API example" ON)
option(N148I_BUILD_VALIDATION "Build the codec validation suite" ON)

set(N148I_SOURCES
    src/n148i.c
    src/header.c
    src/ppm.c
    src/tables.c
    src/dct.c
    src/cpu.c
    src/parallel.c
    src/huffman.c
    src/rans.c
    src/intra.c
    src/perceptual.c
    src/adaptive_quant.c
    src/segmentation.c
    src/variable_transform.c
    src/rdo.c
    src/loop_filter.c
    src/directional_intra.c
    src/entropy_payload.c
    src/chroma_profile.c
    src/luma_profile.c
    src/fidelity_profile.c
    src/entropy_profile.c
    src/encoder.c
    src/decoder.c)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING
        "Minimum supported macOS version")
endif()

if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

function(n148i_configure_library target)
    target_sources(${target} PRIVATE ${N148I_SOURCES})
    target_compile_features(${target} PUBLIC c_std_11)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()

    if(WIN32)
        # The codec uses pthreads on POSIX. The Windows library keeps the same
        # behavior through the tested single-thread fallback.
        target_compile_definitions(${target} PRIVATE N148_NO_THREADS)
    else()
        target_link_libraries(${target} PRIVATE Threads::Threads m)
    endif()
endfunction()

add_library(n148i_static STATIC)
n148i_configure_library(n148i_static)
target_compile_definitions(n148i_static PUBLIC N148I_STATIC_DEFINE)
set_target_properties(n148i_static PROPERTIES
    OUTPUT_NAME n148i
    EXPORT_NAME n148i_static
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/static")

add_library(n148i_shared SHARED)
n148i_configure_library(n148i_shared)
target_compile_definitions(n148i_shared PRIVATE N148I_BUILDING_LIBRARY)
set_target_properties(n148i_shared PROPERTIES
    OUTPUT_NAME n148i
    EXPORT_NAME n148i
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared")

if(WIN32)
    set_target_properties(n148i_static PROPERTIES PREFIX "" SUFFIX ".lib")
    set_target_properties(n148i_shared PROPERTIES
        PREFIX ""
        IMPORT_PREFIX ""
        IMPORT_SUFFIX ".lib")
elseif(UNIX AND NOT APPLE)
    target_link_options(n148i_shared PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/n148i.map")
endif()

add_library(n148i::n148i ALIAS n148i_shared)
add_library(n148i::n148i_static ALIAS n148i_static)

if(N148I_BUILD_CLI)
    add_executable(n148i_cli src/main.c)
    target_include_directories(n148i_cli PRIVATE src)
    target_link_libraries(n148i_cli PRIVATE n148i_static)
    set_target_properties(n148i_cli PROPERTIES OUTPUT_NAME n148i)
endif()

if(N148I_BUILD_EXAMPLE)
    add_executable(n148i_memory_roundtrip examples/memory_roundtrip.c)
    target_link_libraries(n148i_memory_roundtrip PRIVATE n148i_static)
    if(BUILD_TESTING)
        add_test(NAME n148i.memory_roundtrip COMMAND n148i_memory_roundtrip)
    endif()
    add_executable(n148i_shared_roundtrip examples/memory_roundtrip.c)
    target_link_libraries(n148i_shared_roundtrip PRIVATE n148i_shared)
    if(BUILD_TESTING)
        add_test(NAME n148i.shared_roundtrip COMMAND n148i_shared_roundtrip)
        if(WIN32)
            add_custom_command(TARGET n148i_shared_roundtrip POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:n148i_shared>
                    $<TARGET_FILE_DIR:n148i_shared_roundtrip>)
        endif()
    endif()
endif()

if(N148I_BUILD_VALIDATION)
    add_executable(n148i_validate src/validate_cli.c)
    target_include_directories(n148i_validate PRIVATE src)
    target_link_libraries(n148i_validate PRIVATE n148i_static)
    set_target_properties(n148i_validate PROPERTIES OUTPUT_NAME validate)
    if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        add_test(NAME n148i.validation COMMAND n148i_validate)
        set_tests_properties(n148i.validation PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()

configure_file(cmake/n148i.pc.in n148i.pc @ONLY)
configure_package_config_file(
    cmake/n148iConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/n148iConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/n148i")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/n148iConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(TARGETS n148i_shared
    EXPORT n148iTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS n148i_static
    EXPORT n148iTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/static")
install(FILES include/n148i.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION .)
install(FILES "packaging/README-${CMAKE_SYSTEM_NAME}.txt"
    DESTINATION . RENAME README.txt)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/n148i.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/n148iConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/n148iConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/n148i")
install(EXPORT n148iTargets
    FILE n148iTargets.cmake
    NAMESPACE n148i::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/n148i")

if(N148I_BUILD_CLI)
    install(TARGETS n148i_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
