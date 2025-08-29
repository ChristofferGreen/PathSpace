cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

add_executable(PathSpaceTests
  unit/path/test_Iterator.cpp
  unit/core/test_WaitMap.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_SlidingBuffer.cpp
  unit/layer/test_PathView.cpp
  unit/layer/test_PathFileSystem.cpp
  unit/layer/test_PathIO.cpp
  unit/layer/test_PathIODevices.cpp
  unit/layer/test_PathIOStdOut.cpp
  unit/layer/test_PathAlias.cpp
  unit/layer/test_PathIODeviceDiscovery.cpp
  unit/layer/test_PathIOPointerMixer.cpp
  unit/layer/test_PathIOGamepad.cpp
  unit/layer/test_PointerDefault_Composition.cpp
  unit/ext/test_alpaca.cpp
  unit/task/test_TaskPool.cpp
  unit/test_PathSpace_read.cpp
  unit/test_PathSpace_nesting.cpp
  unit/test_PathSpace_extract.cpp
  unit/test_PathSpace_insert.cpp
  unit/test_PathSpace_multithreading.cpp
  unit/test_PathSpace_execution.cpp
  test_main.cpp
)

# Opt-in debug logging for tests
option(PATHSPACE_ENABLE_LOG_DEBUG "Enable SP_LOG_DEBUG logging" OFF)
if(PATHSPACE_ENABLE_LOG_DEBUG)
  target_compile_definitions(PathSpaceTests PRIVATE SP_LOG_DEBUG)
endif()

target_compile_definitions(PathSpaceTests PRIVATE PATHSPACE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(PathSpaceTests
  PRIVATE
  PathSpace
)

include(CTest)
