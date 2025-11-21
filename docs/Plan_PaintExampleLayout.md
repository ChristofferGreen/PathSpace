# Plan: Paint Example Layout & Screenshot Loop

Status Legend — ✅ Done · 🚧 In Progress · ⏳ Planned

## Motivation
- Maintainer feedback shows the declarative paint example still renders all controls inside a single pale column with no actual widgets visible; manual macOS screenshots confirm the column is empty and the status label is clipped.
- The automated `PaintExampleScreenshot` regression test depends on a deterministic baseline PNG, so each UI adjustment (layout, padding, palette rows, etc.) must stay in lockstep with the captured artifact to avoid CI noise.
- The current “fix it in code, capture a screenshot, hope for parity” workflow is cumbersome; we need a repeatable improvement loop with clear ownership of the code paths involved (`examples/paint_example.cpp`, declarative stack helpers, screenshot tooling, docs assets).

## Current State
1. **Layout scaffolding** – `examples/paint_example.cpp` mounts a root horizontal stack with a controls column (`controls_panel`) and a paint canvas (`canvas_panel`). The controls fragment prepares nested stacks for status/brush labels, slider, palette grid, and undo/redo row.
2. **Runtime gaps** – Nested stack fragments rely on `Stack::Fragment`’s finalize pass to rebuild layout metadata, but the child widgets currently do not appear because their parent stack is not populated with actual widgets (status text, slider, palette buttons) after mounting.
3. **Screenshot pipeline** – `scripts/compile.sh --clean --test --loop=15 --release` runs `PaintExampleScreenshot`, comparing against `docs/images/paint_example_baseline.png`. The baseline now reflects the new layout attempt, but it still captures an empty controls column, so future improvements will require updating this same asset.
4. **Assets** – We have three PNGs checked in for context:
   - `docs/images/paint_example_before.png` – legacy overlapping widgets.
   - `docs/images/paint_example_after.png` – current automated capture (controls column + canvas).
   - `docs/images/paint_example_baseline.png` – screenshot test baseline (currently identical to `after`).
   - `docs/images/paint_example_720_baseline.png` – Metal capture at 1280×720 used by the low-height regression test.
   - `docs/images/paint_example_600_reference.png` – 1024×600 capture kept as a manual reference until we add another automated check.

## Outstanding Issues
- *(none — inspector telemetry now flows end-to-end; continue tracking remaining work under Next Steps.)*

## Progress — November 20, 2025
- ✅ Controls column now mounts the status label, brush label, slider, palette grid, and undo/redo row as independent stack panels. Each nested stack sets an `active_panel`, preventing invisible children.
- ✅ `PaintLayoutMetrics` adds dedicated padding + palette button height so the status label no longer hugs the window border and palette rows stay evenly sized.
- ✅ `wait_for_stack_children` plus `PAINT_EXAMPLE_DEBUG_LAYOUT=1` log the missing panel IDs whenever the controls stack fails to populate, making screenshot flakes easier to diagnose.
- ✅ Refreshed `docs/images/paint_example_after.png` and `docs/images/paint_example_baseline.png` (captured November 20, 2025 after the controls fix). This baseline is sourced from a Metal capture (`PATHSPACE_ENABLE_METAL_UPLOADS=1`, `PATHSPACE_UI_METAL=ON`, `--gpu-smoke --screenshot-require-present`) so CI must keep Metal uploads enabled or fall back to re-shooting before running the comparison.
- ✅ Verified `scripts/check_paint_screenshot.py --build-dir build` succeeds when the Metal baseline is in place; failure was previously caused by diffing a GPU capture against the deprecated software-render baseline.
- ✅ `Scene lifecycle publishes scene snapshots and tracks metrics` now watches `/runtime/lifecycle/metrics/last_revision` instead of counting `/builds/*` nodes (which GC trims immediately). The test no longer races the retention policy, and `./scripts/compile.sh --clean --test --loop=15 --release` passed 15/15 loops on November 21, 2025 (`PathSpaceUITests_loop15of15_20251121-113607.log`, manifest archived beside it).

