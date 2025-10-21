cmake_minimum_required(VERSION 3.15)

project(PathSpaceTests VERSION 1.0)

set(PATHSPACE_CORE_TEST_SOURCES
  unit/path/test_Iterator.cpp
  unit/core/test_WaitMap.cpp
  unit/type/test_InputMetadata.cpp
  unit/type/test_SlidingBuffer.cpp
  unit/ext/test_alpaca.cpp
  unit/task/test_TaskPool.cpp
  unit/test_PathSpace_read.cpp
  unit/test_PathSpace_nesting.cpp
  unit/test_PathSpace_extract.cpp
  unit/test_PathSpace_insert.cpp
  unit/test_PathSpace_multithreading.cpp
  unit/test_PathSpace_execution.cpp
  unit/path/test_UnvalidatedPath.cpp
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
  )
endif()

if(PATHSPACE_ENABLE_UI)
  add_executable(PathSpaceUITests
    ui/test_AppBootstrap.cpp
    ui/test_SceneHelpers.cpp
    ui/test_Builders.cpp
    ui/test_SceneSnapshotBuilder.cpp
    ui/test_ProgressiveSurfaceBuffer.cpp
    ui/test_HtmlAssetSerialization.cpp
    ui/test_PathSurfaceSoftware.cpp
    ui/test_PathRenderer2D.cpp
    ui/test_HtmlAdapter.cpp
    ui/test_HtmlPresent.cpp
    ui/test_HtmlReplay.cpp
    ui/test_SceneHitTest.cpp
    ui/test_PathWindowView.cpp
    ui/test_BackendAdapters.cpp
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
endif()
