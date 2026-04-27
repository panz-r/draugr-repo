add_executable(test_ht test_ht.c)
target_link_libraries(test_ht draugr)
add_test(NAME ht_unit_tests COMMAND test_ht)

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