## Progress — November 21, 2025
- ✅ Added a `controls_scale` metric so typography, slider height, palette button height, and stack spacing shrink when the window height drops below 800 px. Captured an extra 1280×720 GPU screenshot (`build/paint_example_after_720.png`) to confirm the status/brush labels no longer clip at the bottom of the column.
- ✅ Instrumented `examples/paint_example.cpp` to emit `widgets/<id>/metrics/history_binding/*` (state + timestamp, buttons_enabled flag, undo/redo success/failure counters, last error code/context/message). The metrics root initializes before the paint surface mounts, flips to `state="ready"` after `UndoableSpace::enableHistory`, and records every undo/redo invocation or missing-binding press so the inspector and screenshot harnesses can flag silent regressions without scraping stdout.
- ✅ `wait_for_stack_children` now also blocks on the nested `actions` stack, undo/redo buttons mount disabled by default, and `set_history_buttons_enabled` flips them on only after the `UndoableSpace` binding resolves. Missing bindings log the specific button ID so we can track telemetry gaps.
- ✅ UI GPU tests (`PaintExampleScreenshot*`, `PixelNoisePerfHarness*`) now share a `ui_gpu_capture` CTest `RESOURCE_LOCK`, and `scripts/check_paint_screenshot.py` retries one extra capture when Metal returns a stale framebuffer. This eliminates the 0.035 mean-error flakes we kept seeing in the 15× loop without lowering the comparison threshold.
- ✅ Re-ran `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON PAINT_EXAMPLE_DEBUG_LAYOUT=1 ./build/paint_example --width=1280 --height=800 --gpu-smoke --screenshot docs/images/paint_example_after.png --screenshot-compare docs/images/paint_example_baseline.png --screenshot-require-present` and `scripts/check_paint_screenshot.py --build-dir build`; both reported `mean error 0`, so the baseline stays untouched.
- ✅ Palette swatches now come from declarative theme tokens (`palette/swatches/{red,orange,yellow,green,blue,purple}`) so accessibility tweaks happen in one place. The paint example falls back to the default swatch values, but custom themes (or `Theme::SetColor`) can recolor the palette without touching `examples/paint_example.cpp`.
- ✅ History bindings publish an inspector-friendly card at `widgets/<id>/metrics/history_binding/card` (serialized `HistoryBindingTelemetryCard`). The paint example keeps the card in sync with state changes, button enables, undo/redo counters, and last error metadata so the upcoming inspector can surface undo readiness without replaying screenshots.
- ✅ Captured a Metal baseline at 1280×720 (`docs/images/paint_example_720_baseline.png`, mean error 0.00127974 vs live capture) and a supplementary 1024×600 reference (`docs/images/paint_example_600_reference.png`). These runs used `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON --gpu-smoke --screenshot-require-present`.
- ✅ Extended `scripts/check_paint_screenshot.py` with `--tag` so concurrent captures don’t clobber artifacts, added a `PaintExampleScreenshot720` CTest entry, and wired the compile-loop harness to run both screenshot profiles each iteration. The low-height baseline enforces the ≤720 px layout requirement called out in this plan so we notice regressions before captures ship.
- ✅ Palette buttons now respect the active declarative theme for legibility. The new `palette/text_on_light` and `palette/text_on_dark` tokens default to the previous heuristics, but painters can override them via `Theme::SetColor` without re-recording screenshots.
- ✅ `scripts/run-test-with-logs.sh` gained `--keep-success-log` and writes a manifest line for every saved log/artifact pair. `./scripts/compile.sh` opts PathSpaceUITests into this mode automatically when `--loop` is used, writes `build/test-logs/loop_manifest.tsv`, and prints the manifest path after the loop completes.
- ✅ Added `--loop-keep-logs`, `--loop-label`, and `--ui-test-extra-args` flags (plus env overrides `PATHSPACE_LOOP_KEEP_LOGS`, `PATHSPACE_LOOP_LABEL_FILTER`, `PATHSPACE_UI_TEST_EXTRA_ARGS`) so we can: (a) keep success logs for additional labels, (b) loop only the flaky executable, and (c) append doctest options like `--success` without touching other binaries. The defaults keep PathSpaceUITests logs whenever `--loop` is active.
- ✅ `--loop-label` validation now happens after the test commands register, so the filter reliably matches `PathSpaceUITests` without tripping a false “no tests configured” error. `scripts/run-test-with-logs.sh` also appends an `[test-runner] EXIT …` banner (exit code, signal, or timeout plus UTC timestamp) to every saved log, which turned the latest loop failure into a precise repro (timeout inside `Scene lifecycle publishes scene snapshots and tracks metrics`).
- ✅ `scripts/compile.sh` now archives failing `PathSpaceUITests` loop iterations under `test-logs/loop_failures/<timestamp>_<label>_loopN/`, copying the loop manifest, the offending log, and the associated artifact directory plus a summary file (label, iteration, exit code, reason). This satisfies the “capture manifest/log pair before re-running” reminder: rerunning the suite no longer clobbers the evidence we need for regressions.
- ✅ Introduced `docs/images/paint_example_baselines.json` plus `scripts/paint_example_capture.py`; the manifest records width/height, renderer mode, capture timestamp, commit hash, and SHA256 for the 1280×800 + 1280×720 PNGs. `scripts/check_paint_screenshot.py` now validates those hashes/dimensions, exports `PAINT_EXAMPLE_BASELINE_*` env vars, and paint_example refuses to run when the manifest revision drops below `kRequiredBaselineManifestRevision` (currently `1`).
- ✅ Inspector surfacing landed: `examples/paint_example.cpp` publishes the baseline manifest (`width`, `height`, `renderer`, `captured_at`, `commit`, `notes`, `sha256`, `tolerance`) plus `last_run/*` stats (status, timestamp, hardware vs software capture, mean error, diff artifact) under `diagnostics/ui/paint_example/screenshot_baseline`, writes the same payload to disk via `--screenshot-metrics-json`, and `scripts/check_paint_screenshot.py` now sets the matching `PAINT_EXAMPLE_BASELINE_*` env vars automatically. `scripts/paint_example_diagnostics_ingest.py` (covered by `tests/tools/test_paint_example_diagnostics_ingest.py`) ingests the per-run JSON files into dashboard/inspector-ready summaries so regressions show up before anyone eyeballs the PNG diff.

