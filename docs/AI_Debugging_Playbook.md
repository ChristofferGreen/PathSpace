# Handoff Notice

> **Handoff note (October 19, 2025):** These troubleshooting steps remain accurate but assume the prior tooling defaults. Confirm any helper script changes in `docs/AI_Onboarding_Next.md` before relying on this playbook.
> **Update (October 21, 2025):** Presenter telemetry now mirrors into window diagnostics sinks (`windows/<win>/diagnostics/metrics/live/views/<view>/present/*`); see §3 for usage.

# PathSpace — Debugging Playbook

> **Context update (October 18, 2025):** The test harness now captures detailed logs for every failure via `scripts/run-test-with-logs.sh`. Use this playbook to triage regressions, inspect diagnostics, and close the loop with the mandated 15× test runs.
> **Diagnostics refresh (October 19, 2025):** Dirty-rect hints are coalesced server-side, and the extended damage/fingerprint/progressive metrics are available when `PATHSPACE_UI_DAMAGE_METRICS=1`. Keep the flag unset during perf runs—the software encode pass is still single-threaded today, so we’re focusing on parallelising it next.

This guide consolidates the practical steps for investigating failures across unit tests, UI targets, and runtime metrics. Pair it with the architecture docs (`docs/AI_Architecture.md`, `docs/Plan_SceneGraph_Renderer.md`) when you need deeper background.

## 1. Reproducing and Capturing Failures

### 1.1 Standard looped test run

```bash
./scripts/compile.sh --test --loop=15 --per-test-timeout=20
```

- Runs both `PathSpaceTests` and `PathSpaceUITests` each iteration.
- Logs land under `build/test-logs/` using the pattern `<test>_loop<iteration>of<total>_<timestamp>.log` if a failure occurs.
- `PATHSPACE_LOG` defaults to `1` in the helper so tagged logging is enabled when an error surfaces; adjust via `--env PATHSPACE_LOG=0` if you need silence.
- Want to exercise the Metal presenter path locally? Append `--enable-metal-tests` (macOS only) so the helper sets `PATHSPACE_UI_METAL=ON` during configuration and runs the suites with `PATHSPACE_ENABLE_METAL_UPLOADS=1`.
- Watching the IO Pump? Check `/system/widgets/runtime/input/metrics/{pointer_events_total,button_events_total,text_events_total,events_dropped_total,last_pump_ns}` plus `/system/widgets/runtime/io/state/running` to confirm the worker is alive and consuming Trellis events. Pump errors share the same `/system/widgets/runtime/input/log/errors/queue` as reducer failures, so grep for `io_pump` tags when diagnosing drops.

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

1. Open the saved log file (e.g., `build/test-logs/PathSpaceTests_loop3of15_20251018-161200.log`).

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
- **Scene dirty state:** `scenes/<sid>/diagnostics/dirty/state` and `scenes/<sid>/diagnostics/dirty/queue` expose layout/build notifications. The `Scene::MarkDirty` doctests show how to wait on these paths without polling.
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
   - Use `--loop=3` on the helper to confirm the fix eliminates intermittent races before scaling back to the mandated 15.
4. **Document findings**
   - Update the relevant plan doc (`docs/Plan_SceneGraph.md` or task entry) with repro steps, log references, and next actions.

## 5. Tooling Quick Reference

