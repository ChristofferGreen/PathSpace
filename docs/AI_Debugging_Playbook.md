# Handoff Notice

> **Handoff note (October 19, 2025):** These troubleshooting steps remain accurate but assume the prior tooling defaults. Confirm any helper script changes in `docs/AI_Onboarding_Next.md` before relying on this playbook.
> **Update (October 21, 2025):** Presenter telemetry now mirrors into window diagnostics sinks (`windows/<win>/diagnostics/metrics/live/views/<view>/present/*`); see §3 for usage.

# PathSpace — Debugging Playbook

> **Context update (October 18, 2025):** The test harness now captures detailed logs for every failure via `scripts/run-test-with-logs.sh`. Use this playbook to triage regressions, inspect diagnostics, and close the loop with the mandated 5× test runs.
> **Diagnostics refresh (October 19, 2025):** Dirty-rect hints are coalesced server-side, and the extended damage/fingerprint/progressive metrics are available when `PATHSPACE_UI_DAMAGE_METRICS=1`. Keep the flag unset during perf runs—the software encode pass is still single-threaded today, so we’re focusing on parallelising it next.

This guide consolidates the practical steps for investigating failures across unit tests, UI targets, and runtime metrics. Pair it with the architecture docs (`docs/AI_Architecture.md`, `docs/Plan_SceneGraph_Renderer.md`) when you need deeper background.

## 1. Reproducing and Capturing Failures

### 1.1 Standard looped test run

```bash
./scripts/compile.sh --test --loop=5 --per-test-timeout=20
```

- Runs both `PathSpaceTests` and `PathSpaceUITests` each iteration (unless you filter via the new `--loop-label` flag described below).
- PathSpaceUITests now keeps its logs even when a loop iteration “passes.” Every saved log/artifact pair is recorded in `build/test-logs/loop_manifest.tsv`, which the helper prints after the loop completes.
- Additional logs use the pattern `<test>_loop<iteration>of<total>_<timestamp>.log`; the manifest is the authoritative index when `--loop-keep-logs` names more tests.
- `PATHSPACE_LOG` defaults to `1` in the helper so tagged logging is enabled when an error surfaces; adjust via `--env PATHSPACE_LOG=0` if you need silence.
- Want to exercise the Metal presenter path locally? Append `--enable-metal-tests` (macOS only) so the helper sets `PATHSPACE_UI_METAL=ON` during configuration and runs the suites with `PATHSPACE_ENABLE_METAL_UPLOADS=1`.
- Need structured evidence from the loop run? Set `PATHSPACE_LOOP_BASELINE_OUT=<path>` and the helper will summarize `build/test-logs/loop_manifest.tsv` into that JSON file (iteration count, timeout, participating labels, saved logs/artifacts). CI’s `loop-stability` workflow writes `loop_baseline.json` at the repo root so dashboards always have a fresh baseline before declarative changes land.
- Watching the IO Pump? Check `/system/widgets/runtime/input/metrics/{pointer_events_total,button_events_total,text_events_total,events_dropped_total,last_pump_ns}` plus `/system/widgets/runtime/io/state/running` to confirm the worker is alive and consuming Trellis events. Pump errors share the same `/system/widgets/runtime/input/log/errors/queue` as reducer failures, so grep for `io_pump` tags when diagnosing drops.
- When `PATHSPACE_RECORD_MANUAL_PUMPS=1` the declarative readiness helper snapshots `/system/widgets/runtime/input/windows/<token>/metrics/*` and `/system/widgets/runtime/input/apps/<app>/metrics/*` into each test’s artifact directory (`manual_pump_metrics.jsonl`). Call `python3 scripts/manual_pump_ingest.py --log-root build/test-logs --output build/test-logs/manual_pump_summary.json` (automatically executed by the helper whenever the env var is set) to aggregate those counters for dashboards or quick regressions triage.
- **Visitor snapshots (November 30, 2025):** `PathSpaceBase::visit(PathVisitor const&, VisitOptions const&)` replaces ad-hoc `listChildren` crawls. Set `VisitOptions` (`root`, `maxDepth`, `maxChildren`, `includeNestedSpaces`, `includeValues`) and supply a callback that receives `PathEntry` metadata plus a `ValueHandle`. Use the handle’s `read<T>()` only when `includeValues=true`; otherwise call `snapshot()` to inspect queue metadata without copying bytes. Layers (PathView/Alias/Trellis/UndoableSpace) already delegate to their upstreams, so the same visitor works on aliased spaces, inspector views, or trellis mounts. Set `maxChildren = VisitOptions::kUnlimitedChildren` (0) whenever you need every child; the exporter/CLI will report `"max_children": "unlimited"` so diagnostics know the cap is disabled.
- **JSON dumps (November 30, 2025):** `PathSpaceBase::toJSON(PathSpaceJsonOptions const&)` wraps `PathSpaceJsonExporter` so you can stamp the same subtree structure that the inspector expects. Tune `maxQueueEntries` for queue depth (default 4), set `includeOpaquePlaceholders=false` if you only want converted values, and disable `visit.includeValues` when you just need structural inventories. Attach the emitted JSON to flaky bug reports alongside the compile-loop logs. The supported CLI (`./build/pathspace_dump_json --demo --root /demo --max-depth 3 --max-children 4 --max-queue-entries 2 --output demo.json`) exercises the same exporter and is covered by the `PathSpaceDumpJsonDemo` fixture. Use `PathSpaceJsonRegisterConverterAs<T>("FriendlyType", lambda)` (or the legacy `PathSpaceJsonRegisterConverter`) to register readable type names—those aliases show up in `values[*].type` and the opaque placeholders when converters are missing.

New helper switches (November 21, 2025):

- `--loop-keep-logs LABELS` keeps success logs for additional tests (default = `PathSpaceUITests` when `--loop` is used). Disable via `--loop-keep-logs=none`.
- `--loop-label LABEL` (repeatable, glob-friendly) restricts the loop to specific tests so you can hammer just `PathSpaceUITests` without re-running `PathSpaceTests`.
- `--ui-test-extra-args "--success"` appends doctest flags to `PathSpaceUITests` only; this is the recommended way to surface doctest’s `--success` output when chasing flakes. Environment equivalents exist for automation: `PATHSPACE_LOOP_KEEP_LOGS`, `PATHSPACE_LOOP_LABEL_FILTER`, and `PATHSPACE_UI_TEST_EXTRA_ARGS`.
- Loop filters are validated **after** the test commands register, so `--loop-label PathSpaceUITests` no longer trips the “did not match any configured tests” error when you only want the UI suite. Filtered runs print `Skipping <label> (loop filter)` for clarity.
- Every saved log now ends with a `[test-runner] EXIT …` banner (exit code, decoded signal, or timeout plus UTC timestamp) emitted by `scripts/run-test-with-logs.sh`, so you can tell immediately whether a loop iteration died to SIGTERM, the 20 s timeout, or a non-zero exit before opening the artifacts.
- Undo/redo telemetry lives under `widgets/<id>/metrics/history_binding/*`; when triaging paint_example regressions, read the `card` node (`HistoryBindingTelemetryCard`) to get the current state, button enablement timestamps, undo/redo counters, and last error context without replaying screenshots (`pathspace_inspect --path "<app>/widgets/paint/metrics/history_binding/card"` is the quickest way today).
- Paint example screenshot status now has a first-class CLI: run `pathspace_paint_screenshot_card --metrics-json build/test-logs/paint_example/diagnostics.json [--max-runs N] [--json] [--output-json <path>]` after `scripts/paint_example_diagnostics_ingest.py` (or point it at the JSON emitted by the screenshot tests) to see severity, tolerance, mean error, hardware flag, and artifact paths without opening PNGs. The Example CLI parser surfaces `--help` and consistent validation, and the inspector/web panel reuses the same helper (`SP::Inspector::BuildPaintScreenshotCard`) so what you see locally matches the dashboards.
- Need a browser panel? Build the tree/detail SPA with `cmake --build build -j` and launch `./build/tools/inspector_server [--ui-root <dir>] [--no-demo]`. Open `http://localhost:8765/` to explore `/` (the bundled SPA) and `/inspector/*` (JSON endpoints). The server proxies live declarative data plus the paint screenshot card; `--ui-root` hotloads custom assets and the legacy `scripts/paint_example_inspector_panel.py` is now an optional SSE prototype rather than the primary UI.
- If `PaintExampleScreenshot*` fails with a fully transparent controls column (mean error ≈0.28 even after seam overlay), confirm `examples/paint_example.cpp` is copying the baseline perimeter before diffing (`apply_controls_background_overlay`) and that the manifest is at revision 7. `pathspace_screenshot_cli` now records `mean_error: 0` in `build/artifacts/paint_example/*_metrics.json` when the backfill is active, so the loop can pass even if the runtime leaves gaps outside the canvas.
- When `Window::Present` refuses to capture on headless Metal hosts, rerun with `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1` (or `pathspace_screenshot_cli --force-software`) so the deterministic fallback renderer produces the screenshot. The CLI and Python helper both read `hardware_capture` from the metrics JSON and exit non-zero if a fallback happens without the guard, which keeps default runs honest while still giving loop-stability a way to stay green during Metal outages.

