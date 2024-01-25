cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(PathSpaceTests
  unit/core/test_Capabilities.cpp
  unit/path/test_ConcretePath.cpp
  unit/path/test_ConcreteName.cpp
  unit/path/test_GlobPath.cpp
  unit/path/test_ConcretePathIterator.cpp
  unit/type/test_InputMetadata.cpp
  unit/serialization/test_InputData.cpp
  #unit/serialization/test_QueueStreamBuffer.cpp
  #unit/utils/test_PopFrontVector.cpp
  #unit/utils/test_PopFrontVectorStreamBuffer.cpp
  unit/utils/test_ByteQueue.cpp
  unit/test_PathSpace.cpp
)

target_link_libraries(PathSpaceTests
PRIVATE
  Catch2::Catch2WithMain
  PathSpace
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(PathSpaceTests)