cmake_minimum_required(VERSION 3.13)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(xxh32_embedded LANGUAGES C)
endif()

set(XXH32_BACKEND "AUTO" CACHE STRING "XXH32 backend: AUTO, GENERIC, or M0PLUS")
set_property(CACHE XXH32_BACKEND PROPERTY STRINGS AUTO GENERIC M0PLUS)
set(XXH32_NAMESPACE "" CACHE STRING "Optional XXH_NAMESPACE symbol prefix, for example mylib_")
option(XXH32_BUILD_M0PLUS_TARGET "Expose xxh32::m0plus even when it is not selected" OFF)

string(TOUPPER "${XXH32_BACKEND}" _xxh32_backend_requested)
if(NOT _xxh32_backend_requested MATCHES "^(AUTO|GENERIC|M0PLUS)$")
    message(FATAL_ERROR "XXH32_BACKEND must be AUTO, GENERIC, or M0PLUS")
endif()

set(_xxh32_is_m0plus FALSE)
if(DEFINED PICO_RP2040 AND PICO_RP2040)
    set(_xxh32_is_m0plus TRUE)
elseif(_xxh32_backend_requested STREQUAL "AUTO")
    include(CheckCSourceCompiles)
    set(_xxh32_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_c_source_compiles("#ifndef __ARM_ARCH_6M__\n#error not ARMv6-M\n#endif\nint main(void) { return 0; }" XXH32_COMPILER_IS_ARMV6M)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_xxh32_saved_try_compile_target_type}")
    if(XXH32_COMPILER_IS_ARMV6M)
        set(_xxh32_is_m0plus TRUE)
    endif()
endif()

if(_xxh32_backend_requested STREQUAL "AUTO")
    if(_xxh32_is_m0plus)
        set(_xxh32_backend "M0PLUS")
    else()
        set(_xxh32_backend "GENERIC")
    endif()
else()
    set(_xxh32_backend "${_xxh32_backend_requested}")
endif()

set(_xxh32_common_sources
    src/xxh32_oneshot.c
    src/xxh32_stream.c
    src/xxh32_copy.c
    src/xxh32_alloc.c
    src/xxh32_canonical.c
    src/xxh32_version.c)

function(_xxh32_configure_target target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(${target} PUBLIC c_std_99)
    if(NOT XXH32_NAMESPACE STREQUAL "")
        target_compile_definitions(${target} PUBLIC XXH_NAMESPACE=${XXH32_NAMESPACE})
    endif()
endfunction()

add_library(xxh32_generic STATIC EXCLUDE_FROM_ALL ${_xxh32_common_sources})
_xxh32_configure_target(xxh32_generic)
target_compile_definitions(xxh32_generic PUBLIC XXH32_BACKEND_GENERIC=1)
add_library(xxh32::generic ALIAS xxh32_generic)

set(_xxh32_make_m0plus_target FALSE)
if(_xxh32_backend STREQUAL "M0PLUS" OR XXH32_BUILD_M0PLUS_TARGET)
    set(_xxh32_make_m0plus_target TRUE)
endif()

if(_xxh32_make_m0plus_target)
    enable_language(ASM)
    add_library(xxh32_m0plus STATIC EXCLUDE_FROM_ALL
        ${_xxh32_common_sources}
        src/m0plus/xxh32_oneshot.S
        src/m0plus/xxh32_stream.S)
    _xxh32_configure_target(xxh32_m0plus)
    target_compile_definitions(xxh32_m0plus PUBLIC XXH32_BACKEND_M0PLUS=1)
    add_library(xxh32::m0plus ALIAS xxh32_m0plus)
endif()

add_library(xxh32_selected_backend INTERFACE)
if(_xxh32_backend STREQUAL "M0PLUS")
    target_link_libraries(xxh32_selected_backend INTERFACE xxh32_m0plus)
    target_compile_definitions(xxh32_selected_backend INTERFACE XXH32_BACKEND_M0PLUS=1)
else()
    target_link_libraries(xxh32_selected_backend INTERFACE xxh32_generic)
    target_compile_definitions(xxh32_selected_backend INTERFACE XXH32_BACKEND_GENERIC=1)
endif()

add_library(xxh32 INTERFACE)
target_link_libraries(xxh32 INTERFACE xxh32_selected_backend)
add_library(xxh32::xxhash ALIAS xxh32)

message(STATUS "xxh32 backend: ${_xxh32_backend}")
if(DEFINED PICO_RP2040 AND PICO_RP2040)
    message(STATUS "xxh32: Pico SDK RP2040 detected; selecting Cortex-M0+ backend")
endif()
