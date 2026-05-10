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

add_executable(test_internal test_internal.c)
target_link_libraries(test_internal draugr)
add_test(NAME ht_internal_tests COMMAND test_internal)

add_executable(test_oom test_oom.c)
target_sources(test_oom PRIVATE ../src/ht.c ../src/ht_cache.c ../src/util.c)
target_include_directories(test_oom PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_oom PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_oom m)
add_test(NAME ht_oom_tests COMMAND test_oom)

add_executable(test_bugs test_bugs.c)
target_sources(test_bugs PRIVATE ../src/ht.c ../src/ht_cache.c ../src/util.c)
target_include_directories(test_bugs PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_bugs PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_bugs m)
add_test(NAME ht_bugs_tests COMMAND test_bugs)

add_executable(test_property test_property.c)
target_sources(test_property PRIVATE ../src/ht.c ../src/ht_cache.c ../src/util.c)
target_include_directories(test_property PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_property PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_property m)
add_test(NAME ht_property_tests COMMAND test_property)

add_executable(test_property_b test_property_b.c)
target_sources(test_property_b PRIVATE ../src/ht.c ../src/ht_cache.c ../src/util.c)
target_include_directories(test_property_b PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_options(test_property_b PRIVATE -Wl,--wrap,malloc,--wrap,calloc,--wrap,realloc,--wrap,free)
target_link_libraries(test_property_b m)
add_test(NAME ht_property_tests_b COMMAND test_property_b)

add_executable(test_next_pow2 test_next_pow2.c ../src/util.c)
target_include_directories(test_next_pow2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME next_pow2_tests COMMAND test_next_pow2)
