cmake_minimum_required(VERSION 3.21)
project(cardinal VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings are errors, and every target gets them.
add_library(cardinal_warnings INTERFACE)
target_compile_options(cardinal_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

# The core library. The shell, tests and benchmarks all link to it.
add_library(cardinal_core
  src/binder/binder.cpp
  src/binder/bound_expr.cpp
  src/binder/format.cpp
  src/binder/scope.cpp
  src/catalog/catalog.cpp
  src/common/value.cpp
  src/common/version.cpp
  src/engine/database.cpp
  src/engine/explain.cpp
  src/engine/result_format.cpp
  src/exec/build.cpp
  src/exec/filter.cpp
  src/exec/limit.cpp
  src/exec/nested_loop_join.cpp
  src/exec/operator.cpp
  src/exec/project.cpp
  src/exec/seq_scan.cpp
  src/exec/sort.cpp
  src/expr/evaluator.cpp
  src/logical/plan_printer.cpp
  src/logical/plan_util.cpp
  src/logical/planner.cpp
  src/sql/ast_printer.cpp
  src/sql/lexer.cpp
  src/sql/parser.cpp
  src/optimizer/boolean_cleanup.cpp
  src/optimizer/constant_folding.cpp
  src/optimizer/default_rules.cpp
  src/optimizer/expr_rule.cpp
  src/optimizer/filter_cleanup.cpp
  src/optimizer/filter_pushdown.cpp
  src/optimizer/rule_optimizer.cpp
  src/physical/physical_planner.cpp
  src/physical/physical_printer.cpp
  src/storage/table.cpp
)
target_include_directories(cardinal_core PUBLIC src)
target_link_libraries(cardinal_core PUBLIC cardinal_warnings)

add_executable(cardinal src/shell/main.cpp)
target_link_libraries(cardinal PRIVATE cardinal_core)

enable_testing()
add_subdirectory(tests)
