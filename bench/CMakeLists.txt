add_executable(cardinal_bench bench.cpp)
target_link_libraries(cardinal_bench PRIVATE cardinal_core)

# Written into every results file, so a number can be traced to the build that made it.
string(TOUPPER "${CMAKE_BUILD_TYPE}" cardinal_build_type)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${cardinal_build_type}}" cardinal_flags)
target_compile_definitions(cardinal_bench PRIVATE
  CARDINAL_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  CARDINAL_FLAGS="${cardinal_flags}")

# Only checks that it runs; the numbers come from bench/run_e1.sh on a release build.
add_test(NAME bench/smoke COMMAND cardinal_bench e1 --sizes 40 --shares 10 --runs 1)
add_test(NAME bench/smoke-e5 COMMAND cardinal_bench e5 --rows 300)
