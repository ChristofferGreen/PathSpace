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
  unit/inspector/test_PaintScreenshotCard.cpp
  unit/inspector/test_InspectorSnapshot.cpp
  unit/inspector/test_InspectorHttpServer.cpp
  unit/web/test_HtmlHelpers.cpp
  unit/web/test_ServeHtmlOAuthGoogle.cpp
  unit/web/test_ServeHtmlOptions.cpp
  unit/web/test_ServeHtmlSessionStore.cpp
  unit/web/test_ServeHtmlSseBroadcaster.cpp
  unit/web/test_HtmlMirror.cpp
  unit/web/test_PathSpaceHtmlServer.cpp
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
  unit/examples/test_ExampleCli.cpp
  unit/pathspace/test_PathSpaceJsonExporter.cpp
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
  target_sources(PathSpaceTests PRIVATE unit/ui/test_HistoryBinding.cpp)
endif()

if(PATHSPACE_ENABLE_UI)
  add_executable(PathSpaceUITests
    ui/test_SceneHelpers.cpp
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
  ui/test_DeclarativePaintSurface.cpp
  ui/test_ScreenshotOverlay.cpp
  ui/test_WidgetEventTrellis.cpp
  ui/test_WidgetStateMutators.cpp
  ui/test_DeclarativeTheme.cpp
  ui/test_PathWindowView.cpp
  ui/test_WidgetBindingsText.cpp
  ui/test_PaintExampleNew.cpp
  ui/test_BackendAdapters.cpp
  ui/test_DeclarativeRuntime.cpp
  ui/test_DeclarativeScreenshot.cpp
  ui/test_DiagnosticsRuntime.cpp
  ui/test_WidgetGallery.cpp
  ui/test_SurfaceDescValidation.cpp
  ui/test_WidgetRenderPackage.cpp
  ui/test_WidgetEmptyNodes.cpp
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
  PathSpaceServeHtml
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

add_test(NAME PathSpaceServeHtmlHttp
  COMMAND "${PATHSPACE_TEST_RUNNER}"
          --label PathSpaceServeHtmlHttp
          --log-dir "${PATHSPACE_TEST_LOG_DIR}"
          --timeout 120
          --env PATHSPACE_LOG=1
          --env PATHSPACE_TEST_TIMEOUT=1
          --env MallocNanoZone=0
          --
          python3 "${CMAKE_SOURCE_DIR}/tests/tools/test_pathspace_serve_html_http.py"
                  --repo-root "${CMAKE_SOURCE_DIR}"
                  --server "$<TARGET_FILE:pathspace_serve_html>"
)
set_tests_properties(PathSpaceServeHtmlHttp PROPERTIES LABELS serve_html)

add_test(NAME PathSpaceServeHtmlSse
  COMMAND "${PATHSPACE_TEST_RUNNER}"
          --label PathSpaceServeHtmlSse
          --log-dir "${PATHSPACE_TEST_LOG_DIR}"
          --timeout 120
          --env PATHSPACE_LOG=1
          --env PATHSPACE_TEST_TIMEOUT=1
          --env MallocNanoZone=0
          --
          python3 "${CMAKE_SOURCE_DIR}/tests/tools/test_pathspace_serve_html_sse.py"
                  --repo-root "${CMAKE_SOURCE_DIR}"
                  --server "$<TARGET_FILE:pathspace_serve_html>"
)
set_tests_properties(PathSpaceServeHtmlSse PROPERTIES LABELS serve_html)

add_test(NAME PathSpaceServeHtmlGoogle
  COMMAND "${PATHSPACE_TEST_RUNNER}"
          --label PathSpaceServeHtmlGoogle
          --log-dir "${PATHSPACE_TEST_LOG_DIR}"
          --timeout 150
          --env PATHSPACE_LOG=1
          --env PATHSPACE_TEST_TIMEOUT=1
          --env MallocNanoZone=0
          --
          python3 "${CMAKE_SOURCE_DIR}/tests/tools/test_pathspace_serve_html_google.py"
                  --repo-root "${CMAKE_SOURCE_DIR}"
                  --server "$<TARGET_FILE:pathspace_serve_html>"
)
set_tests_properties(PathSpaceServeHtmlGoogle PROPERTIES LABELS serve_html)

add_test(NAME PathSpaceHtmlServerEmbed
  COMMAND "${PATHSPACE_TEST_RUNNER}"
          --label PathSpaceHtmlServerEmbed
          --log-dir "${PATHSPACE_TEST_LOG_DIR}"
          --timeout 90
          --env PATHSPACE_LOG=1
          --env PATHSPACE_TEST_TIMEOUT=1
          --env MallocNanoZone=0
          --
          python3 "${CMAKE_SOURCE_DIR}/tests/tools/test_pathspace_html_server_embed_smoke.py"
                  --repo-root "${CMAKE_SOURCE_DIR}"
                  --server "$<TARGET_FILE:pathspace_html_server_embed>"
)
set_tests_properties(PathSpaceHtmlServerEmbed PROPERTIES LABELS serve_html)

if(PATHSPACE_ENABLE_UI)
  set(PATHSPACE_UI_TEST_COMMON_ENV
      --env PATHSPACE_LOG=1
      --env PATHSPACE_TEST_TIMEOUT=1
      --env MallocNanoZone=0
      --env PATHSPACE_DISABLE_SURFACE_CACHE_WATCH=1
      --env PATHSPACE_CAPTURE_FONT_DIAGNOSTICS=1)

  function(pathspace_add_ui_test LABEL)
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
                    --binary "$<TARGET_FILE:declarative_button_example>"
  )
  set_tests_properties(DeclarativeButtonJsonDump PROPERTIES LABELS ui)

  if(APPLE)
    # PixelNoise perf harness removed until the ServeHtml/example rewrite reinstates it.
  endif()
endif()

add_executable(pathspace_html_server_embed tools/pathspace_html_server_embed.cpp)
target_link_libraries(pathspace_html_server_embed PRIVATE PathSpaceServeHtml)
