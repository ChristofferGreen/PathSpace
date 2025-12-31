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
  unit/path/test_UnvalidatedPath.cpp
  unit/history/test_UndoJournal.cpp
  unit/history/test_UndoJournalState.cpp
  unit/history/test_UndoJournalPersistence.cpp
  unit/history/test_UndoableSpace.cpp
  unit/pathspace/test_PathSpaceJsonExporter.cpp
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

add_test(NAME HistoryTelemetryDashboard
  COMMAND python3
          "${CMAKE_SOURCE_DIR}/tests/tools/test_history_cli_roundtrip_dashboard.py"
          --repo-root "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(HistoryTelemetryDashboard PROPERTIES LABELS history)

if(PATHSPACE_ENABLE_UI)
  set(PATHSPACE_UI_TEST_COMMON_ENV
      --env PATHSPACE_LOG=1
      --env PATHSPACE_TEST_TIMEOUT=1
      --env MallocNanoZone=0
      --env PATHSPACE_DISABLE_SURFACE_CACHE_WATCH=1
      --env PATHSPACE_CAPTURE_FONT_DIAGNOSTICS=1)

  function(pathspace_add_ui_test LABEL)
    if(NOT _PATHSPACE_HAVE_TEST_RUNNER)
      return()
    endif()
    set(oneValueArgs SUITE SUITE_EXCLUDE TIMEOUT)
    set(multiValueArgs EXTRA_ARGS)
    cmake_parse_arguments(UI "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(test_timeout "60")
    if(UI_TIMEOUT)
      set(test_timeout "${UI_TIMEOUT}")
    endif()

    set(env_args ${PATHSPACE_UI_TEST_COMMON_ENV})
    if(UI_SUITE)
      string(REPLACE ";" "," _ui_suite_filter "${UI_SUITE}")
      list(APPEND env_args "--env" "PATHSPACE_TEST_SUITE=${_ui_suite_filter}")
    endif()
    if(UI_SUITE_EXCLUDE)
      foreach(exclude_suite IN LISTS UI_SUITE_EXCLUDE)
        list(APPEND env_args "--env" "PATHSPACE_TEST_SUITE_EXCLUDE=${exclude_suite}")
      endforeach()
    endif()

    add_test(NAME ${LABEL}
      COMMAND "${PATHSPACE_TEST_RUNNER}"
              --label ${LABEL}
              --log-dir "${PATHSPACE_TEST_LOG_DIR}"
              --timeout ${test_timeout}
              ${env_args}
              --
              "$<TARGET_FILE:PathSpaceUITests>"
              ${UI_EXTRA_ARGS}
    )
    set_tests_properties(${LABEL} PROPERTIES LABELS ui)
  endfunction()

  pathspace_add_ui_test("PathSpaceUITests_All" TIMEOUT 0)
  set_tests_properties(PathSpaceUITests_All PROPERTIES LABELS ui)

  if(TARGET minimal_button_example)
    add_test(NAME DeclarativeButtonJsonDump
      COMMAND "${PATHSPACE_TEST_RUNNER}"
              --label DeclarativeButtonJsonDump
              --log-dir "${PATHSPACE_TEST_LOG_DIR}"
              --timeout 20
              --env PATHSPACE_LOG=1
              --env PATHSPACE_TEST_TIMEOUT=1
              --env MallocNanoZone=0
              --
              python3 "${CMAKE_SOURCE_DIR}/tests/tools/test_declarative_button_dump.py"
                      --binary "$<TARGET_FILE:minimal_button_example>"
    )
    set_tests_properties(DeclarativeButtonJsonDump PROPERTIES LABELS ui)
  endif()

  if(APPLE)
    # PixelNoise perf harness removed until the ServeHtml/example rewrite reinstates it.
  endif()
endif()
