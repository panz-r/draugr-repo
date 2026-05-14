cmake_minimum_required(VERSION 3.10)
project(draugr VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CheckCCompilerFlag)

# Warnings as errors for clean code
add_compile_options(-Wall -Wextra -Wunused -Werror -pedantic)

option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(DRAUGR_USE_MALLOC "Use standard malloc/calloc/realloc/free instead of arena allocator" OFF)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Enable SIMD extensions for arena slab bitmap scan
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    check_c_compiler_flag(-mavx2 HAS_AVX2)
    if(HAS_AVX2)
        add_compile_options(-mavx2)
        message(STATUS "AVX2 enabled for slab bitmap scan")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    message(STATUS "NEON available for slab bitmap scan (always on aarch64)")
endif()

# Determine source files based on mode
if(DRAUGR_USE_MALLOC)
    add_compile_options(-DDRAUGR_USE_MALLOC)
    message(STATUS "Building with DRAUGR_USE_MALLOC: using stdlib allocators (arena excluded)")
    set(DRAUGR_SOURCES src/ht.c src/ht_cache.c src/util.c src/htc.c)
else()
    message(STATUS "Building with arena allocator (default)")
    set(DRAUGR_SOURCES src/ht.c src/ht_cache.c src/util.c src/arena.c src/htc.c)
endif()

add_library(draugr ${DRAUGR_SOURCES})
target_link_libraries(draugr m)

target_include_directories(draugr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

option(BUILD_TESTS "Build tests" ON)

# Testing
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
