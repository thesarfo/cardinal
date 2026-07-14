# Placeholder until Catch2 arrives in task 0.2.
add_executable(cardinal_tests smoke.cpp)
target_link_libraries(cardinal_tests PRIVATE cardinal_core)
add_test(NAME smoke COMMAND cardinal_tests)
