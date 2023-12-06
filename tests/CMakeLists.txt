cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)

FetchContent_MakeAvailable(Catch2)

add_executable(PathSpaceTests
  unit/test_Path.cpp
  unit/test_PathSpace.cpp
  unit/test_Capabilities.cpp
)

target_link_libraries(PathSpaceTests
PRIVATE
  Catch2::Catch2WithMain
)

target_include_directories(PathSpaceTests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(PathSpaceTests)