### 1.4 Telemetry, throttling, and subscriber toggles

- `CreateTelemetryControl` is enabled by default (see `LaunchOptions::start_io_telemetry_control`). Use it to flip telemetry without spelunking through every device path:
  - Enable metrics: `space.insert("/_system/telemetry/start/queue", SP::Runtime::TelemetryToggleCommand{.enable = true});`
  - Disable: push `.enable = false` to `/_system/telemetry/stop/queue`.
  - Health: verify `/_system/telemetry/io/state/running = true` and tail `/_system/telemetry/log/errors/queue` for malformed commands.
- Device push/subscriber tweaks:
- Queue `DevicePushCommand{.device = "/system/devices/in/pointer/default", .subscriber = "telemetry_test", .enable = true}` at `/_system/io/push/subscriptions/queue` to opt a device+subscriber pair in. Set `.device = "*"` or `/system/devices/in/pointer/*` to broadcast across multiple mounts. When `.subscriber` is empty the telemetry controller falls back to `TelemetryControlOptions::default_subscriber` (currently `io_trellis`). Use `.set_telemetry = true` to force `config/push/telemetry_enabled` for the same devices.
  - Queue `DeviceThrottleCommand{.device = "*", .set_rate_limit = true, .rate_limit_hz = 480, .set_max_queue = true, .max_queue = 64}` at `/_system/io/push/throttle/queue` to clamp every provider. Leave `set_*` flags false when you do not want a field to change (commands without either flag are ignored and reported via the telemetry log).
- Every command runs asynchronously; confirm success by re-reading the relevant nodes (config paths or metrics roots). When debugging throttling or subscriber issues in production, capture both the command you posted and the resulting telemetry log entry so follow-up shifts understand the change history.

### 1.2 Single test reproduction

Use doctest filters with the wrapper when isolating a case:

```bash
./scripts/run-test-with-logs.sh \
  --label PathSpaceTests.read \
  --log-dir build/test-logs \
  --timeout 20 \
  -- \
  ./build/tests/PathSpaceTests --test-case "PathSpace read blocking"
```

Environment knobs (all respected by the wrapper and the logger):

| Variable | Purpose |
| --- | --- |
| `PATHSPACE_LOG` | Enable/disable log emission (truthy values: `1`, `true`, `on`, etc.). |
| `PATHSPACE_LOG_ENABLE_TAGS` | Comma-separated allowlist (e.g., `TEST,WAIT`). |
| `PATHSPACE_LOG_SKIP_TAGS` | Comma-separated blocklist. |
| `PATHSPACE_TEST_TIMEOUT` | Millisecond poll interval for blocking tests (default `1`). |
| `MallocNanoZone` | Set to `0` to reduce allocation overhead on macOS (default from helper). |
| `PATHSPACE_ENABLE_METAL_UPLOADS` | Opt-in Metal texture uploads during UI tests; leave unset in CI/headless runs so builders fall back to the software raster. |

### 1.3 Widget session capture and replay

- Record an interactive widgets gallery run (creates a pointer/keyboard trace with per-event metadata):

  ```bash
  ./scripts/record_widget_session.sh \
    --output traces/widget-hover-repro.trace
  ```

  The script builds `widgets_example` (unless `--no-build` is supplied), launches it, and writes the event stream to `WIDGETS_EXAMPLE_TRACE_RECORD`. The C++ helper ensures parent directories are created; traces are newline-delimited and suitable for diffing.

- Replay a trace headlessly (no macOS window is opened) and optionally run the UI suite afterwards:

  ```bash
  ./scripts/replay_widget_session.sh \
    --input traces/widget-hover-repro.trace \
    --run-tests
  ```

  Under the hood the script exports `WIDGETS_EXAMPLE_HEADLESS=1` and `WIDGETS_EXAMPLE_TRACE_REPLAY=<trace>` so `widgets_example` replays events deterministically. Add extra arguments after `--` to tweak theme or other CLI flags.

- Environment variables:

  | Variable | Purpose |
  | --- | --- |
  | `WIDGETS_EXAMPLE_TRACE_RECORD` | Absolute/relative path that receives recorded pointer + keyboard events. Set automatically by the capture script. |
  | `WIDGETS_EXAMPLE_TRACE_REPLAY` | Path to a trace file to replay. When present, `widgets_example` skips the LocalWindow bridge and replays events headlessly. |
  | `WIDGETS_EXAMPLE_HEADLESS` | When truthy, suppresses the interactive window. The replay script defaults this to `1` so replays run on CI hosts. |

  The recorder/replayer now lives in shared UI tooling—include `pathspace/ui/WidgetTrace.hpp` and construct `SP::UI::WidgetTraceOptions` with your own env var names when you want the same workflow in other apps or tests.

### 1.4 Widget gallery screenshots

- Capture a deterministic PNG of the gallery UI without manual interaction:

  ```bash
  ./build/widgets_example --screenshot out/widgets_gallery.png
  ```

  The binary now runs entirely headless for this mode: it boots the gallery, drives a scripted slider drag to exercise focus/dirty paths, renders a single frame, writes a PNG via `stb_image_write` (creating parent directories when needed), prints the path, and exits. Use this to verify focus highlights, themes, or layout regressions when you can’t interact with the GUI directly. A current sample lives at `docs/images/widgets_gallery_drag.png`.

`./build/devices_example --paint-controls-demo --screenshot <path>` shares the headless CLI described above (`--screenshot-compare/--screenshot-diff/--screenshot-metrics`, `--screenshot-max-mean-error`, `--screenshot-force-software`, `--screenshot-allow-software-fallback`). It reuses the helper in `<pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>`, so you can capture the paint-controls UI without bespoke glue. `declarative_hello_example` stays CLI-free as the literal quickstart, but exporting `PATHSPACE_HELLO_SCREENSHOT=out/declarative_hello.png` now routes through the same helper and, when hardware capture is unavailable, falls back to a deterministic reference render so the docs always have a screenshot to reference.
Recent fix: declarative presents once again honor `/present/params/capture_framebuffer`, and the screenshot helper always runs a present pass even when `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1`, so the env-driven hello screenshot captures the actual UI unless both hardware and software readbacks fail (in which case the deterministic PNG is still emitted).

