cmake_minimum_required(VERSION 3.15)

# Define the project name and the language used
project(PathSpaceTests LANGUAGES CXX)

# Define the C++ standard to use
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()
add_subdirectory(src)
add_subdirectory(tests)