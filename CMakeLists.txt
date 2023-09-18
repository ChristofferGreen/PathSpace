cmake_minimum_required(VERSION 3.14)
project(PathSpace LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#SET (CMAKE_CXX_FLAGS "-fuse-ld=mold")
set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON")

add_subdirectory("tests")