- Capture the declarative paint demo without opening a window:

  ```bash
  ./build/paint_example --screenshot out/paint_demo.png
  ```

  This flag flips the sample into headless mode, replays a fixed set of brush strokes through `PaintRuntime::HandleAction`, enables framebuffer capture, presents once, and saves `out/paint_demo.png`. Re-run after UI or renderer tweaks to produce before/after PNGs for quick visual diffs; the scripted strokes keep outputs stable across hosts.
  Add `--screenshot-compare=PATH` to diff the live framebuffer against a baseline PNG, `--screenshot-diff=PATH` to emit a visual heatmap of the differences, `--screenshot-max-mean-error=<0-1>` to configure the allowed mean absolute error (default `0.0015` to accommodate the slider highlight’s subpixel jitter), and `--screenshot-require-present` to fail immediately if `Window::Present` falls back to the software renderer.
  `scripts/check_paint_screenshot.py --build-dir build` wraps this workflow for CI/CTest: it runs `paint_example --gpu-smoke --screenshot … --screenshot-compare=docs/images/paint_example_baseline.png`, enforces framebuffer capture, writes artifacts under `build/artifacts/paint_example/`, and surfaces diffs when the declarative paint UI regresses (the script keeps the diff PNG whenever the mean error exceeds the tolerance). The C++ CLI now mirrors the same behavior: every capture is forced through `SceneLifecycle::ForcePublish`, runs inside an isolated child process, and automatically retries up to three times (0.5 s backoff) before giving up so compile-loop flakes leave deterministic logs.
  - The capture helper now also forwards manifest metadata (`PAINT_EXAMPLE_BASELINE_{WIDTH,HEIGHT,RENDERER,CAPTURED_AT,COMMIT,NOTES,SHA256,TOLERANCE}` plus the manifest revision/tag) to `paint_example`, so the binary publishes `/diagnostics/ui/paint_example/screenshot_baseline/{manifest_*,last_run/*}` and mirrors the payload to JSON when `--screenshot-metrics-json` (passed by default) is present. Each screenshot run leaves a metrics snapshot under `build/artifacts/paint_example/<tag>_metrics.json`; aggregate them with `scripts/paint_example_diagnostics_ingest.py --inputs build/artifacts/paint_example/*_metrics.json --output-json build/test-logs/paint_example/diagnostics.json --output-html build/test-logs/paint_example/diagnostics.html` to feed the inspector/web dashboards (covered by `tests/tools/test_paint_example_diagnostics_ingest.py`).
  - `scripts/paint_example_monitor.py` now enforces the manifest revision lock (`docs/images/paint_example_manifest_revision.txt`), validates that all guardrail runs emitted screenshots/metrics for the tracked tags, and writes `build/test-logs/paint_example/monitor_report.json`. The compile loop, CTest suite, and pre-push hook run the monitor immediately after the three `pathspace_screenshot_cli` guards, so palette/descriptor changes cannot land unless the manifest + lock file are refreshed via `scripts/paint_example_capture.py --tags …`.
  If the controls column comes up empty, set `PATHSPACE_UI_DEBUG_STACK_LAYOUT=1` (legacy `PAINT_EXAMPLE_DEBUG_LAYOUT=1` still toggles the same logger) before running the command; the binary routes the shared `SP::UI::Declarative::WaitForStackChildren` logs to stderr so you can see exactly which child IDs are still missing. Any declarative stack can reuse the helper (`include/pathspace/ui/declarative/StackReadiness.hpp`) to get the same diagnostics.

- Smoke-test the declarative paint GPU uploader without launching the UI:

  ```bash
  ./build/paint_example --gpu-smoke=out/paint_gpu.png
  ```

  The `--gpu-smoke` flag enables the paint widget’s GPU staging path, replays the scripted strokes, waits for `render/gpu/state` to transition to `Ready`, confirms dirty queues drain, prints a staged-texture digest, and (optionally) dumps the RGBA payload to `out/paint_gpu.png` for pixel diffing. Use it in CI or before landing renderer changes to ensure the declarative pipeline replaces the legacy smoke run.

- `build/tests/PathSpaceUITests --test-case "Widget focus slider-to-list transition covers highlight footprint"` guards the historical lingering highlight bug. The case now asserts that dirty hints cover both slider and list footprints, checks focus hand-off, and compares framebuffer diffs; it must pass. If it fails, confirm slider footprints are persisted and that `Widgets::Input::FocusHighlightPadding()` matches the renderer’s highlight inflation before digging into pointer routing.
- Because the capture is headless, the LocalWindow bridge is skipped—no IOSurface hand-off is required. If you need to reproduce an interactive issue instead, run without `--screenshot` or use the trace replay helpers below.

### 1.5 Sanitizer runs on demand

- Run AddressSanitizer or ThreadSanitizer loops without juggling flags manually:

  ```bash
  ./scripts/compile.sh --asan-test         # builds in ./build-asan, runs tests once
  ./scripts/compile.sh --tsan-test --loop=5 --per-test-timeout=40
  ```

  These presets:
  - point CMake at dedicated build directories (`./build-asan`, `./build-tsan`) unless you override `--build-dir`;
  - disable Metal presenter coverage by default (sanitizers emit false positives inside the OS frameworks);
  - export baseline runtime settings (`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_init_order=1`, `TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0`) while keeping user-provided values intact.
  Adjust loop counts with `--loop=<n>` or the usual test arguments (`--args`, `--per-test-timeout`, etc.).

- Pre-push hook toggles mirror the CLI presets so you can opt-in sanitized runs during local pushes:

  ```bash
  RUN_ASAN=1 SANITIZER_CLEAN=1 ./scripts/git-hooks/pre-push.local.sh
  RUN_TSAN=1 TSAN_LOOP=3 ./scripts/git-hooks/pre-push.local.sh
  ```

  Available knobs: `RUN_ASAN=1`, `RUN_TSAN=1`, `ASAN_LOOP=<n>`, `TSAN_LOOP=<n>`, `SANITIZER_CLEAN=1` (forces `--clean`), and `SANITIZER_BUILD_TYPE=<Debug|RelWithDebInfo|...>` to override the default debug build.

## 2. Inspecting Collected Logs

1. Open the saved log file (e.g., `build/test-logs/PathSpaceTests_loop3of5_20251018-161200.log`).

### Focus metadata quick checks (November 15, 2025)

- Declarative focus issues? Inspect `widgets/focus/current` for the active widget path, `widgets/<id>/focus/current` for per-widget booleans, and `widgets/<id>/focus/order` for the depth-first index. Every scene mirrors the active widget under `scenes/<scene>/structure/window/<window-id>/focus/current`; if that string is empty while `widgets/<id>/focus/current = true`, the scene detachment logic is out of sync.
- `WidgetFocus::BuildWindowOrder` now dumps the computed traversal order—use it in targeted doctests or REPL builds when you need to confirm that depth-first ordering matches the mounted widget tree.
- `Widgets::Focus::Move(space, config, Direction)` now consumes the stored `focus/order` metadata directly, so you can reproduce Tab/Shift+Tab or gamepad hops in doctests by calling the controller without crafting bespoke focus lists. The helper automatically schedules the `focus-navigation` auto-render events, making it easy to verify dirty-rect hints in isolation.
2. The tail section is also echoed to stderr at the time of failure; the full file includes tagged entries (`[TEST][INFO] …`) plus doctest progress markers.
3. When ASAN/UBSAN is enabled, the helper preserves the sanitizer output verbatim. Re-run with `PATHSPACE_ENABLE_CORES=1` to generate core dumps for deeper analysis.

## 3. PathSpace Runtime Diagnostics

- **Structured errors:** Renderer/presenter components publish `PathSpaceError` payloads under `renderers/<rid>/targets/<tid>/diagnostics/errors/live`. In tests, call `Diagnostics::ReadTargetMetrics` to fetch the payload and correlate with frame indices.
- **Per-target metrics:** `renderers/<rid>/targets/<tid>/output/v1/common/*` stores the latest `frameIndex`, `revision`, `renderMs`, progressive copy counters (including `progressiveTileSize`, `progressiveWorkersUsed`, `progressiveJobs`), encode fan-out stats (`encodeWorkersUsed`, `encodeJobs`), backend telemetry (`backendKind`, `usedMetalTexture`), GPU timings (`gpuEncodeMs`, `gpuPresentMs`), present policy outcomes, and—when `PATHSPACE_UI_DAMAGE_METRICS=1` is set—damage/fingerprint telemetry (`damageRectangles`, `damageCoverageRatio`, `fingerprint*`, `progressiveTiles*`). Use `PathWindowView` doctest helpers or the builders’ diagnostics reader to dump these values.
  - **New (October 27, 2025):** `damageTiles` lists the tile-aligned damage rectangles the renderer cleared for the last frame. Enable `PATHSPACE_UI_DAMAGE_METRICS=1` for the extended counters; the tile list is always populated and is handy when cross-checking the widget footprints logged by `widgets_example`.
