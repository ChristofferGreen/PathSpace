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

enable_testing()

add_executable(PathSpaceTests
  unit/test_PathRoot.cpp
  unit/test_SpacePath.cpp
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

# If you need to link against other libraries from your project
# target_link_libraries(PathSpaceTests PRIVATE my_project_lib)
include(CTest)
include(Catch)
catch_discover_tests(PathSpaceTests)