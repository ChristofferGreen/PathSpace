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
  unit/test_PathSpace_insert.cpp
  unit/test_PathSpace_multithreading.cpp
  unit/test_PathSpace_execution.cpp
  unit/path/test_UnvalidatedPath.cpp
  unit/history/test_UndoJournal.cpp
  unit/history/test_UndoJournalState.cpp
  unit/history/test_UndoJournalPersistence.cpp
  unit/history/test_UndoableSpace.cpp
  unit/runtime/test_IOPump.cpp
  unit/runtime/test_TelemetryControl.cpp
  test_main.cpp
)

add_executable(PathSpaceTests ${PATHSPACE_CORE_TEST_SOURCES})

if(PATHSPACE_ENABLE_APP)
  target_sources(PathSpaceTests PRIVATE app/test_AppPaths.cpp)
endif()

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

if(PATHSPACE_ENABLE_UI)
  add_executable(PathSpaceUITests
    ui/test_AppBootstrap.cpp
    ui/test_SceneHelpers.cpp
    ui/test_Builders.cpp
    ui/test_WidgetInput.cpp
    ui/test_TextBuilder.cpp
    ui/test_WidgetReducersFuzz.cpp
    ui/test_SceneSnapshotBuilder.cpp
    ui/test_ProgressiveSurfaceBuffer.cpp
    ui/test_HtmlAssetSerialization.cpp
    ui/test_PathSurfaceSoftware.cpp
    ui/test_FontResources.cpp
    ui/test_PathRenderer2D.cpp
    ui/test_RendererFaultInjection.cpp
    ui/test_HtmlAdapter.cpp
    ui/test_HtmlPresent.cpp
    ui/test_HtmlReplay.cpp
    ui/test_SceneHitTest.cpp
    ui/test_DeclarativeWidgets.cpp
    ui/test_DeclarativeSceneLifecycle.cpp
  ui/test_PathWindowView.cpp
  ui/test_BackendAdapters.cpp
  ui/test_DeclarativeRuntime.cpp
  test_main.cpp
)
  if(APPLE AND PATHSPACE_UI_METAL)
  target_sources(PathSpaceUITests PRIVATE
    ui/test_PathWindowView_Metal.mm
    ui/test_PathRenderer2DMetal.mm
    ui/test_BackendAdaptersMetal.mm
  )
  set_source_files_properties(ui/test_PathWindowView_Metal.mm ui/test_PathRenderer2DMetal.mm ui/test_BackendAdaptersMetal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  endif()
  target_compile_definitions(PathSpaceUITests PRIVATE PATHSPACE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(PathSpaceUITests PRIVATE PathSpace)

  add_executable(html_canvas_dump tools/html_canvas_dump.cpp)
  target_link_libraries(html_canvas_dump PRIVATE PathSpace)

  find_program(PATHSPACE_NODE_EXEC node)
  if(PATHSPACE_NODE_EXEC)
    add_test(NAME HtmlCanvasVerify
      COMMAND ${PATHSPACE_NODE_EXEC} ${CMAKE_SOURCE_DIR}/scripts/verify_html_canvas.js
              $<TARGET_FILE:html_canvas_dump>)
    set_tests_properties(HtmlCanvasVerify PROPERTIES LABELS html)
    add_test(NAME HtmlAssetInspect
      COMMAND ${PATHSPACE_NODE_EXEC} ${CMAKE_SOURCE_DIR}/scripts/verify_hsat_assets.js
              $<TARGET_FILE:pathspace_hsat_inspect>)
    set_tests_properties(HtmlAssetInspect PROPERTIES LABELS html)
  else()
    add_test(NAME HtmlCanvasVerify
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping HTML canvas verification (node not found)")
    set_tests_properties(HtmlCanvasVerify PROPERTIES LABELS html)
    add_test(NAME HtmlAssetInspect
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping HSAT inspection verification (node not found)")
    set_tests_properties(HtmlAssetInspect PROPERTIES LABELS html)
  endif()
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

set(PATHSPACE_TEST_RUNNER "${CMAKE_SOURCE_DIR}/scripts/run-test-with-logs.sh")
if(NOT EXISTS "${PATHSPACE_TEST_RUNNER}")
  message(FATAL_ERROR "Test runner script missing: ${PATHSPACE_TEST_RUNNER}")
endif()

set(PATHSPACE_TEST_LOG_DIR "$<SHELL_PATH:${CMAKE_BINARY_DIR}/test-logs>")

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

add_test(NAME HistorySavefileCLIRoundTrip
  COMMAND "${PATHSPACE_TEST_RUNNER}"
          --label HistorySavefileCLI
          --log-dir "${PATHSPACE_TEST_LOG_DIR}"
          --timeout 60
          --env PATHSPACE_LOG=1
          --env PATHSPACE_TEST_TIMEOUT=1
          --env MallocNanoZone=0
          --
          "$<TARGET_FILE:pathspace_history_cli_roundtrip>"
)
set_tests_properties(HistorySavefileCLIRoundTrip PROPERTIES LABELS history)

add_test(NAME HistoryTelemetryDashboard
  COMMAND python3
          "${CMAKE_SOURCE_DIR}/tests/tools/test_history_cli_roundtrip_dashboard.py"
          --repo-root "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(HistoryTelemetryDashboard PROPERTIES LABELS history)

if(PATHSPACE_ENABLE_UI)
  add_test(NAME PathSpaceUITests
    COMMAND "${PATHSPACE_TEST_RUNNER}"
            --label PathSpaceUITests
            --log-dir "${PATHSPACE_TEST_LOG_DIR}"
            --timeout 120
            --env PATHSPACE_LOG=1
            --env PATHSPACE_TEST_TIMEOUT=1
            --env MallocNanoZone=0
            --
            "$<TARGET_FILE:PathSpaceUITests>"
  )
  set_tests_properties(PathSpaceUITests PROPERTIES LABELS ui)

  if(APPLE)
    add_test(NAME PixelNoisePerfHarness
      COMMAND "${PATHSPACE_TEST_RUNNER}"
              --label PixelNoisePerfHarness
              --log-dir "${PATHSPACE_TEST_LOG_DIR}"
              --timeout 90
              --env PATHSPACE_LOG=1
              --env PATHSPACE_TEST_TIMEOUT=1
              --env MallocNanoZone=0
              --
              python3
              "${CMAKE_SOURCE_DIR}/scripts/check_pixel_noise_baseline.py"
              --build-dir "${CMAKE_BINARY_DIR}"
    )
    set_tests_properties(PixelNoisePerfHarness PROPERTIES LABELS perf)
    if(PATHSPACE_UI_METAL)
      add_test(NAME PixelNoisePerfHarnessMetal
        COMMAND "${PATHSPACE_TEST_RUNNER}"
                --label PixelNoisePerfHarnessMetal
                --log-dir "${PATHSPACE_TEST_LOG_DIR}"
                --timeout 90
                --env PATHSPACE_LOG=1
                --env PATHSPACE_TEST_TIMEOUT=1
                --env MallocNanoZone=0
                --env PATHSPACE_ENABLE_METAL_UPLOADS=1
                --
                python3
                "${CMAKE_SOURCE_DIR}/scripts/check_pixel_noise_baseline.py"
                --build-dir "${CMAKE_BINARY_DIR}"
                --baseline "${CMAKE_SOURCE_DIR}/docs/perf/pixel_noise_metal_baseline.json"
      )
      set_tests_properties(PixelNoisePerfHarnessMetal PROPERTIES LABELS perf)
    endif()
  endif()
endif()