- **Trellis runtime mirrors (November 14, 2025):** The previous PathSpaceTrellis layer and its diagnostics were removed on November 14, 2025. Historical notes remain for context only; new fan-in tooling will be documented once the replacement lands.
- **Window diagnostics sinks:** Presenter metrics mirror into `windows/<win>/diagnostics/metrics/live/views/<view>/present/*`, including frame indices, present/render timings, progressive counters, last error strings, and timestamps, so dashboards can monitor windows without crawling every renderer target. With `PATHSPACE_UI_DAMAGE_METRICS=1` the mirror also carries the tile-damage diagnostics (`progressiveTiles{Dirty,Total,Skipped}`, `progressiveTilesUpdated`, `progressiveBytesCopied`, encode worker/job counts) alongside the core progressive copy telemetry.
- **Font manager metrics (October 30, 2025):** `diagnostics/metrics/fonts/*` now captures font registration + cache telemetry: `registeredFonts`, `cacheHits`, `cacheMisses`, `cacheEvictions`, `cacheSize`, `cacheCapacity`, `cacheHardCapacity`, `atlasSoftBytes`, `atlasHardBytes`, and `shapedRunApproxBytes`. Check these nodes when typography glitches appear—cache thrash, mismatched budgets, or an empty registration set often signals missing `FontManager::register_font` calls or stale font metadata. The metrics publish on every registration and `FontManager::shape_text` invocation. Use `FontManager::resolve_font` during triage to confirm the stored font meta (family, style, weight, fallbacks) and active atlas revisions match expectations.
- **Dashboards & thresholds:** Residency, progressive, and perf guardrail feeds are wired into the shared dashboards so on-call engineers do not need to reverse engineer path names when paging:
  | Topic | Metrics Nodes | Dashboard / Panel | Thresholds & Alerts |
  | --- | --- | --- | --- |
  | Residency budgets | `diagnostics/metrics/residency/<target>/{cpu,gpu}{Soft,Hard}{Bytes,BudgetRatio}`, `overallStatus` | Grafana → **Renderer Residency** (panels: CPU Budget %, GPU Budget %, Residency Status) | `overallStatus != ok` pages the **Renderer Residency** alert; soft ratios > 0.85 raise yellow, hard ratios > 0.95 raise red. |
  | Progressive tiles & encode workers | `output/v1/common/<target>/progressive*`, `diagnostics/metrics/residency/<target>/progressive*`, plus mirrored copies under `windows/<win>/diagnostics/metrics/live/views/<view>/present/*` | Grafana → **Renderer Progressive Health** (panels: Damage Coverage %, Tiles Dirty/Total, Encode Worker Utilisation) | Coverage < 0.05 for ≥5 min triggers **Progressive Stalled**; encode workers or jobs = 0 while damage > 0 triggers **Encode Idle With Damage**. |
  | Perf guardrail trends | `build/perf/history/*.json` (produced by `scripts/perf_guardrail.py --record`), `docs/perf/performance_baseline.json` | Grafana → **Renderer Performance Guardrail** (panels: Frame Time vs Baseline, FPS vs Budget, Size Delta) | Guardrail script marks regressions as `status=fail`; dashboards watch for the flag and raise **Renderer Perf Regression** if any entry fails for 2 consecutive runs. |
  | Widget ops & action throughput | `widgets/<id>/ops/{inbox,actions}/metrics/*` (published by reducers) | Grafana → **Widget Action Flow** (panels: Ops Inflight, Actions/sec, Focus Handoffs) | Actions/sec trending to zero with non-empty inbox triggers **Widget Action Backlog**. |

  The dashboards live under the shared `PathSpace / UI` folder; bookmark the Grafana search `dash:pathspace-ui` to jump directly to the four panels above. Update this table whenever new metrics land or panel names change so responders can correlate alerts with the emitting path quickly.
- **Scene dirty state:** `scenes/<sid>/diagnostics/dirty/state` and `scenes/<sid>/diagnostics/dirty/queue` expose layout/build notifications. Use `SP::UI::Declarative::SceneLifecycle::MarkDirty` (or the legacy `SP::UI::Builders::Scene::MarkDirty` shim) to enqueue events; the declarative doctests show how to wait on these paths without polling.
- **HTML/IO logs:** Application surfaces write to `<app>/io/log/{error,warn,info,debug}`. The global mirrors live at `/system/io/std{out,err}`; see `docs/AI_Paths.md` §2 for the canonical layout.

## 4. Workflow Checklist After a Failure

1. **Collect artifacts**
   - Inspect the saved log file(s) under `build/test-logs/`.
   - Preserve any core dumps or sanitizer traces (enable with `PATHSPACE_ENABLE_CORES=1` if needed).
2. **Correlate with diagnostics**
   - Query `diagnostics/errors/live` and `output/v1/common/*` for affected targets.
   - For scene issues, inspect `diagnostics/dirty/*` to confirm dirty markers and queues behave as expected.
3. **Isolate the regression**
   - Re-run the failing test with doctest filters and specific tags enabled (`PATHSPACE_LOG_ENABLE_TAGS=TEST,UI`).
   - Use `--loop=3` on the helper to confirm the fix eliminates intermittent races before scaling back to the mandated 5.
4. **Document findings**
   - Update the relevant plan doc (`docs/Plan_SceneGraph.md` or task entry) with repro steps, log references, and next actions.

## 5. Tooling Quick Reference

| Task | Command |
| --- | --- |
| Run both suites once with logs | `./scripts/compile.sh --test` |
| Run both suites 5× with 20 s timeout | `./scripts/compile.sh --test --loop=5 --per-test-timeout=20` |
| Run suites once with Metal presenter enabled (macOS) | `./scripts/compile.sh --enable-metal-tests --test` |
| Run a single suite via CTest | `ctest --output-on-failure -R PathSpaceUITests` |
| Re-run failed tests only | `ctest --rerun-failed --output-on-failure` |
| Verify HTML adapter command stream | `ctest -R HtmlCanvasVerify --output-on-failure` |
| Verify HSAT asset inspection tooling | `ctest -R HtmlAssetInspect --output-on-failure` |
| Inspect HSAT payload contents manually | `./build/pathspace_hsat_inspect [--input <payload.hsat> | --stdin | -] [--pretty]` |
| Inspect Undo history on disk | `./build/pathspace_history_inspect [--history-root <history_dir>] [history_dir]` |
| Export PathSpace JSON snapshot | `./build/pathspace_dump_json --root /demo --max-depth 3 --max-children 4 --max-queue-entries 2 --output dump.json` |
| Export/import undo savefiles | `./build/pathspace_history_savefile <export|import> --root <path> --history-dir <dir> (--out/--in <bundle>.history.journal.v1>) [--persistence-root <dir>] [--namespace <token>] [--help]` |
| Smoke-test savefile CLI roundtrip | `./build/pathspace_history_cli_roundtrip [--keep-scratch] [--archive-dir <dir>] [--scratch-root <dir>] [--debug-logging]` |
| Run CLI roundtrip regression | `ctest -R HistorySavefileCLIRoundTrip --output-on-failure` |
| Tail latest failure log | `ls -t build/test-logs | head -1 | xargs -I{} tail -n 80 build/test-logs/{}` |
| Inspect renderer metrics path | `build/tests/PathSpaceUITests --test-case Diagnostics::ReadTargetMetrics` |
| Benchmark damage/fingerprint metrics | `./build/benchmarks/path_renderer2d_benchmark --metrics [--canvas=WIDTHxHEIGHT]` |
| Capture renderer FPS traces | `./scripts/capture_renderer_fps_traces.py --pretty` |
| Capture pixel-noise baseline JSON | `./scripts/capture_pixel_noise_baseline.sh` |
| Check pixel-noise run against budgets | `python3 scripts/check_pixel_noise_baseline.py --build-dir build` |
| Capture pixel-noise PNG frame | `./build/pixel_noise_example --headless --frames=1 --write-frame=docs/images/perf/pixel_noise.png` |
| Guardrail demo binary sizes | `./scripts/compile.sh --size-report` |

