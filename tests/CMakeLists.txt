include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(cardinal_tests smoke.cpp value_test.cpp lexer_test.cpp ast_printer_test.cpp expr_parser_test.cpp statement_parser_test.cpp parse_error_test.cpp catalog_test.cpp binder_test.cpp)
target_link_libraries(cardinal_tests PRIVATE cardinal_core Catch2::Catch2WithMain)
catch_discover_tests(cardinal_tests)