| Task | Command |
| --- | --- |
| Run both suites once with logs | `./scripts/compile.sh --test` |
| Run both suites 15× with 20 s timeout | `./scripts/compile.sh --test --loop=15 --per-test-timeout=20` |
| Run suites once with Metal presenter enabled (macOS) | `./scripts/compile.sh --enable-metal-tests --test` |
| Run a single suite via CTest | `ctest --output-on-failure -R PathSpaceUITests` |
| Re-run failed tests only | `ctest --rerun-failed --output-on-failure` |
| Verify HTML adapter command stream | `ctest -R HtmlCanvasVerify --output-on-failure` |
| Verify HSAT asset inspection tooling | `ctest -R HtmlAssetInspect --output-on-failure` |
| Inspect HSAT payload contents manually | `./build/pathspace_hsat_inspect --input <payload.hsat>` |
| Inspect Undo history on disk | `./build/pathspace_history_inspect <history_dir>` |
| Export/import undo savefiles | `./build/pathspace_history_savefile <export|import> --root <path> --history-dir <dir> --out/--in <bundle>.history.journal.v1` |
| Smoke-test savefile CLI roundtrip | `./build/pathspace_history_cli_roundtrip` |
| Run CLI roundtrip regression | `ctest -R HistorySavefileCLIRoundTrip --output-on-failure` |
| Tail latest failure log | `ls -t build/test-logs | head -1 | xargs -I{} tail -n 80 build/test-logs/{}` |
| Inspect renderer metrics path | `build/tests/PathSpaceUITests --test-case Diagnostics::ReadTargetMetrics` |
| Benchmark damage/fingerprint metrics | `./build/benchmarks/path_renderer2d_benchmark --metrics [--canvas=WIDTHxHEIGHT]` |
| Capture renderer FPS traces | `./scripts/capture_renderer_fps_traces.py --pretty` |
| Capture pixel-noise baseline JSON | `./scripts/capture_pixel_noise_baseline.sh` |
| Check pixel-noise run against budgets | `python3 scripts/check_pixel_noise_baseline.py --build-dir build` |
| Capture pixel-noise PNG frame | `./build/pixel_noise_example --headless --frames=1 --write-frame=docs/images/perf/pixel_noise.png` |
| Guardrail demo binary sizes | `./scripts/compile.sh --size-report` |

- `./scripts/compile.sh --test` now runs the PixelNoise perf harness (software and, when enabled, Metal) alongside the core and UI test executables. The mandated 15× loop therefore enforces the perf budgets without a separate CTest call—refresh `docs/perf/pixel_noise_baseline.json` and `docs/perf/pixel_noise_metal_baseline.json` whenever thresholds legitimately move.

### 5.1 Pixel Noise Perf Harness Baselines

- Use `./scripts/capture_pixel_noise_baseline.sh` after rebuilding (`cmake --build build -j`) to refresh `docs/perf/pixel_noise_baseline.json`.
- Pass `--backend=metal` (and export `PATHSPACE_ENABLE_METAL_UPLOADS=1`) to capture the Metal2D variant in `docs/perf/pixel_noise_metal_baseline.json`; commit both baselines together when budgets shift.
- The helper launches `pixel_noise_example` headless with the standard perf budgets (≥25 FPS, ≤20 ms render/present, ≤40 ms present-call post shaped-text rollout) and records the run via `--write-baseline=<path>`.
- `python3 scripts/check_pixel_noise_baseline.py --build-dir build` reruns the harness with the recorded parameters, writes a temporary metrics snapshot, and fails if the averaged frame times exceed the stored budgets or if FPS dips below the baseline threshold. The script respects the baseline’s `backendKind`, forwarding the matching `--backend` flag and enabling Metal uploads automatically when needed. `PixelNoisePerfHarness` (software) and `PixelNoisePerfHarnessMetal` (Metal, PATHSPACE_UI_METAL builds) in CTest now go through the same script so regressions surface during the 15× loop.
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

- Run `./scripts/perf_guardrail.py --build-dir build --build-type Release --jobs <n>` to execute the PathRenderer2D benchmark and the pixel noise example, compare their metrics against `docs/perf/performance_baseline.json`, and fail if regressions exceed per-metric tolerances.
- Append `--history-dir build/perf/history --print` when iterating locally; the helper writes a JSONL snapshot per scenario so you can trend metrics after each change.
- Refresh the baseline only after intentional performance wins: `./scripts/perf_guardrail.py --build-dir build --build-type Release --jobs <n> --write-baseline`. Commit the updated JSON alongside the change and note the justification in the PR description.
- The guardrail runs automatically from `scripts/compile.sh --perf-report` and the local pre-push hook; export `SKIP_PERF_GUARDRAIL=1` sparingly (e.g., when profiling on unsupported hardware) and document the reason in the PR.
- Inspect `docs/perf/performance_baseline.json` for the tracked metrics. Key checks today:
  - `path_renderer2d` scenario covers full-repaint vs incremental workloads (avg ms, FPS, damage ratios, tile counts).
  - `pixel_noise_software` scenario validates presenter timings (average FPS, render/present/present-call ms, bytes copied per frame).