- `./scripts/compile.sh --test` now runs the PixelNoise perf harness (software and, when enabled, Metal) alongside the core and UI test executables. The mandated 5× loop therefore enforces the perf budgets without a separate CTest call—refresh `docs/perf/pixel_noise_baseline.json` and `docs/perf/pixel_noise_metal_baseline.json` whenever thresholds legitimately move.

### 5.1 Pixel Noise Perf Harness Baselines

- Use `./scripts/capture_pixel_noise_baseline.sh` after rebuilding (`cmake --build build -j`) to refresh `docs/perf/pixel_noise_baseline.json`.
- Pass `--backend=metal` (and export `PATHSPACE_ENABLE_METAL_UPLOADS=1`) to capture the Metal2D variant in `docs/perf/pixel_noise_metal_baseline.json`; commit both baselines together when budgets shift.
- The helper launches `pixel_noise_example` headless with the standard perf budgets (≥25 FPS, ≤20 ms render/present, ≤40 ms present-call post shaped-text rollout) and records the run via `--write-baseline=<path>`.
- `python3 scripts/check_pixel_noise_baseline.py --build-dir build` reruns the harness with the recorded parameters, writes a temporary metrics snapshot, and fails if the averaged frame times exceed the stored budgets or if FPS dips below the baseline threshold. The script respects the baseline’s `backendKind`, forwarding the matching `--backend` flag and enabling Metal uploads automatically when needed. `PixelNoisePerfHarness` (software) and `PixelNoisePerfHarnessMetal` (Metal, PATHSPACE_UI_METAL builds) in CTest now go through the same script so regressions surface during the 5× loop.
- Inspect the resulting JSON for:
  - `summary.*` — aggregate FPS and timing averages used to confirm budgets.
  - `tileStats.*` — progressive tiling activity (tiles updated/dirty/skipped/copied, worker/job counts) for spotting regression hot spots.
  - `residency.*` — mirrored metrics from `diagnostics/metrics/residency/*` so residency budget drifts surface during perf reviews.
- Pair the JSON with the console summary emitted by the example (`pixel_noise_example: summary …`) when reporting numbers in PRs.
- When updating visuals, run `./build/pixel_noise_example --headless --frames=1 --write-frame=docs/images/perf/pixel_noise.png` to refresh the canonical PNG shown in docs; the flag enables framebuffer capture automatically.
- Keep older baselines committed when behaviour intentionally shifts; diffing JSON across commits highlights the magnitude of a regression or improvement.

### 5.2 Demo Binary Size Guardrail

- Run `./scripts/compile.sh --size-report` to build the demo executables with `BUILD_PATHSPACE_EXAMPLES=ON` and compare their sizes against `docs/perf/example_size_baseline.json`. The helper fails the build when a binary exceeds the baseline by more than 5 % or 256 KiB (whichever is larger).
- Record a new baseline with `./scripts/compile.sh --size-write-baseline` after intentional asset or dependency additions. Commit the refreshed JSON alongside the relevant code/doc changes.
- Baseline entries live under `docs/perf/example_size_baseline.json` and track `devices_example`, `html_replay_example`, `paint_example`, `pixel_noise_example`, and `widgets_example`. Update the doc when new demos are added so the guardrail lists stay in sync.

### 5.3 Renderer/Presenter Performance Guardrail

- Run `./scripts/perf_guardrail.py --build-dir build --build-type Release --jobs <n>` to execute all registered scenarios (PathRenderer2D, pixel noise, and the declarative-vs-legacy widget pipeline), compare their metrics against `docs/perf/performance_baseline.json`, and fail if regressions exceed per-metric tolerances.
- Use `./scripts/perf_guardrail.py --scenarios widget_pipeline --print` for a fast local read on declarative widget latency/dirty throughput regressions before kicking off the longer renderer runs.
- Append `--history-dir build/perf/history --print` when iterating locally; the helper writes a JSONL snapshot per scenario so you can trend metrics after each change.
- Refresh the baseline only after intentional performance wins: `./scripts/perf_guardrail.py --build-dir build --build-type Release --jobs <n> --write-baseline`. Commit the updated JSON alongside the change and note the justification in the PR description.
- The guardrail runs automatically from `scripts/compile.sh --perf-report` and the local pre-push hook; export `SKIP_PERF_GUARDRAIL=1` sparingly (e.g., when profiling on unsupported hardware) and document the reason in the PR.
- Inspect `docs/perf/performance_baseline.json` for the tracked metrics. Key checks today:
  - `path_renderer2d` scenario covers full-repaint vs incremental workloads (avg ms, FPS, damage ratios, tile counts).
  - `pixel_noise_software` scenario validates presenter timings (average FPS, render/present/present-call ms, bytes copied per frame).
- Keep the baseline and tolerances in sync with `docs/perf/README.md` when new scenarios are added.

### 5.4 Undo history inspection

- Build the CLI with `cmake --build build -j` and run `./build/pathspace_history_inspect --history-root <history_dir>` (or pass the directory positionally) to audit persisted undo stacks. The Example CLI parser now standardizes `--help`, error messages, and duplicate-input checks; the tool emits the lightweight journal summary (entry count, inserts, takes, barrier markers) and snapshot decoding remains removed alongside the snapshot backend. Pull structured telemetry from `_history/stats/*` at runtime if deeper analysis is required.
- Point the CLI at `${PATHSPACE_HISTORY_ROOT:-$TMPDIR/pathspace_history}/<space_uuid>/<encoded_root>` when reproducing bugs; pair the findings with the `_history/stats/*` inspector nodes referenced in `docs/finished/Plan_PathSpace_UndoHistory_Finished.md`.
- Use `./build/pathspace_history_savefile export --root /doc --history-dir $PATHSPACE_HISTORY_ROOT/<space_uuid>/<encoded_root> --out doc.history.journal.v1 [--persistence-root <dir>] [--namespace <token>]` to author journal savefiles directly from persisted history directories. The CLI now validates mutually exclusive `--in/--out` choices, exposes `--help`, and reuses the shared Example CLI error channel. Pair with `import --root /doc --history-dir … --in doc.history.journal.v1 [--no-apply-options]` when seeding a fresh subtree before reproducing a bug—append `--no-apply-options` if you need to preserve local retention knobs.
- The CLI works alongside the programmatic helpers (`UndoableSpace::exportHistorySavefile` / `importHistorySavefile`) so integration tests can still call straight into C++, but editor/recovery scripts should prefer the CLI to avoid bespoke harnesses. Update postmortem docs with the exact command + PSJL bundle whenever you capture undo history for analysis.
- `./build/pathspace_history_cli_roundtrip [--keep-scratch] [--archive-dir <dir>] [--scratch-root <dir>] [--debug-logging]` exercises both CLI commands against a temporary persistence tree, re-exports the result, and diffs the summaries (values, undo/redo counts). The harness still writes `history_cli_roundtrip/telemetry.json` (bundle hashes, entry/byte counts) plus `original.history.journal.v1` / `roundtrip.history.journal.v1` into the active test artifact directory, so dashboards/inspector tooling can scrape the data automatically. Pre-push runs pick up the same artifacts under `build/test-logs/history_cli_roundtrip/` with a timestamped subdirectory.
- The dedicated regression lives in CTest as `HistorySavefileCLIRoundTrip`; run it (or the pre-push hook) whenever the savefile codec or CLI surface changes to ensure PSJL bundles continue to round-trip end-to-end.
- Override the artifact destination with `./build/pathspace_history_cli_roundtrip --archive-dir /path/to/dir` (falling back to `PATHSPACE_CLI_ROUNDTRIP_ARCHIVE_DIR` or `PATHSPACE_TEST_ARTIFACT_DIR` when omitted), pass `--keep-scratch` when you need to inspect the export/import directories after the run, and use `--scratch-root <dir>` to redirect the temporary tree into a deterministic location for scripting.
- Aggregate the telemetry for dashboards/inspector runs with `scripts/history_cli_roundtrip_ingest.py --artifacts-root build/test-logs --relative-base build --output build/test-logs/history_cli_roundtrip/index.json`. The pre-push hook already invokes this helper, yielding a rolling history of bundle hashes, undo/redo counts, and direct download links for each PSJL pair.
- Pass `--html-output build/test-logs/history_cli_roundtrip/dashboard.html` (enabled by default in the pre-push hook) to emit an inline dashboard that charts undo/redo counts and disk usage trends while linking directly to the archived PSJL bundles and telemetry JSON.

