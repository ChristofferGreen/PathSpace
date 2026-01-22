cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

set(PATHSPACE_CORE_TEST_SOURCES
  unit/path/test_Iterator.cpp
  unit/path/test_IteratorValidation.cpp
  unit/path/test_GlobPath.cpp
  unit/path/test_ConcretePath.cpp
  unit/path/test_PathCoverage.cpp
  unit/path/test_PathUtils.cpp
  unit/core/test_WaitMap.cpp
  unit/core/test_InsertReturn.cpp
  unit/type/test_InputData.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_Serialization.cpp
  unit/type/test_TypeMetadataRegistry.cpp
  unit/type/test_SlidingBuffer.cpp
  unit/core/test_NodeDataSnapshot.cpp
  unit/core/test_NodeDataNested.cpp
  unit/core/test_NodeDataCoverage.cpp
  unit/core/test_PathSpaceContext.cpp
  unit/ext/test_alpaca.cpp
  unit/task/test_TaskPool.cpp
  unit/test_PathSpace_read.cpp
  unit/test_PathSpace_nesting.cpp
  unit/test_PathSpace_extract.cpp
  unit/test_PathSpace_listChildren.cpp
  unit/test_PathSpace_visit.cpp
  unit/test_PathSpace_insert.cpp
  unit/test_PathSpace_retarget.cpp
  unit/test_PathSpace_multithreading.cpp
  unit/test_PathSpace_execution.cpp
  unit/test_PathSpace_copy.cpp
  unit/test_PathSpace_internal.cpp
  unit/test_ValueHandleGuards.cpp
  unit/test_PathSpace_indexed.cpp
  unit/test_PathSpace_pod_fastpath.cpp
  unit/path/test_UnvalidatedPath.cpp
  unit/history/test_UndoJournal.cpp
  unit/history/test_UndoJournalState.cpp
  unit/history/test_UndoJournalPersistence.cpp
  unit/history/test_UndoHistoryUtils.cpp
  unit/history/test_UndoSavefileCodec.cpp
  unit/history/test_UndoableSpace.cpp
  unit/pathspace/test_PathSpaceJsonExporter.cpp
  unit/pathspace/test_PathSpaceHeader.cpp
  unit/test_BoundedPathSpace.cpp
  unit/core/test_Error.cpp
  unit/core/test_ExpectedShim.cpp
  unit/core/test_PodPayloadCoverage.cpp
  unit/core/test_PodPayloadFullCoverage.cpp
  unit/path/test_GlobMatch.cpp
  unit/log/test_TaggedLogger.cpp
  unit/layer/test_PathSpaceTrellisCore.cpp
  unit/layer/test_PathSpaceTrellisBlocking.cpp
  unit/layer/test_PathAliasCoverage.cpp
  unit/path/test_PathViewCoverage.cpp
  unit/path/test_validation.cpp
  unit/task/test_FutureCoverage.cpp
  unit/task/test_TaskStateAtomic.cpp
  unit/task/test_IFutureAny.cpp
  unit/task/test_ExecutorForwarding.cpp
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

set(PATHSPACE_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(PathSpaceTests PRIVATE PATHSPACE_SOURCE_DIR="${PATHSPACE_ROOT_DIR}")
# Treat CI/test builds as coverage-friendly to reduce timing flake sensitivity.
target_compile_definitions(PathSpaceTests PRIVATE PATHSPACE_COVERAGE_BUILD=1)

target_link_libraries(PathSpaceTests
  PRIVATE
  PathSpace
)

# Tagged logger coverage lives in the main test binary while keeping SP_LOG_DEBUG
# scoped to just the logging sources to avoid slowing the entire suite.
set(_PATHSPACE_LOGGER_SOURCES
  unit/log/test_TaggedLogger.cpp
)
set_source_files_properties(${_PATHSPACE_LOGGER_SOURCES}
  PROPERTIES COMPILE_DEFINITIONS "SP_LOG_DEBUG"
)

# PathSpace itself is usually built without SP_LOG_DEBUG; include the logger
# implementation here so the tests link against a debug-enabled build.
if(NOT PATHSPACE_ENABLE_LOG_DEBUG)
  target_sources(PathSpaceTests PRIVATE
    ${PATHSPACE_ROOT_DIR}/src/pathspace/log/TaggedLogger.cpp
  )
  set_source_files_properties(${PATHSPACE_ROOT_DIR}/src/pathspace/log/TaggedLogger.cpp
    PROPERTIES COMPILE_DEFINITIONS "SP_LOG_DEBUG"
  )
endif()

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
