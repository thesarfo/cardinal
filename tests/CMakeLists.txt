include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(cardinal_tests smoke.cpp value_test.cpp lexer_test.cpp ast_printer_test.cpp expr_parser_test.cpp statement_parser_test.cpp parse_error_test.cpp catalog_test.cpp binder_test.cpp evaluator_test.cpp logical_plan_test.cpp physical_plan_test.cpp operator_test.cpp sort_limit_test.cpp database_test.cpp explain_test.cpp rule_optimizer_test.cpp constant_folding_test.cpp boolean_cleanup_test.cpp filter_cleanup_test.cpp join_test.cpp join_exec_test.cpp filter_pushdown_test.cpp cross_to_inner_test.cpp column_pruning_test.cpp answer_checker_test.cpp fuzz_test.cpp datagen_test.cpp analyze_test.cpp histogram_test.cpp)
target_link_libraries(cardinal_tests PRIVATE cardinal_core Catch2::Catch2WithMain)
catch_discover_tests(cardinal_tests)

# SQL-level tests: each tests/slt/*.test file is one ctest test.
add_executable(cardinal_slt slt/slt_runner.cpp)
target_link_libraries(cardinal_slt PRIVATE cardinal_core)

file(GLOB slt_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/slt/*.test)
foreach(file ${slt_files})
  get_filename_component(name ${file} NAME_WE)
  add_test(NAME slt/${name} COMMAND cardinal_slt ${file})
  add_test(NAME slt-unoptimized/${name} COMMAND cardinal_slt --no-optimizer ${file})
endforeach()

# Random queries through the answer checker. scripts/fuzz.sh runs many seeds, 10,000 queries each.
add_executable(cardinal_fuzz fuzz_main.cpp)
target_link_libraries(cardinal_fuzz PRIVATE cardinal_core)
add_test(NAME fuzz/seed1-2000 COMMAND cardinal_fuzz 1 2000)