#### History CLI telemetry sample

The roundtrip helper prints a compact telemetry summary so you can compare the persisted journal before and after import without digging through the artifact tree. Treat `original` as the on-disk baseline, `roundtrip` as the freshly re-exported bundle, and `import` as the live state after replaying the journal into a fresh root. A representative output (captured November 10, 2025) looks like:

```json
Telemetry: {
  "timestampIso": "2025-11-10T12:25:48.056Z",
  "original": {
    "hashFnv1a64": "05716157ded25f7e",
    "sizeBytes": 328,
    "undoCount": 2,
    "redoCount": 0,
    "diskEntries": 0,
    "undoBytes": 203,
    "redoBytes": 0,
    "liveBytes": 17,
    "manualGarbageCollect": false
  },
  "roundtrip": {
    "hashFnv1a64": "05716157ded25f7e",
    "sizeBytes": 328,
    "undoCount": 2,
    "redoCount": 0,
    "diskEntries": 0,
    "undoBytes": 203,
    "redoBytes": 0,
    "liveBytes": 17,
    "manualGarbageCollect": false
  },
  "import": {
    "undoCount": 2,
    "redoCount": 0,
    "diskEntries": 2,
    "cachedUndo": 2,
    "cachedRedo": 0,
    "manualGarbageCollect": false,
    "undoBytes": 203,
    "redoBytes": 0,
    "liveBytes": 17,
    "diskBytes": 265,
    "totalBytes": 220
  }
}
```

Check that `hashFnv1a64`, counts, and byte totals match between `original` and `roundtrip` to confirm the export/import loop is lossless. Differences under `import` are expected: it reports the live in-memory state after replay (including cache counters and disk usage). Capture a “before” sample, run your repro or retention workflow, then capture an “after” sample to quantify how trims or manual garbage-collect operations affected the journal.

#### Undo journal migration runbook
1. **Detect legacy persistence** — If a history root still contains `state.meta`, `snapshots/`, and `entries/`, it was authored by the snapshot backend. Do not mount the directory on the journal-only build.
2. **Export with the bridge commit** — Check out the last pre-removal commit (`git log --before '2025-11-10' -1` surfaces the hash while snapshots were still active), build the tooling, and run  
   `./build/pathspace_history_savefile export --root <path> --history-dir <legacy-dir> --out <bundle>.history.journal.v1`.  
   The bridge release transparently loads snapshot persistence, replays it through the journal layer, and emits the new bundle format.
3. **Import on main** — Return to `master`, create an empty persistence directory, and execute  
   `./build/pathspace_history_savefile import --root <path> --history-dir <new-dir> --in <bundle>.history.journal.v1`.  
   Verify with a trivial insert/undo cycle and `./build/pathspace_history_inspect <new-dir>`—undo/redo counts should match the exported summary.
4. **Archive artifacts** — Store the exported bundle with your handoff notes, then delete the legacy snapshot directories (or move them into `_archive/`) so future tooling never mixes formats. All subsequent exports should rely on the journal build.

Record the export/import commands plus validation output in the working note so the next shift can trace exactly how the migration completed. If the bridge commit or tooling changes, update this section and the matching plan entry.

## 6. Closing the Loop

Always finish a debugging session by:

1. Re-running the full loop: `./scripts/compile.sh --test --loop=5 --per-test-timeout=20`
2. Verifying no new logs were emitted (`find build/test-logs -type f -mmin -5` should be empty on success).
3. Recording the outcome (pass/fail, relevant paths, log snippets) in the working note or PR description for traceability.

With the shared test runner and this playbook, every failure leaves behind actionable artifacts, keeping the UI/renderer hardening efforts measurable and repeatable.

## 7. Current Blockers (October 20, 2025)

- None. The HTML asset hydration failure was cleared on October 20, 2025 by introducing a dedicated `Html::Asset` serialization codec (`include/pathspace/ui/HtmlSerialization.hpp`) and a regression test (`Html::Asset vectors survive PathSpace round-trip`). Reconfirm with `ctest --test-dir build --output-on-failure -R "Html::Asset"` or the full `PathSpaceUITests` loop when touching the renderer stack.

## 8. Inspector Embedding & Usage

### 8.1 Run the standalone host

