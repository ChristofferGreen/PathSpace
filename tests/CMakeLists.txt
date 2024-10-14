cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)


add_executable(PathSpaceTests
  unit/core/test_Capabilities.cpp
  unit/path/test_ConcretePath.cpp
  unit/path/test_ConcreteName.cpp
  unit/path/test_GlobPath.cpp
  unit/path/test_ConcretePathIterator.cpp
  unit/path/test_ConstructiblePath.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_InputData.cpp
  #unit/utils/test_ByteQueue.cpp
  unit/ext/test_alpaca.cpp
  unit/taskpool/test_TaskPool.cpp
  #unit/ext/test_glaze.cpp
  unit/test_PathSpace.cpp
  test_main.cpp
)

target_link_libraries(PathSpaceTests
PRIVATE
  PathSpace
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(PathSpaceTests PRIVATE SP_LOG_DEBUG)
endif()

include(CTest)