- Keep the baseline and tolerances in sync with `docs/perf/README.md` when new scenarios are added.

### 5.4 Undo history inspection

- Build the CLI with `cmake --build build -j` and run `./build/pathspace_history_inspect <history_dir>` to audit persisted undo stacks. The tool now emits a lightweight journal summary (entry count, inserts, takes, barrier markers). Snapshot decoding/diffs have been removed along with the snapshot backend; pull structured telemetry from `_history/stats/*` at runtime if deeper analysis is required.
- Add `--dump <generation>` to traverse a snapshot and preview payload bytes; `--preview-bytes` tunes the hex sampler and `--no-analyze` skips snapshot decoding when only file coverage matters.
- Point the CLI at `${PATHSPACE_HISTORY_ROOT:-$TMPDIR/pathspace_history}/<space_uuid>/<encoded_root>` when reproducing bugs; pair the findings with the `_history/stats/*` inspector nodes referenced in `docs/finished/Plan_PathSpace_UndoHistory_Finished.md`.
- Use `./build/pathspace_history_savefile export --root /doc --history-dir $PATHSPACE_HISTORY_ROOT/<space_uuid>/<encoded_root> --out doc.history.journal.v1` to author journal savefiles (`history.journal.v1`) directly from persisted history directories; the CLI derives the persistence namespace automatically and fsyncs by default. Pair with `import --root /doc --history-dir … --in doc.history.journal.v1` when seeding a fresh subtree before reproducing a bug—append `--no-apply-options` if you need to preserve local retention knobs.
- The CLI works alongside the programmatic helpers (`UndoableSpace::exportHistorySavefile` / `importHistorySavefile`) so integration tests can still call straight into C++, but editor/recovery scripts should prefer the CLI to avoid bespoke harnesses. Update postmortem docs with the exact command + PSJL bundle whenever you capture undo history for analysis.
- `./build/pathspace_history_cli_roundtrip` exercises both CLI commands against a temporary persistence tree, re-exports the result, and diffs the summaries (values, undo/redo counts). The harness now also writes `history_cli_roundtrip/telemetry.json` (bundle hashes, entry/byte counts) plus `original.history.journal.v1` / `roundtrip.history.journal.v1` into the active test artifact directory, so dashboards/inspector tooling can scrape the data automatically. Pre-push runs pick up the same artifacts under `build/test-logs/history_cli_roundtrip/` with a timestamped subdirectory.
- The dedicated regression lives in CTest as `HistorySavefileCLIRoundTrip`; run it (or the pre-push hook) whenever the savefile codec or CLI surface changes to ensure PSJL bundles continue to round-trip end-to-end.
- Override the artifact destination with `PATHSPACE_CLI_ROUNDTRIP_ARCHIVE_DIR=/path/to/dir ./build/pathspace_history_cli_roundtrip` when scripting ad-hoc captures. Test loops default to `PATHSPACE_TEST_ARTIFACT_DIR` (exported by `scripts/run-test-with-logs.sh`), so every failure leaves behind telemetry + PSJL pairs alongside other logs.
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

1. Re-running the full loop: `./scripts/compile.sh --test --loop=15 --per-test-timeout=20`
2. Verifying no new logs were emitted (`find build/test-logs -type f -mmin -5` should be empty on success).
3. Recording the outcome (pass/fail, relevant paths, log snippets) in the working note or PR description for traceability.

With the shared test runner and this playbook, every failure leaves behind actionable artifacts, keeping the UI/renderer hardening efforts measurable and repeatable.

## 7. Current Blockers (October 20, 2025)

- None. The HTML asset hydration failure was cleared on October 20, 2025 by introducing a dedicated `Html::Asset` serialization codec (`include/pathspace/ui/HtmlSerialization.hpp`) and a regression test (`Html::Asset vectors survive PathSpace round-trip`). Reconfirm with `ctest --test-dir build --output-on-failure -R "Html::Asset"` or the full `PathSpaceUITests` loop when touching the renderer stack.
