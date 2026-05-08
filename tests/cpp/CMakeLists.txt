cmake_minimum_required(VERSION 3.20)
project(StobePortableTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(stobe_portable_tests
  stobe_text_tests.cpp
  ../../src/StobeText.cpp
)

target_include_directories(stobe_portable_tests PRIVATE
  ../../src
)

enable_testing()
add_test(NAME stobe_portable_tests COMMAND stobe_portable_tests)
