cmake_minimum_required(VERSION 3.16)
project(llm_structured_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(llm_structured
  src/llm_structured.cpp
)

target_include_directories(llm_structured PUBLIC include)

add_executable(llm_structured_cli
  src/cli.cpp
)

target_link_libraries(llm_structured_cli PRIVATE llm_structured)

add_executable(llm_structured_tests
  test/tests.cpp
)

target_link_libraries(llm_structured_tests PRIVATE llm_structured)

enable_testing()
add_test(NAME llm_structured_tests COMMAND llm_structured_tests)
