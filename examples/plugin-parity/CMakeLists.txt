cmake_minimum_required(VERSION 3.20)
project(ParityProbe LANGUAGES CXX)

# Example addon only. It uses the plain C API header and Windows, not KenshiLib.
add_library(ParityProbe SHARED ParityProbe.cpp)
target_include_directories(ParityProbe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)
set_target_properties(ParityProbe PROPERTIES OUTPUT_NAME "ParityProbe")
