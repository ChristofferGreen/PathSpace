cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

set(PATHSPACE_CORE_TEST_SOURCES
  unit/path/test_Iterator.cpp
  unit/path/test_ConcretePath.cpp
  unit/core/test_WaitMap.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_SlidingBuffer.cpp
  unit/core/test_NodeDataSnapshot.cpp
  unit/ext/test_alpaca.cpp
  unit/task/test_TaskPool.cpp
  unit/test_PathSpace_read.cpp
  unit/test_PathSpace_nesting.cpp
  unit/test_PathSpace_extract.cpp
  unit/test_PathSpace_listChildren.cpp
  unit/test_PathSpace_visit.cpp
  unit/test_PathSpace_insert.cpp
  unit/test_PathSpace_multithreading.cpp
  unit/test_PathSpace_execution.cpp
  unit/test_PathSpace_copy.cpp
  unit/path/test_UnvalidatedPath.cpp
  unit/history/test_UndoJournal.cpp
  unit/history/test_UndoJournalState.cpp
  unit/history/test_UndoJournalPersistence.cpp
  unit/history/test_UndoableSpace.cpp
  unit/pathspace/test_PathSpaceJsonExporter.cpp
  unit/test_BoundedPathSpace.cpp
  test_main.cpp
)

add_executable(PathSpaceTests ${PATHSPACE_CORE_TEST_SOURCES})

if(PATHSPACE_ENABLE_EXTRA)
  target_sources(PathSpaceTests PRIVATE
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
    unit/io/test_IoTrellis.cpp
  )
endif()

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

set(PATHSPACE_TEST_RUNNER "${PROJECT_SOURCE_DIR}/scripts/run-test-with-logs.sh")
set(PATHSPACE_TEST_LOG_DIR "$<SHELL_PATH:${CMAKE_BINARY_DIR}/test-logs>")

if(EXISTS "${PATHSPACE_TEST_RUNNER}")
  set(_PATHSPACE_HAVE_TEST_RUNNER TRUE)
else()
  set(_PATHSPACE_HAVE_TEST_RUNNER FALSE)
endif()

if(_PATHSPACE_HAVE_TEST_RUNNER)
  add_test(NAME PathSpaceTests
    COMMAND "${PATHSPACE_TEST_RUNNER}"
            --label PathSpaceTests
            --log-dir "${PATHSPACE_TEST_LOG_DIR}"
            --timeout 120
            --env PATHSPACE_LOG=1
            --env PATHSPACE_TEST_TIMEOUT=1
            --env MallocNanoZone=0
            --
            "$<TARGET_FILE:PathSpaceTests>"
  )

  set_tests_properties(PathSpaceTests PROPERTIES LABELS core)
endif()
