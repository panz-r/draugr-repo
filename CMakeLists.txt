cmake_minimum_required(VERSION 3.10)
project(draugr VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Library definition
add_library(draugr src/ht.c src/ht_cache.c)

target_include_directories(draugr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Testing
enable_testing()
add_subdirectory(tests)