## Work Breakdown
1. 🚧 **Controls Column MVP**
   - Mount status & brush labels with typography + padding.
   - Mount slider with width tied to `controls_width`.
   - Build palette grid via nested horizontal stacks; ensure each button fragment inherits consistent spacing.
   - Mount undo/redo row and confirm history binding availability.
2. 🚧 **Layout Metrics & Styling**
   - Tune `PaintLayoutMetrics` padding/spacing values for 1280×800 default while scaling down gracefully (≥800×600).
   - Add optional theme overrides (colors, corner radii) so controls visually contrast against the dark background.
3. 🚧 **Screenshot Discipline**
   - Document the exact capture commands (`./build/paint_example --screenshot …`, `scripts/compile.sh …`) and store them near the plan (see Checklist below).
   - Record baseline SHA in this doc whenever we update `docs/images/paint_example_baseline.png`.
4. 🚧 **Validation Hooks**
   - `PAINT_EXAMPLE_DEBUG_LAYOUT=1` now prints the controls stack contents while `wait_for_stack_children` polls; expand it into a full overlay once we have bandwidth.
   - Extend `scripts/check_paint_screenshot.py` (already present) to fail fast when mean error exceeds threshold and link to remediation steps.

## Checklist for Each Iteration
1. 🚧 Implement UI/layout change in `examples/paint_example.cpp`.
2. 🚧 `cmake --build build -j` to refresh binaries.
3. 🚧 `./build/paint_example --screenshot docs/images/paint_example_after.png --width=1280 --height=800` for visual inspection (set `PAINT_EXAMPLE_DEBUG_LAYOUT=1` when controls look empty). Use `--gpu-smoke --screenshot-require-present --screenshot-compare docs/images/paint_example_baseline.png` whenever the baseline needs to stay Metal-accurate, and repeat with `--height=720 --screenshot docs/images/paint_example_720_baseline.png` after UI changes to keep the low-height asset current. When the baseline must change, prefer `scripts/paint_example_capture.py --tags 1280x800 paint_720` so the manifest revisions, SHA256 hashes, timestamps, and commits stay in sync.
4. 🚧 After accepting a baseline change, run `scripts/check_paint_screenshot.py --tag paint_720 --height 720 --baseline docs/images/paint_example_720_baseline.png --manifest docs/images/paint_example_baselines.json --metrics-output build/artifacts/paint_example/paint_720_metrics.json` (plus the 1280×800 variant) to confirm mean error ≤ 0.0015, commit the refreshed PNGs + manifest, ingest the resulting metrics via `scripts/paint_example_diagnostics_ingest.py --inputs build/artifacts/paint_example/*_metrics.json --output-json build/test-logs/paint_example/diagnostics.json --output-html build/test-logs/paint_example/diagnostics.html`, and log the new manifest revision inside this plan.
5. 🚧 Run `./scripts/compile.sh --clean --test --loop=15 --release` (and bump `--per-test-timeout 40` if a new failure needs more headroom). The harness now auto-archives any failing iteration (`test-logs/loop_failures/...`); include that path in bug reports when a loop flakes so we can diff against the November 21, 2025 green baseline (`PathSpaceUITests_loop15of15_20251121-113607.log`). This loop now executes both `PaintExampleScreenshot` and `PaintExampleScreenshot720`.
6. 🚧 Update this plan + docs with any new decisions or baseline hashes (including the supplemental 1024×600 reference if it changes).

