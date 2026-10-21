cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

set(PATHSPACE_CORE_TEST_SOURCES
  unit/path/test_Iterator.cpp
  unit/path/test_IteratorRange.cpp
  unit/path/test_IteratorValidation.cpp
  unit/path/test_GlobPath.cpp
  unit/path/test_ConcretePath.cpp
  unit/path/test_ConcretePathIterator.cpp
  unit/path/test_PathCoverage.cpp
  unit/path/test_PathUtils.cpp
  unit/path/test_ConcreteNameCoverage.cpp
  unit/path/test_TransparentString.cpp
  unit/core/test_WaitMap.cpp
  unit/core/test_Node.cpp
  unit/core/test_NodeCoverage.cpp
  unit/core/test_LeafCoverage.cpp
  unit/core/test_BlockOptions.cpp
  unit/core/test_InsertReturn.cpp
  unit/core/test_InCoverage.cpp
  unit/type/test_InputData.cpp
  unit/type/test_InputDataCoverage.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_Serialization.cpp
  unit/type/test_DataCategory.cpp
  unit/type/test_FunctionCategory.cpp
  unit/type/test_TypeMetadataRegistry.cpp
  unit/type/test_TypeMetadataRegistryPrivates.cpp
  unit/type/test_SlidingBuffer.cpp
  unit/core/test_NodeDataSnapshot.cpp
  unit/core/test_NodeDataNested.cpp
  unit/core/test_NodeDataCoverage.cpp
  unit/core/test_NodeDataDeserializeEdges.cpp
  unit/core/test_PathSpaceContext.cpp
  unit/pathspace/test_PathSpaceBaseSpanPack.cpp
  unit/ext/test_alpaca.cpp
  unit/core/test_ExpectedShimCoverage.cpp
  unit/core/test_ElementType.cpp
  unit/core/test_Out.cpp
  unit/task/test_TaskPool.cpp
  unit/task/test_TaskCreate.cpp
  unit/task/test_TaskT.cpp
  unit/task/test_TaskPoolTraceInternals.cpp
  unit/trace/test_PathSpaceTrace.cpp
  unit/pathspace/test_PathSpaceRead.cpp
  unit/pathspace/test_PathSpaceNestingBasic.cpp
  unit/pathspace/test_PathSpaceNestingIndexed.cpp
  unit/pathspace/test_PathSpaceNestingLifecycle.cpp
  unit/pathspace/test_PathSpaceNestingConcurrent.cpp
  unit/pathspace/test_PathSpaceExtract.cpp
  unit/pathspace/test_PathSpaceListChildren.cpp
  unit/pathspace/test_PathSpaceVisitValueHandle.cpp
  unit/pathspace/test_PathSpaceVisitNested.cpp
  unit/pathspace/test_PathSpaceVisit.cpp
  unit/pathspace/test_PathSpaceInsert.cpp
  unit/pathspace/test_PathSpaceRetarget.cpp
  unit/pathspace/test_PathSpaceMultithreading.cpp
  unit/pathspace/test_PathSpaceExecution.cpp
  unit/pathspace/test_PathSpaceCopy.cpp
  unit/pathspace/test_PathSpaceInternal.cpp
  unit/pathspace/test_ValueHandleGuards.cpp
  unit/pathspace/test_ValueHandleReadInto.cpp
  unit/pathspace/test_PathSpaceIndexed.cpp
  unit/pathspace/test_PathSpacePodFastpathLayers.cpp
  unit/pathspace/test_PathSpacePodFastpathConcurrency.cpp
  unit/pathspace/test_PathSpacePodFastpathPackInsert.cpp
  unit/pathspace/test_PathSpacePodFastpathSpanPack.cpp
  unit/pathspace/test_PathSpacePodFastpathCore.cpp
  unit/path/test_UnvalidatedPath.cpp
  unit/history/test_UndoJournalRoundTrip.cpp
  unit/history/test_UndoJournalState.cpp
  unit/history/test_UndoJournalPersistence.cpp
  unit/history/test_UndoJournalEntry.cpp
  unit/history/test_UndoHistoryUtils.cpp
  unit/history/test_UndoSavefileCodec.cpp
  unit/history/test_UndoableSpaceCore.cpp
  unit/history/test_UndoableSpaceRetention.cpp
  unit/history/test_UndoableSpaceMetrics.cpp
  unit/history/test_UndoableSpaceValidation.cpp
  unit/history/test_UndoableSpacePersistence.cpp
  unit/history/test_UndoableSpaceConcurrency.cpp
  unit/history/test_UndoJournalPersistenceCoverage.cpp
  unit/history/test_UndoableSpaceCoverage.cpp
  unit/history/test_UndoableSpaceHistoryTelemetryCoverage.cpp
  unit/history/test_UndoableSpaceHistoryPersistenceCoverage.cpp
  unit/history/test_UndoableSpaceHistoryJournalCoverage.cpp
  unit/history/test_UndoableSpaceHistoryGuardsCoverage.cpp
  unit/history/test_UndoableSpaceTransactionsCoverage.cpp
  unit/pathspace/test_PathSpaceJsonExporterFlatPaths.cpp
  unit/pathspace/test_PathSpaceJsonExporterLimits.cpp
  unit/pathspace/test_PathSpaceJsonExporterPlaceholders.cpp
  unit/pathspace/test_PathSpaceJsonExporterErrors.cpp
  unit/pathspace/test_PathSpaceJsonExporterModes.cpp
  unit/pathspace/test_PathSpaceJsonExporterBasics.cpp
  unit/pathspace/test_PathSpaceJsonConverters.cpp
  unit/pathspace/test_PathSpaceHeader.cpp
  unit/pathspace/test_PathSpaceBaseDefaults.cpp
  unit/pathspace/test_VisitOptions.cpp
  unit/layer/test_BoundedPathSpace.cpp
  unit/core/test_Error.cpp
  unit/core/test_ExpectedShim.cpp
  unit/core/test_PodPayloadCoverage.cpp
  unit/core/test_PodPayloadFullCoverage.cpp
  unit/core/test_PodPayloadCorruption.cpp
  unit/path/test_GlobMatch.cpp
  unit/log/test_TaggedLogger.cpp
  unit/layer/test_PathSpaceTrellisCore.cpp
  unit/layer/test_PathSpaceTrellisBlocking.cpp
  unit/layer/test_PathAliasCoverage.cpp
  unit/layer/test_SnapshotCacheLifecycle.cpp
  unit/layer/test_SnapshotCacheReadPaths.cpp
  unit/layer/test_SnapshotCacheMetrics.cpp
  unit/layer/test_SnapshotCacheRebuild.cpp
  unit/layer/test_SnapshotCacheDirtyRoots.cpp
  unit/layer/test_SnapshotCacheMutationTracking.cpp
  unit/layer/test_PathViewCoverage.cpp
  unit/path/test_validation.cpp
  unit/task/test_FutureCoverage.cpp
  unit/task/test_TaskStateAtomic.cpp
  unit/task/test_IFutureAny.cpp
  unit/task/test_ExecutorForwarding.cpp
  test_main.cpp
  unit/pathspace/test_SpanCallbackPointers.cpp
  unit/layer/test_PathView.cpp
  unit/layer/test_PathAlias.cpp
)

add_executable(PathSpaceTests ${PATHSPACE_CORE_TEST_SOURCES})

# Shared test helpers (PathSpaceTestHelper.hpp) live in tests/unit.
target_include_directories(PathSpaceTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

# Opt-in debug logging for tests
option(PATHSPACE_ENABLE_LOG_DEBUG "Enable SP_LOG_DEBUG logging" OFF)
if(PATHSPACE_ENABLE_LOG_DEBUG)
  target_compile_definitions(PathSpaceTests PRIVATE SP_LOG_DEBUG)
endif()

set(PATHSPACE_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(PathSpaceTests PRIVATE PATHSPACE_SOURCE_DIR="${PATHSPACE_ROOT_DIR}")
if(PATHSPACE_NEEDS_EXPECTED_SHIM)
  target_compile_definitions(PathSpaceTests PRIVATE PATHSPACE_NEEDS_EXPECTED_SHIM=1)
endif()

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
