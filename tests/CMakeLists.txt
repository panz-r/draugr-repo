# Tests must always have assertions enabled (CMake Release sets -DNDEBUG)
add_compile_options(-UNDEBUG)

add_executable(test_ht test_ht.c)
target_link_libraries(test_ht draugr)
add_test(NAME ht_unit_tests COMMAND test_ht)

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api draugr)
add_test(NAME ht_public_api_tests COMMAND test_public_api)

add_executable(test_stress test_stress.c)
target_link_libraries(test_stress draugr)
add_test(NAME ht_stress_tests COMMAND test_stress)

add_executable(test_grow_shrink test_grow_shrink.c)
target_link_libraries(test_grow_shrink draugr)
add_test(NAME ht_grow_shrink_tests COMMAND test_grow_shrink)

add_executable(test_migration test_migration.c)
target_link_libraries(test_migration draugr)
add_test(NAME ht_migration_tests COMMAND test_migration)

add_executable(test_edge_cases_a test_edge_cases_a.c)
target_link_libraries(test_edge_cases_a draugr)
add_test(NAME ht_edge_case_a_tests COMMAND test_edge_cases_a)

add_executable(test_edge_cases_b test_edge_cases_b.c)
target_link_libraries(test_edge_cases_b draugr)
add_test(NAME ht_edge_case_b_tests COMMAND test_edge_cases_b)

add_executable(test_ht_cache test_ht_cache.c)
target_link_libraries(test_ht_cache draugr)
add_test(NAME ht_cache_tests COMMAND test_ht_cache)

# test_arena: only when arena is included (not DRAUGR_USE_MALLOC)
if(NOT DRAUGR_USE_MALLOC)
add_executable(test_arena test_arena.c)
target_sources(test_arena PRIVATE ${CMAKE_SOURCE_DIR}/src/arena.c)
target_include_directories(test_arena PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_arena m)
add_test(NAME arena_tests COMMAND test_arena)

add_executable(test_ht_arena test_ht_arena.c)
target_link_libraries(test_ht_arena draugr)
add_test(NAME ht_arena_tests COMMAND test_ht_arena)

add_executable(test_arena_concurrent test_arena_concurrent.c)
target_sources(test_arena_concurrent PRIVATE ${CMAKE_SOURCE_DIR}/src/arena.c)
target_include_directories(test_arena_concurrent PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_arena_concurrent m pthread)
add_test(NAME arena_concurrent_tests COMMAND test_arena_concurrent)
endif()

add_executable(test_internal test_internal.c)
target_link_libraries(test_internal draugr)
add_test(NAME ht_internal_tests COMMAND test_internal)

add_executable(test_htc test_htc.c)
target_compile_options(test_htc PRIVATE -Wno-missing-field-initializers)
target_link_libraries(test_htc draugr)
add_test(NAME htc_phase1_tests COMMAND test_htc)

add_executable(bench_htc bench_htc.c)
target_link_libraries(bench_htc draugr m)

# Filter tests
add_executable(test_bloom test_bloom.c)
target_link_libraries(test_bloom draugr m)
add_test(NAME bloom_filter_tests COMMAND test_bloom)

add_executable(test_vacuum test_vacuum.c)
target_link_libraries(test_vacuum draugr m)
add_test(NAME vacuum_filter_tests COMMAND test_vacuum)

add_executable(test_amq test_amq.c)
target_link_libraries(test_amq draugr m)
add_test(NAME amq_filter_tests COMMAND test_amq)

# Concurrent filter test
add_executable(test_htc_filter test_htc_filter.c)
target_link_libraries(test_htc_filter draugr m)
add_test(NAME htc_filter_tests COMMAND test_htc_filter)

# Property-based AMQ filter tests
add_executable(test_amq_property test_amq_property.c)
target_link_libraries(test_amq_property draugr m)
add_test(NAME amq_property_tests COMMAND test_amq_property)

# htc_kv high-level key-value wrapper
add_executable(test_htc_kv test_htc_kv.c)
target_link_libraries(test_htc_kv draugr m)
add_test(NAME htc_kv_tests COMMAND test_htc_kv)

add_executable(test_htc_kv_property test_htc_kv_property.c)
target_link_libraries(test_htc_kv_property draugr m)
add_test(NAME htc_kv_property_tests COMMAND test_htc_kv_property)

# These tests use --wrap to intercept stdlib allocators
# In DRAUGR_USE_MALLOC mode, --wrap tests still work (they intercept real malloc)
set(WRAP_TEST_COMMON ../src/ht.c ../src/ht_cache.c ../src/util.c)
if(NOT DRAUGR_USE_MALLOC)
    list(APPEND WRAP_TEST_COMMON ../src/arena.c)
endif()

add_executable(test_oom test_oom.c)
target_sources(test_oom PRIVATE ${WRAP_TEST_COMMON})
target_include_directories(test_oom PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_oom PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_oom m)
add_test(NAME ht_oom_tests COMMAND test_oom)

add_executable(test_bugs test_bugs.c)
target_sources(test_bugs PRIVATE ${WRAP_TEST_COMMON})
target_include_directories(test_bugs PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_bugs PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_bugs m)
add_test(NAME ht_bugs_tests COMMAND test_bugs)

set(PROPERTY_UTILS property_utils.c)

add_executable(test_property_a test_property_a.c ${PROPERTY_UTILS})
target_sources(test_property_a PRIVATE ${WRAP_TEST_COMMON})
target_include_directories(test_property_a PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_property_a PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_property_a m)
add_test(NAME ht_property_tests_a COMMAND test_property_a)

add_executable(test_property_c test_property_c.c ${PROPERTY_UTILS})
target_sources(test_property_c PRIVATE ${WRAP_TEST_COMMON})
target_include_directories(test_property_c PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_property_c PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_property_c m)
add_test(NAME ht_property_tests_c COMMAND test_property_c)

add_executable(test_property_b test_property_b.c ${PROPERTY_UTILS})
target_sources(test_property_b PRIVATE ${WRAP_TEST_COMMON})
target_include_directories(test_property_b PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_property_b PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_property_b m)
add_test(NAME ht_property_tests_b COMMAND test_property_b)

add_executable(test_next_pow2 test_next_pow2.c ../src/util.c)
target_include_directories(test_next_pow2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME next_pow2_tests COMMAND test_next_pow2)