## Next Steps
1. ⏳ Build the inspector/debugging panel that consumes `diagnostics/ui/paint_example/screenshot_baseline/*` (or the aggregated JSON emitted by `scripts/paint_example_diagnostics_ingest.py`) so dashboards surface screenshot status + mean error automatically, without running the script manually.
2. ⏳ Evaluate whether we should split palette/slider/undo sections into dedicated declarative components for reuse in other samples.
3. ⏳ Decide whether to add a second low-height automation (1024×600) or treat `docs/images/paint_example_600_reference.png` as a manual audit artifact only.
4. ⏳ Align `examples/paint_example.cpp` structure and declarative syntax with the canonical example layout described in `docs/Plan_WidgetDeclarativeAPI.md` (syntax sample section) so we demonstrate the preferred builder patterns verbatim.

## Platformization Opportunities
To keep this example lean and ensure other UI samples benefit, spin the following into shared PathSpace components:
- 🚧 **Screenshot capture/diff service** – move the GPU smoke wait, `Window::Present` capture, overlay, and baseline comparison logic out of `examples/paint_example.cpp` into a reusable UI-layer helper so every sample and test can trigger deterministic screenshots without bespoke plumbing. Target API (PathSpace UI module):
  - `struct ScreenshotRequest { WindowPath window; std::string_view view; int width; int height; std::filesystem::path output_png; std::optional<std::filesystem::path> diff_png; std::optional<std::filesystem::path> baseline_png; double max_mean_error; ScreenshotOptions options; };`
  - `struct ScreenshotResult { bool captured_hardware; double mean_error; std::uint8_t max_channel_delta; std::filesystem::path artifact; std::optional<std::filesystem::path> diff_artifact; };`
  - `class ScreenshotService { static SP::Expected<ScreenshotResult> Capture(SP::PathSpace&, ScreenshotRequest const&); };` — ensures Metal-present captures when requested, handles baseline diffs, and publishes telemetry under `diagnostics/ui/screenshot/*`.
  - CLI wrapper: `pathspace_screenshot_cli --window paint_window --view main --width 1280 --height 800 --baseline docs/images/paint_example_baseline.png --diff build/artifacts/paint_example/diff.png` so automation no longer shells directly into `paint_example`.
- ⏳ **Stack readiness helpers** – promote `wait_for_stack_children` (plus the `PAINT_EXAMPLE_DEBUG_LAYOUT` logging) into a declarative runtime utility that any stack fragment can call to verify child panels and emit consistent diagnostics.
- ⏳ **History binding utilities** – provide a builder-level hook for wiring `UndoableSpace` instances (alias creation, options, enable, telemetry surfaces) instead of re-implementing the binding flow inside each example.
- 🚧 **Screenshot harness tooling** – evolve `scripts/check_paint_screenshot.py` and the CTest glue into a first-class PathSpace CLI/module, standardizing artifact paths, tags, and Metal-present enforcement so new samples inherit the discipline automatically.
  - Manifest + helper (`docs/images/paint_example_baselines.json`, `scripts/paint_example_capture.py`) now cover PaintExampleScreenshot; next step is extracting the logic into a reusable UI module.
- ⏳ **Example CLI utilities** – introduce a shared argument parser (e.g., `ExampleCli cli; cli.add("--width", ExampleCli::IntOption(1280, [&] (int value) { window_width = value; })); cli.parse(argc, argv);`) so every example can register handlers with a single call instead of re-implementing `std::from_chars` loops. Ship common bundles for screenshot/headless/GPU flags to keep per-example code minimal.

### Migration Track — Example CLI
1. ⏳ Land the shared CLI helper in `src/pathspace/examples/cli/` with coverage (unit tests + sample wiring in `examples/minimal_example.cpp`).
2. ⏳ Update `examples/paint_example.cpp`, `examples/widgets_example.cpp`, `examples/devices_example.cpp`, and the scripted screenshot harnesses to consume the helper.
3. ⏳ Roll through the remaining examples (`pathspace_history_cli_roundtrip`, `pathspace_inspect`, etc.) so every binary uses the unified parser; mark the Platformization item complete once no example relies on bespoke parsing.