- Build the tree (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`) and launch the helper:

  ```bash
  ./build/pathspace_inspector_server \
    --host 0.0.0.0 \
    --port 0 \
    --root /app \
    --max-depth 3 \
    --max-children 64 \
    --diagnostics-root /diagnostics/ui/paint_example \
    --ui-root out/inspector-ui \
    --no-demo
  ```

- Omitting `--no-demo` seeds the deterministic demo data so you can poke the UI without attaching to a live app. `--ui-root <dir>` hotloads custom SPA builds; leave it unset to serve the embedded assets. Pair `--no-ui` with a reverse proxy when you only need the JSON/SSE endpoints.
- `--stream-poll-ms`, `--stream-keepalive-ms`, `--stream-idle-timeout-ms`, `--stream-max-pending`, and `--stream-max-events` mirror the runtime knobs so you can stress-test throttling/backpressure without editing source.

### 8.2 Embed inside a PathSpace app

1. Include `inspector/InspectorHttpServer.hpp` and construct the server next to the `PathSpace` instance you already expose to declarative widgets.
2. Configure `InspectorHttpServer::Options`:
   - `host`/`port` default to `127.0.0.1:8765`. Set `port = 0` for an ephemeral port that you can log via `server.port()`.
   - Scope `snapshot.root` to the subtree you want exposed (defaults: root `/`, depth = 2, children = 32, include values = true). `max_depth`, `max_children`, and `include_values` can also be overridden per request via query parameters.
   - Update `stream.poll_interval` / `keepalive_interval` for SSE-heavy dashboards (defaults 350 ms and 5 s; anything under 100 ms is clamped server-side).
   - Clamp per-session budgets with `stream.max_pending_events` (defaults to 64 queued events), `stream.max_events_per_tick` (defaults to 8 events flushed per scheduler pass), and `stream.idle_timeout` (defaults to 30 s). Those values drive `/inspector/metrics/stream` and the SPA’s stream-health card so ops can see when a connection starts dropping/resending snapshots.
   - Point `paint_card.diagnostics_root` at your paint example metrics if they differ from `/diagnostics/ui/paint_example`.
   - Use `ui_root` (empty by default) to serve a local UI build or set `enable_ui=false` when another server handles static assets.
3. Start the server during startup, log the bound port, and stop/join it during shutdown:

   ```cpp
   SP::Inspector::InspectorHttpServer::Options options;
   options.host                  = "0.0.0.0";
   options.port                  = 0;             // pick a free port automatically
   options.snapshot.root         = "/app";
   options.snapshot.max_depth    = 3;
   options.snapshot.max_children = 64;

   SP::Inspector::InspectorHttpServer server(space, options);
   if (auto started = server.start(); !started) {
       std::fprintf(stderr,
                    "Inspector failed to start: %s\n",
                    SP::describeError(started.error()).c_str());
       std::exit(1);
   }

   std::printf("Inspector listening on %s:%u\n",
               options.host.c_str(),
               static_cast<unsigned>(server.port()));

   // Later in shutdown handlers
   server.stop();
   server.join();
   ```

4. When embedding into production builds, leave `ui_root` empty (serves the bundled SPA) unless you have a hardened frontend build directory. Document the chosen root/port in your service README so operators know where the inspector lives.

### 8.3 Endpoint + SSE checklist

- `/inspector/tree?root=/demo&depth=3&max_children=64&include_values=1` returns a bounded JSON snapshot. Useful for sanity checks and manual refresh fallbacks.
- `/inspector/node?path=/demo/widget` drills into a specific node without resending the entire tree.
- `/inspector/cards/paint-example?diagnostics_root=/diagnostics/ui/paint_example` exposes the screenshot manifest + last-run stats that also power the SPA card.
- `/inspector/stream?root=/demo&poll_ms=300&keepalive_ms=4000` emits an initial `event: snapshot` frame followed by `event: delta` frames built from `changes.{added,updated,removed}`. Test locally with:

  ```bash
  curl -N http://127.0.0.1:8765/inspector/stream?root=/demo&poll_ms=300
  ```

- Clients may request faster/slower updates via `poll_ms` (min 100 ms) and `keepalive_ms`. The SPA surfaces stream state (idle/connected/error) and falls back to manual refresh when EventSource is unavailable.
- `/inspector/metrics/stream` (and the mirrored `/inspector/metrics/stream/*` nodes) surface queue depth, max depth seen, dropped events, snapshot resends, active/total sessions, and disconnect counts so dashboards and the SPA’s stream-health panel can spot backpressure without scraping logs.
- `/inspector/metrics/search` provides both GET + POST surfaces (mirrored under `/inspector/metrics/search/*` inside the inspected PathSpace). POST `{ "query": { "latency_ms", "match_count", "returned_count" }, "watch": { "live", "missing", "truncated", "out_of_scope", "unknown" } }` to record a search/watch evaluation; GET to retrieve totals, truncated node counts, and last/average latency for dashboards. The SPA reports automatically after each search/watch run and polls every 5 s to light up badges whenever truncation or ACL hits spike.

#### Tree virtualization & lazy detail panes

- The SPA tree now renders through a virtualized list (28 px rows with overscan) that reuses the flattened snapshot cache. SSE deltas apply incremental updates to that cache, so the inspector only re-renders the rows that are currently visible instead of rebuilding thousands of DOM nodes per poll interval. Scrolling stays responsive (~100 k nodes on mid-range laptops) as long as the viewport itself remains in view.
- Node selection still performs a `/inspector/node` fetch, but highlights move instantly as deltas land because the flattened tree shares the same path map as the EventSource handler. If a delta removes the currently viewed root, the viewport clears immediately and `refreshTree()` repopulates it when the server publishes the next snapshot.
- Watchlists, snapshots, write toggles, and the paint card now hydrate lazily via `IntersectionObserver`. Those panels fetch their data the moment they scroll into view or when the operator presses their refresh/save buttons. That keeps idle tabs cheap without removing functionality—manual refresh always forces the panel active, and dashboards no longer need to poll these endpoints until someone is actually looking at them.

### 8.4 Watchlists, bookmarks, and imports

- Saved watchlists persist per authenticated user under `/inspector/user/<id>/watchlists/*` (soft-deleted entries move to `/inspector/user/<id>/watchlists_trash/*` so ops can audit removals). The backend exposes:
  - `GET /inspector/watchlists` — returns `{ user, user_id, count, limits, watchlists: [...] }` where each watchlist mirrors the stored JSON.
  - `POST /inspector/watchlists` — create/update a set. Pass `{ "name": "Critical nodes", "paths": ["/app/state/foo"], "id": "optional-slug", "overwrite": true }` to overwrite an existing record.
  - `DELETE /inspector/watchlists?id=<id>` — delete a saved set (the server relocates it under the trash path).
  - `GET /inspector/watchlists/export` / `POST /inspector/watchlists/import` — export/import JSON payloads so bug repros and dashboards can share curated watchlists. Import requests accept `{ "mode": "replace" | "merge", "watchlists": [ ... ] }` bodies.
- `InspectorHttpServer::Options::watchlists` defines per-user limits (`max_saved_sets`, `max_paths_per_set`). Defaults: 32 saved sets, 256 paths per set. Override them only when you have a clear operational need, document the new values, and keep them in sync with service README files.
- `POST /inspector/metrics/usage` accepts `{ "timestamp_ms": <optional>, "panels": [{ "id": "tree", "dwell_ms": 4200, "entries": 1 }] }` from the SPA’s panel tracker; the server sanitizes the IDs, pushes totals into `/diagnostics/web/inspector/usage/*`, and acknowledges with `202`. Use it when embedding the SPA elsewhere or when synthetic clients want to describe scripted sessions.
- `GET /inspector/metrics/usage` mirrors the published totals so dashboards and operators do not have to poll PathSpace directly. Expect `total.{dwell_ms,entries,last_updated_ms}` plus one entry per panel (same schema as the POST payload). The underlying PathSpace nodes live at `/diagnostics/web/inspector/usage/total/*` and `/diagnostics/web/inspector/usage/panels/<id>/*` (dwell, entries, last sample, last update) if you want to wire Grafana or offline log processors.
- The SPA’s watchlist panel now includes save/save-as/load/delete/export/import controls. All buttons hit the endpoints above, so manual API clients and the UI always reflect the same state.

### 8.5 Snapshots, exports, and diffs

- `POST /inspector/snapshots` captures the current tree bounds using `InspectorSnapshotOptions` plus a `PathSpaceJsonExporter` dump. Pass `{ "label": "Search baseline", "note": "before rollback", "options": { "root": "/app", "max_depth": 3, "max_children": 64, "include_values": true } }` to label the capture; the server stores both the inspector snapshot (for SSE-style diffs) and the exporter JSON under `/inspector/user/<id>/snapshots/<slug>/...` and trims the oldest entries when you exceed `InspectorHttpServer::Options::snapshots.max_saved_snapshots` (default 20).
- `GET /inspector/snapshots` returns `{ user, user_id, count, limit, max_snapshot_bytes, snapshots: [...] }`. Each entry mirrors the stored metadata (created timestamp, label, traversal bounds, byte counts) so automation can list available attachments before requesting the heavy payloads.
- `GET /inspector/snapshots/export?id=<id>` streams the `PathSpaceJsonExporter` payload augmented with the capture `options` block (root/depth/children/values) and a `Content-Disposition` filename. Drop the body directly into bug reports or archive directories—no need to shell into the host and run `pathspace_dump_json` manually.
- `POST /inspector/snapshots/diff` accepts `{ "before": "snapshot-a", "after": "snapshot-b" }` and replies with the Inspector delta schema (`changes.added/updated/removed`, diagnostics, and options). Because it reuses `BuildInspectorStreamDelta`, the JSON matches what `/inspector/stream` would have emitted live.
- The SPA’s “Snapshots” panel wraps all of the above: capture form (root/depth/children default to the current view), saved snapshot select, download/delete buttons, and a diff viewer that renders the `/inspector/snapshots/diff` payload. Operators can now capture evidence, compare revisions, and attach JSON bundles without leaving the browser.

### 8.6 Troubleshooting & rollout notes

- Call `InspectorHttpServer::is_running()` (or watch your service health checks) before advertising the inspector endpoint.
- Scope inspector access behind your existing auth stack; only “root” roles should observe `/`.
- Log the bound port + root at startup so operators can connect without reading code. The example above uses `std::printf`, but production builds should wire into the existing logging pipeline.
- When shipping custom UI bundles, put them under version control beside your service and pass the directory via `ui_root`. The server falls back to the embedded SPA whenever files are missing, so mismatched asset paths fail safe.
- Keep your `docs/finished/Plan_PathSpace_Inspector_Finished.md` reference in sync whenever defaults change; downstream teams now rely on this playbook plus the plan’s embedding section as the single source of truth.

### 8.7 Remote mounts & `/remote/<alias>` trees

- Phase 2 of the inspector plan introduces `InspectorHttpServer::Options::remote_mounts`, a vector of remote endpoints the server polls in the background. Each entry requires:

  ```cpp
  SP::Inspector::RemoteMountOptions remote;
  remote.alias                = "alpha";       // shows up under /remote/alpha
  remote.host                 = "10.0.0.42";
  remote.port                 = 8765;           // remote inspector host/port
  remote.root                 = "/app";        // subtree exported by the remote app
  remote.snapshot.max_depth   = 3;              // caps for the remote fetcher
  remote.snapshot.max_children = 64;
  remote.snapshot.include_values = true;
  remote.access_hint         = "Requires corp VPN"; // shown in UI hover + API
  options.remote_mounts.push_back(remote);
  ```

- The embedded `RemoteMountManager` opens one HTTP client per alias, fetches `/inspector/tree?root=<remote.root>` on the configured interval, and rewrites every path under `/remote/<alias>`. The local `/inspector/tree` and `/inspector/node` responses now include a synthetic `/remote` node with children for each mount so dashboards immediately see which aliases are live/offline.
- `/inspector/stream` automatically multiplexes the cached remote snapshots into the session’s snapshot/delta sequence, so consumers receive local + remote changes via a single EventSource connection (no per-client remote SSE fans). Remote fetches happen on background threads, so slow/offline mounts can’t stall local polling.
- Offline mounts publish placeholders plus status strings (last error, timeout, etc.) in both the tree diagnostics and the SPA’s detail panes; once a mount reconnects the placeholder is replaced by the live subtree.
- `GET /inspector/remotes` now exposes the configured aliases with their `/remote/<alias>` path, connection status, last update timestamp, and the optional `access_hint`. The SPA, dashboards, and automation can poll this endpoint without taking a full tree snapshot when they only care about mount health.
- Remote diagnostics now mirror into `/inspector/metrics/remotes/<alias>/{status,latency,requests,waiters,timestamps,meta}`: the server records last/avg/max latency (in ms), total successes/errors, consecutive failure streaks, waiter depth/max waiter depth (covers both `/inspector/tree` remote roots and SSE sessions rooted at `/remote/<alias>`), plus last-update/error timestamps and the configured `access_hint`. `/inspector/remotes` exposes the same data via nested `latency`, `requests`, `waiters`, and `health` objects, so dashboards and the SPA can chart slow/offline mounts without scraping logs.
- The SPA’s quick-root dropdown consumes `/inspector/remotes`, persists the selected root via the page URL + `localStorage`, and shows per-alias status pills with hover hints (VPN/auth scope, last error). Clicking a badge switches the inspector to that mount immediately, keeping remote workflows fast for operators.
- Document your aliases + hostnames in service READMEs so operators know which remote roots are expected; the quick-root dropdown renders whatever `/inspector/remotes` advertises, so stale config shows up immediately.

### 8.6 ACL gating & diagnostics

- `InspectorHttpServer::Options::acl` scopes every `/inspector/tree`, `/inspector/node`, and `/inspector/stream` request before the snapshot builder runs. Set `default_role` to the role you expect when no header is present (defaults to `root`), override `role_header` / `user_header` if your auth proxy writes different header names, and push one `InspectorAclRuleConfig` per role. Rules have two knobs: `allow_all` (grant the role full access) and `roots` (normalized path prefixes such as `/app` or `/remote/alpha`).
- Example configuration:

  ```cpp
  options.acl.default_role = "user";
  options.acl.role_header  = "x-pathspace-role";
  options.acl.user_header  = "x-pathspace-user";

  SP::Inspector::InspectorAclRuleConfig admin;
  admin.role      = "root";
  admin.allow_all = true;
  options.acl.rules.push_back(admin);

  SP::Inspector::InspectorAclRuleConfig scoped;
  scoped.role  = "user";
  scoped.roots = {"/app", "/remote/beta"};
  options.acl.rules.push_back(scoped);
  ```

- The backend normalizes requested paths (collapsing duplicate slashes, trimming trailing `/`) before evaluating the rule, so `/app///foo` can’t escape `/app`. Unauthorized attempts return `403` with a structured payload: `{ "error": "inspector_acl_denied", "message": "…", "role": "user", "requested_path": "/system", "allowed_roots": ["/app"] }`.
- The SPA consumes those payloads automatically—root badges turn red, the tree panel shows the message, and EventSource retries are paused until the operator selects an allowed root. Manual refresh keeps working for administrators, and watch/search forms still submit metrics so dashboards can see when ACL pressure spikes.
- Violations are logged inside the inspected PathSpace: `/diagnostics/web/inspector/acl/violations/total`, `/diagnostics/web/inspector/acl/violations/last/{timestamp_ms,role,user,endpoint,requested_path,reason}`, plus append-only JSON blobs under `/diagnostics/web/inspector/acl/violations/events/<timestamp>`. Feed those nodes into Grafana or Grafite to audit who attempted to leave their scope without tailing HTTP logs.
- Keep the `root` role reserved for break-glass sessions and wire your auth gateway so scoped users receive headers (`x-pathspace-role`, `x-pathspace-user` by default) that match the rules you install. When service teams need temporary wider access, add a new rule anchored to the relevant subtree instead of relaxing `/` globally.

### 8.7 Accessibility & Audits
- The inspector SPA now exposes explicit ARIA roles/labels for the tree, watchlist, snapshots, paint card, and remote badges. Arrow keys/Home/End/Page Up/Page Down move the virtualized tree selection, while Space/Enter re-fetch the focused node. Remote badges advertise themselves as buttons, so keyboard operators can switch mounts without the mouse.
- All interactive controls share a high-contrast focus ring, so color-only cues are no longer required during triage. When embedding the SPA via `ui_root`, keep the new CSS block intact so the ring colors remain consistent with the bundled assets.
- Run the axe-core regression any time you touch the SPA markup: `INSPECTOR_TEST_BASE_URL=http://127.0.0.1:8765 \
  (cd tests/inspector/playwright && npx playwright test tests/inspector/playwright/tests/inspector-sse.spec.js --grep "axe-core WCAG")`. The helper wraps `@axe-core/playwright` and fails on WCAG 2.0/2.1 A/AA violations.

### 8.8 Admin write toggles & audit trail

- Use `InspectorHttpServer::Options::write_toggles` to whitelist the handful of flag flips/resets you are comfortable exposing through the inspector. Each entry includes:
  - `id`/`label`/`description` for the UI.
  - `path` (normalized via `NormalizeInspectorPath`) and `kind` (`ToggleBool` or `SetBool`).
  - `default_state` which acts as both the fallback value (when the path is absent) and the forced value for `SetBool` actions (e.g., “Reset flag to disabled”).
- Requests flow through `GET/POST /inspector/actions/toggles`:
  - The GET endpoint lists configured actions, the current boolean value at each path, whether confirmation headers are required, and the allowed roles.
  - The POST endpoint executes the action, but only when the caller’s role matches `write_toggles.allowed_roles` (defaults to `root`) **and** the confirmation header matches `write_toggles.confirmation_header` / `confirmation_token` (defaults to `x-pathspace-inspector-write-confirmed: true`). Missing headers return `428 Precondition Required` so automated clients cannot flip flags silently.
- The SPA now exposes an “Enable session writes” gate above the write-toggle panel. Operators must type `ENABLE` (or whatever you instruct them to use) before the buttons are active, and the panel surfaces the same allowed-role/confirmation hints returned by the backend. When the gate is disabled, the buttons are inert and the warning text reminds users that the session is read-only.
- Every attempt is logged inside the inspected PathSpace under `/diagnostics/web/inspector/audit_log/{total,last/*,events/*}`:
  - `total` increments monotonically.
  - `last/*` captures timestamp, action id/label, role, user header, client address, previous/new values, and outcome (`success`/`failure`).
  - `events/<timestamp>-<slug>` stores the full JSON blob (including any operator-provided note) so dashboards can replay write history or diff behavior between services without tailing HTTP logs.
- Treat the write-toggles feature as an escape hatch, not a general-purpose mutator. Keep the allowlist small, document each action in your service README, and rotate the confirmation header/token if you suspect a compromised admin session. For anything beyond boolean flips or deterministic resets, continue to land code changes or CLI workflows instead of widening inspector access.
