cmake_minimum_required(VERSION 3.10)
project(draugr VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings as errors for clean code
add_compile_options(-Wall -Wextra -Wunused -Werror -pedantic)

option(ENABLE_ASAN "Enable Address Sanitizer" OFF)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Library definition
add_library(draugr src/ht.c src/ht_cache.c src/util.c)

target_include_directories(draugr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Testing
enable_testing()
add_subdirectory(tests)
