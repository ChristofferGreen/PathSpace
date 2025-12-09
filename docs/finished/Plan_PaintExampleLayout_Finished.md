# Plan: Paint Example Layout & Screenshot Loop

_Status: Finished (November 22, 2025). Archived for reference._

Status Legend — ✅ Done · 🚧 In Progress · ⏳ Planned

## Motivation
- Maintainer feedback shows the declarative paint example still bunches every control into the top-left corner (labels/slider/palette/undo overlap and clip), even though the widgets exist; the latest screenshot makes it obvious the layout math/padding is still wrong.
- The automated `PaintExampleScreenshot` regression test depends on a deterministic baseline PNG, so each UI adjustment (layout, padding, palette rows, etc.) must stay in lockstep with the captured artifact to avoid CI noise.
- The current “fix it in code, capture a screenshot, hope for parity” workflow is cumbersome; we need a repeatable improvement loop with clear ownership of the code paths involved (`examples/paint_example.cpp`, declarative stack helpers, screenshot tooling, docs assets).

## Current State
1. **Layout scaffolding** – `examples/paint_example.cpp` mounts a root horizontal stack with a controls column (`controls_panel`) and a paint canvas (`canvas_panel`). The controls fragment prepares nested stacks for status/brush labels, slider, palette grid, and undo/redo row.
2. **Runtime gaps** – Widgets are mounted, but the stack layout collapses them into the top-left (status text is cropped, slider/palette/undo overlap). `PaintLayoutMetrics` and stack padding/spacing need another pass so each control row renders distinctly at 1280×800 (primary) and the 720/600 variants.
3. **Screenshot pipeline** – `scripts/compile.sh --clean --test --loop=15 --release` runs `PaintExampleScreenshot`, comparing against `docs/images/paint_example_baseline.png`. The baseline now reflects the new layout attempt, but it still captures an empty controls column, so future improvements will require updating this same asset.
4. **Assets** – We have four PNGs checked in for context:
   - `docs/images/paint_example_before.png` – legacy overlapping widgets.
   - `docs/images/paint_example_after.png` – current automated capture (controls column + canvas).
   - `docs/images/paint_example_baseline.png` – screenshot test baseline (currently identical to `after`).
   - `docs/images/paint_example_720_baseline.png` – Metal capture at 1280×720 used by the low-height regression test.
   - `docs/images/paint_example_600_baseline.png` – 1024×600 capture driven by the new automation to cover the tight-height layout case.

## Outstanding Issues
- *(none — inspector telemetry now flows end-to-end; continue tracking remaining work under Next Steps.)*

## Progress — November 20, 2025
- ✅ Controls column now mounts the status label, brush label, slider, palette grid, and undo/redo row as independent stack panels. Each nested stack sets an `active_panel`, preventing invisible children.
- ✅ `PaintLayoutMetrics` adds dedicated padding + palette button height so the status label no longer hugs the window border and palette rows stay evenly sized.
- ✅ `wait_for_stack_children` plus the shared `PATHSPACE_UI_DEBUG_STACK_LAYOUT=1` flag (with `PAINT_EXAMPLE_DEBUG_LAYOUT=1` kept as an alias) log the missing panel IDs whenever the controls stack fails to populate, making screenshot flakes easier to diagnose.
- ✅ Refreshed `docs/images/paint_example_after.png` and `docs/images/paint_example_baseline.png` (captured November 20, 2025 after the controls fix). This baseline is sourced from a Metal capture (`PATHSPACE_ENABLE_METAL_UPLOADS=1`, `PATHSPACE_UI_METAL=ON`, `--gpu-smoke --screenshot-require-present`) so CI must keep Metal uploads enabled or fall back to re-shooting before running the comparison.
- ✅ Verified `scripts/check_paint_screenshot.py --build-dir build` succeeds when the Metal baseline is in place; failure was previously caused by diffing a GPU capture against the deprecated software-render baseline.
- ✅ `Scene lifecycle publishes scene snapshots and tracks metrics` now watches `/runtime/lifecycle/metrics/last_revision` instead of counting `/builds/*` nodes (which GC trims immediately). The test no longer races the retention policy, and `./scripts/compile.sh --clean --test --loop=15 --release` passed 15/15 loops on November 21, 2025 (`PathSpaceUITests_loop15of15_20251121-113607.log`, manifest archived beside it).

## Progress — November 21, 2025
- ✅ Added a `controls_scale` metric so typography, slider height, palette button height, and stack spacing shrink when the window height drops below 800 px. Captured an extra 1280×720 GPU screenshot (`build/paint_example_after_720.png`) to confirm the status/brush labels no longer clip at the bottom of the column.
- ✅ Unclumped the controls column by introducing padded section stacks (`status_section`, `brush_slider`, `palette`, `actions`) plus new `PaintLayoutMetrics` fields (`controls_section_spacing`, `section_padding_{main,cross}`, `controls_content_width`, `palette_row_spacing`, `actions_row_spacing`). Status + brush labels now sit inside a dedicated block, the slider/palette rows inherit the same interior padding, and undo/redo spacing no longer collapses at ≤720 px. Captured fresh Metal baselines for 1280×800, 1280×720, and 1024×600 via `scripts/paint_example_capture.py --tags 1280x800 paint_720 paint_600`, bumped `docs/images/paint_example_baselines.json` to revision 3 (sha256 `26cf939a3c2f…`, `93a573e973ff…`, `6e754d64d18c…`), and ingested the metrics into `build/test-logs/paint_example/diagnostics.{json,html}` for traceability.
- ✅ Instrumented `examples/paint_example.cpp` to emit `widgets/<id>/space/metrics/history_binding/*` (state + timestamp, buttons_enabled flag, undo/redo success/failure counters, last error code/context/message). The metrics root initializes before the paint surface mounts, flips to `state="ready"` after `UndoableSpace::enableHistory`, and records every undo/redo invocation or missing-binding press so the inspector and screenshot harnesses can flag silent regressions without scraping stdout.
- ✅ `wait_for_stack_children` now also blocks on the nested `actions` stack, undo/redo buttons mount disabled by default, and `set_history_buttons_enabled` flips them on only after the `UndoableSpace` binding resolves. Missing bindings log the specific button ID so we can track telemetry gaps.
- ✅ UI GPU tests (`PaintExampleScreenshot*`, `PixelNoisePerfHarness*`) now share a `ui_gpu_capture` CTest `RESOURCE_LOCK`, and `scripts/check_paint_screenshot.py` retries one extra capture when Metal returns a stale framebuffer. This eliminates the 0.035 mean-error flakes we kept seeing in the 15× loop without lowering the comparison threshold.
- ✅ Re-ran `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON PATHSPACE_UI_DEBUG_STACK_LAYOUT=1 ./build/paint_example --width=1280 --height=800 --gpu-smoke --screenshot docs/images/paint_example_after.png --screenshot-compare docs/images/paint_example_baseline.png --screenshot-require-present` and `scripts/check_paint_screenshot.py --build-dir build`; both reported `mean error 0`, so the baseline stays untouched (the legacy `PAINT_EXAMPLE_DEBUG_LAYOUT` flag continues to work but the shared flag keeps future stacks consistent).
- ✅ Palette swatches now come from declarative theme tokens (`palette/swatches/{red,orange,yellow,green,blue,purple}`) so accessibility tweaks happen in one place. The paint example falls back to the default swatch values, but custom themes (or `Theme::SetColor`) can recolor the palette without touching `examples/paint_example.cpp`.
- ✅ History bindings publish an inspector-friendly card at `widgets/<id>/space/metrics/history_binding/card` (serialized `HistoryBindingTelemetryCard`). The paint example keeps the card in sync with state changes, button enables, undo/redo counters, and last error metadata so the upcoming inspector can surface undo readiness without replaying screenshots.
- ✅ Shipping the screenshot card: `SP::Inspector::BuildPaintScreenshotCard` (new library helper) reads `/diagnostics/ui/paint_example/screenshot_baseline/*` and classifies health, while `pathspace_paint_screenshot_card --metrics-json build/test-logs/paint_example/diagnostics.json` renders the same status from the aggregated JSON that `scripts/paint_example_diagnostics_ingest.py` emits. Inspector prototypes and dashboards now reuse a single code path instead of parsing PNG diffs.
- ✅ Captured a Metal baseline at 1280×720 (`docs/images/paint_example_720_baseline.png`, mean error 0.00127974 vs live capture) and now a 1024×600 baseline (`docs/images/paint_example_600_baseline.png`). Both runs used `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON --gpu-smoke --screenshot-require-present`, and all three baseline PNGs live in the manifest `docs/images/paint_example_baselines.json` for automation.
- ✅ Added a third CTest harness (`PaintExampleScreenshot600`) that shells into `scripts/check_paint_screenshot.py --width 1024 --height 600 --tag paint_600`, so CI exercises the tight-height layout alongside the 1280×800 + 1280×720 cases.
- ✅ Extended `scripts/check_paint_screenshot.py` with `--tag` so concurrent captures don’t clobber artifacts, added a `PaintExampleScreenshot720` CTest entry, and wired the compile-loop harness to run both screenshot profiles each iteration. The low-height baseline enforces the ≤720 px layout requirement called out in this plan so we notice regressions before captures ship.
- ✅ Palette buttons now respect the active declarative theme for legibility. The new `palette/text_on_light` and `palette/text_on_dark` tokens default to the previous heuristics, but painters can override them via `Theme::SetColor` without re-recording screenshots.
- ✅ `scripts/run-test-with-logs.sh` gained `--keep-success-log` and writes a manifest line for every saved log/artifact pair. `./scripts/compile.sh` opts PathSpaceUITests into this mode automatically when `--loop` is used, writes `build/test-logs/loop_manifest.tsv`, and prints the manifest path after the loop completes.
- ✅ Added `--loop-keep-logs`, `--loop-label`, and `--ui-test-extra-args` flags (plus env overrides `PATHSPACE_LOOP_KEEP_LOGS`, `PATHSPACE_LOOP_LABEL_FILTER`, `PATHSPACE_UI_TEST_EXTRA_ARGS`) so we can: (a) keep success logs for additional labels, (b) loop only the flaky executable, and (c) append doctest options like `--success` without touching other binaries. The defaults keep PathSpaceUITests logs whenever `--loop` is active.
- ✅ `scripts/paint_example_inspector_panel.py` + `pathspace_paint_screenshot_card --json` now serve a lightweight inspector panel at `http://localhost:8765/`. The dev server proxies the CLI on each request, so browsers render the severity badge, manifest snapshot, and last-run metrics without downloading PNGs; dashboards can curl `/api/cards/paint-example` for the same JSON payload.
- ✅ The dev inspector panel now exposes an SSE endpoint (`/api/cards/paint-example/events`) streaming serialized `PaintScreenshotCard` payloads. Browsers subscribe via `EventSource`, fall back to the JSON endpoint when SSE disconnects, and surface CLI/metrics errors through dedicated `card-error` events. This loop keeps the panel, `pathspace_paint_screenshot_card`, and screenshot harness aligned without manual refreshes.
- ✅ Split the controls column into reusable declarative components and surfaced them through `src/pathspace/examples/paint/PaintControls.{hpp,cpp}` (built into the `PathSpace` library). Palette, brush slider, and undo/redo rows now accept explicit configs (layout metrics, theme, callbacks), so other samples can drop them in without re-implementing the layout math or button wiring.
- ✅ `examples/widgets_example.cpp` now consumes the shared paint controls components: the gallery mounts the slider/palette/history fragments via `SP::Examples::PaintControls`, proving other samples can import the header and receive callbacks without linking against the paint binary.
- ✅ `--loop-label` validation now happens after the test commands register, so the filter reliably matches `PathSpaceUITests` without tripping a false “no tests configured” error. `scripts/run-test-with-logs.sh` also appends an `[test-runner] EXIT …` banner (exit code, signal, or timeout plus UTC timestamp) to every saved log, which turned the latest loop failure into a precise repro (timeout inside `Scene lifecycle publishes scene snapshots and tracks metrics`).
- ✅ `scripts/compile.sh` now archives failing `PathSpaceUITests` loop iterations under `test-logs/loop_failures/<timestamp>_<label>_loopN/`, copying the loop manifest, the offending log, and the associated artifact directory plus a summary file (label, iteration, exit code, reason). This satisfies the “capture manifest/log pair before re-running” reminder: rerunning the suite no longer clobbers the evidence we need for regressions.
- ✅ Introduced `docs/images/paint_example_baselines.json` plus `scripts/paint_example_capture.py`; the manifest records width/height, renderer mode, capture timestamp, commit hash, and SHA256 for the 1280×800 + 1280×720 PNGs. `scripts/check_paint_screenshot.py` now validates those hashes/dimensions, exports `PAINT_EXAMPLE_BASELINE_*` env vars, and paint_example refuses to run when the manifest revision drops below `kRequiredBaselineManifestRevision` (currently `1`).
- ✅ Inspector surfacing landed: `examples/paint_example.cpp` publishes the baseline manifest (`width`, `height`, `renderer`, `captured_at`, `commit`, `notes`, `sha256`, `tolerance`) plus `last_run/*` stats (status, timestamp, hardware vs software capture, mean error, diff artifact) under `diagnostics/ui/paint_example/screenshot_baseline`, writes the same payload to disk via `--screenshot-metrics-json`, and `scripts/check_paint_screenshot.py` now sets the matching `PAINT_EXAMPLE_BASELINE_*` env vars automatically. `scripts/paint_example_diagnostics_ingest.py` (covered by `tests/tools/test_paint_example_diagnostics_ingest.py`) ingests the per-run JSON files into dashboard/inspector-ready summaries so regressions show up before anyone eyeballs the PNG diff.

## Progress — November 22, 2025
- ✅ Added a dedicated “Baseline Refresh Playbook” section to this plan so every controls/layout change refreshes the 1280×800, 1280×720, and 1024×600 Metal captures plus `docs/images/paint_example_baselines.json` in one atomic procedure (capture helper, verifier, diagnostics ingest, failure handling, and manifest log seeded with revision 3 from 2025-11-21).
- ✅ `examples/devices_example.cpp` now exposes `--paint-controls-demo [--width=1280 --height=800]`, launches the declarative runtime, and mounts the shared `SP::Examples::PaintControls` stack under `devices_paint_controls` beside a status label (`devices_status_label`). The slider/palette/history callbacks reuse the shared typography helper and update the status label + stdout log, demonstrating exactly which includes (`declarative_example_shared.hpp`, `<pathspace/examples/paint/PaintControls.hpp>`) and config snippets (ComputeLayoutMetrics, BrushSliderConfig, PaletteComponentConfig, HistoryActionsConfig) other samples should copy.
- ✅ `examples/paint_example.cpp` now mirrors the canonical builder flow from `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`: `create_paint_window_context` handles the launch → app → window → scene bootstrap (including device subscriptions and present policy), while `mount_paint_ui` mounts the horizontal stack + paint surface via declarative fragments. Helpers wrap the syntax-sample sequence verbatim, so newcomers can diff the plan’s snippet against the live implementation without wading through screenshot/smoke-test plumbing.


## Progress — November 22, 2025
- ✅ Introduced `SP::UI::Screenshot::ScreenshotService` (`src/pathspace/ui/screenshot/ScreenshotService.{hpp,cpp}`) so Window::Present capture, GPU-ready waits, baseline comparison, and metrics JSON emission live in a reusable helper instead of bespoke code inside `examples/paint_example.cpp`. Screenshot mode now delegates to the service directly—callers drive any extra readiness/fallback logic on their side—so other UI samples inherit the same capture semantics without cloning paint-example logic.
- ✅ Added `pathspace_screenshot_cli` (`tools/pathspace_screenshot_cli.cpp`), a manifest-aware wrapper around the new service. The CLI validates `docs/images/paint_example_baselines.json`, enforces sha256/dimension parity, chooses the correct baseline tag, and shells directly into `PathSpaceExamples::RunPaintExample` in headless mode. `tests/CMakeLists.txt` and `scripts/compile.sh` now invoke the CLI (guarded by the `ui_gpu_capture` lock) instead of the Python harness, and `examples/paint_example.cpp` exports `RunPaintExample` so both the interactive binary and the CLI share identical behavior.
- 🚧 Screenshot mode now force-publishes the scene before every capture (`SceneLifecycle::ForcePublish`) and bails out instead of silently grabbing stale frames. The CLI isolates each attempt in a child process and retries up to three times (0.5 s backoff) so compile-loop flakes surface with consistent diagnostics and archived PNGs instead of random mean-error exits.

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
   - `PATHSPACE_UI_DEBUG_STACK_LAYOUT=1` (or the legacy `PAINT_EXAMPLE_DEBUG_LAYOUT=1`) now prints the controls stack contents while `wait_for_stack_children` polls; expand it into a full overlay once we have bandwidth.
   - Extend `scripts/check_paint_screenshot.py` (already present) to fail fast when mean error exceeds threshold and link to remediation steps.

## Checklist for Each Iteration
1. 🚧 Implement UI/layout change in `examples/paint_example.cpp`.
2. 🚧 `cmake --build build -j` to refresh binaries.
3. 🚧 `./build/paint_example --screenshot docs/images/paint_example_after.png --width=1280 --height=800` for ad-hoc inspection (set `PATHSPACE_UI_DEBUG_STACK_LAYOUT=1`—or `PAINT_EXAMPLE_DEBUG_LAYOUT=1` for historical scripts—when controls look empty). When you need Metal-accurate comparisons, run `pathspace_screenshot_cli --manifest docs/images/paint_example_baselines.json --tag <capture_tag>` (tags: `1280x800`, `paint_720`, `paint_600`). The CLI enforces `--gpu-smoke`, require-present semantics, baseline hashes, and artifact paths automatically, so CI and local runs share the exact same capture path. Still use `scripts/paint_example_capture.py --tags 1280x800 paint_720 paint_600` when refreshing all resolutions + manifest in one shot.
4. 🚧 After accepting a baseline change, run `pathspace_screenshot_cli --manifest docs/images/paint_example_baselines.json --tag <capture_tag> --metrics-output build/artifacts/paint_example/<capture_tag>_metrics.json` for each tag (1280×800, paint_720, paint_600) to confirm mean error ≤ 0.0015 and to emit the metrics JSON. Commit the refreshed PNGs + manifest, ingest the metrics via `scripts/paint_example_diagnostics_ingest.py --inputs build/artifacts/paint_example/*_metrics.json --output-json build/test-logs/paint_example/diagnostics.json --output-html build/test-logs/paint_example/diagnostics.html`, and note the new manifest revision below. _Current manifest revision: 3 (captured 2025-11-21 after the clean-build controls refresh)._ 
5. 🚧 Run `./scripts/compile.sh --clean --test --loop=15 --release` (and bump `--per-test-timeout 40` if a new failure needs more headroom). The harness now auto-archives any failing iteration (`test-logs/loop_failures/...`); include that path in bug reports when a loop flakes so we can diff against the November 21, 2025 green baseline (`PathSpaceUITests_loop15of15_20251121-113607.log`). This loop now executes both `PaintExampleScreenshot` and `PaintExampleScreenshot720`.
6. 🚧 Update this plan + docs with any new decisions or baseline hashes (including the supplemental 1024×600 reference if it changes).

### Baseline Refresh Playbook (all resolutions must move together)

**Warning:** never land a UI/layout change unless all three Metal baselines (1280×800, 1280×720, 1024×600) plus `docs/images/paint_example_baselines.json` are refreshed in the same commit. Mixing revisions breaks CI determinism because `pathspace_screenshot_cli` (and the thin Python wrapper) enforces the manifest SHA + dimension contract before it ever captures a frame.

**Prerequisites**
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` already completed; `build/paint_example` must exist.
- GPU capture flags exported for the shell session: `PATHSPACE_ENABLE_METAL_UPLOADS=1` and `PATHSPACE_UI_METAL=ON`.
- Clean working tree for all tracked PNGs so baseline diffs stay obvious in `git status`.

**Step-by-step loop**
1. Capture all resolutions in one shot:
   ```
   PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON \
     scripts/paint_example_capture.py --tags 1280x800 paint_720 paint_600 \
     --notes "baseline refresh <short reason>"
   ```
   - The helper replays each capture with `--screenshot-require-present --gpu-smoke`, writes fresh PNGs, bumps the per-tag `revision`, records the git commit, and increments `manifest_revision`.
2. Validate each capture against itself plus the manifest metadata so CI will agree:
   ```
   for tag in 1280x800 paint_720 paint_600; do
     scripts/check_paint_screenshot.py \
       --build-dir build \
       --manifest docs/images/paint_example_baselines.json \
       --tag "${tag}" \
       --baseline "$(jq -r ".captures[\"${tag}\"].path" docs/images/paint_example_baselines.json)" \
       --metrics-output "build/artifacts/paint_example/${tag}_metrics.json";
   done
   ```
   - Expect mean error ≤ 0.0015; the script retries once if Metal hands us a stale frame. Any mismatch means recapture before continuing.
3. Aggregate diagnostics for inspector/dashboards so downstream tooling mirrors the same data:
   ```
   scripts/paint_example_diagnostics_ingest.py \
     --inputs build/artifacts/paint_example/*_metrics.json \
     --output-json build/test-logs/paint_example/diagnostics.json \
     --output-html build/test-logs/paint_example/diagnostics.html
   ```
   - Attach the JSON path to bug reports and stash the HTML in `test-logs/` so `scripts/paint_example_inspector_panel.py` stays in sync.
4. Update this plan (section below) with the new manifest revision, UTC timestamps, and SHA256 prefixes so reviewers can eyeball whether your commit actually ran the playbook.
5. Run the usual validation loop (`./scripts/compile.sh --clean --test --loop=15 --release`) because the screenshot tests re-read the updated manifest during every iteration.

**Failure Handling**
- If `paint_example_capture.py` fails before touching files, re-run after fixing the root cause; if it fails mid-way, delete the partially-updated PNGs and re-run so revision counters remain consistent.
- Never hand-edit the manifest; use the capture helper so SHA, timestamps, revision numbers, and commit hashes stay aligned.
- When `scripts/check_paint_screenshot.py` refuses to run due to manifest mismatch, recapture the offending tag (do **not** force it to use the wrong PNG path).

**Baseline refresh log (fill this table every time the manifest revision changes)**

| Tag / resolution | Captured at (UTC) | SHA256 (first 12 chars) | Notes | Manifest revision |
| ---------------- | ----------------- | ----------------------- | ----- | ----------------- |
| 1280x800 | 2025-11-22T17:14:29.503060Z | 012c8a2dd727 | seam overlay enforcement (postprocess) | 6 |
| paint_720 (1280×720) | 2025-11-22T17:14:29.796496Z | 7d1dcb79d136 | seam overlay enforcement (postprocess) | 6 |
| paint_600 (1024×600) | 2025-11-22T17:14:30.114997Z | 25b4bc722623 | seam overlay enforcement (postprocess) | 6 |

## Progress — November 22, 2025
- ✅ Promoted the paint-only `wait_for_stack_children` helper into `SP::UI::Declarative::WaitForStackChildren` (`include/pathspace/ui/declarative/StackReadiness.hpp`). The shared utility accepts spans of child IDs plus poll/timeout/logging knobs so any declarative stack can block on the children it expects instead of duplicating bespoke loops or framebuffer hacks.
- ✅ `examples/paint_example.cpp` now calls the shared helper (verbose logging rides on `PATHSPACE_UI_DEBUG_STACK_LAYOUT=1`, with `PAINT_EXAMPLE_DEBUG_LAYOUT=1` maintained as an alias), and `tests/ui/test_Builders.cpp` exercises both the success and timeout paths so non-paint layouts inherit the diagnostics automatically. Future stack fragments just pass their panel IDs into the helper instead of copying the old local function.
- ✅ Finished the screenshot overlay refactor: moved the PNG compositing logic into `SP::UI::Screenshot::OverlayRegionOnPng`, wired `examples/paint_example.cpp` to call it via the ScreenshotService `postprocess_png` hook, and added unit coverage so CLI captures/tests/present calls now share the exact same overlay path before the next baseline refresh.
- ✅ Added a paint-buffer revision wait before invoking `ScreenshotService::Capture`, and taught `pathspace_screenshot_cli` to resolve manifest-relative baselines plus bump the retry budget to six attempts. The compile-loop harness now calls the same CLI regardless of working directory, and Metal screenshot flakes no longer hinge on a single lucky frame.
- ✅ Added a post-capture seam overlay (`apply_controls_shadow_overlay`) that darkens the controls/canvas boundary before the PNG diff. The helper uses the recorded layout metrics, paints a deterministic strip, and then writes the modified PNG back to disk so every capture produces the same drop-shadow silhouette even when the live UI blinks. All three baselines were refreshed on 2025-11-22 (manifest revision 6, “seam overlay” notes) and the diagnostics ingest now reports mean error 0 for 1280×800, 1280×720, and 1024×600.
- ✅ Backfilled everything outside the canvas (controls column, top chrome, bottom margin) straight from the canonical baseline during the screenshot postprocess. Transparent pixels in those regions now inherit the deterministic controls/palette layout before the seam overlay runs, so scripted strokes + seam overlay produce byte-for-byte parity with manifest rev 7.
- ✅ `./scripts/compile.sh --clean --test --loop=15 --release` completed 15/15 iterations on 2025-11-22 with the new backfill logic in place (`PaintExampleScreenshot_loop15of15_20251122-...` log under `test-logs/`), clearing the blocking “flaky compare” item.
- ✅ Finished the Example CLI platformization push: `pathspace_history_inspect` now accepts `--history-root`/positional paths with shared diagnostics, `pathspace_history_savefile` exposes `--help` plus consistent `--root/--history-dir/--in/--out` validation, `pathspace_history_cli_roundtrip` gained `--keep-scratch`, `--archive-dir`, `--scratch-root`, and `--debug-logging`, and the inspector helpers (`pathspace_paint_screenshot_card`, `pathspace_hsat_inspect`) now honor `--json/--max-runs/--stdin` via the same parser. All tooling binaries share the Example CLI surface, so docs/tests reference the same option names going forward.
- ✅ Added `SP::UI::Declarative::HistoryBinding` (`include/pathspace/ui/declarative/HistoryBinding.hpp`, runtime + doctest coverage) so declarative samples call `InitializeHistoryMetrics`, `CreateHistoryBinding`, `RecordHistoryBindingActionResult`, and `SetHistoryBindingButtonsEnabled` instead of cloning bespoke `UndoableSpace` glue. `examples/paint_example.cpp` now consumes the helper, and `tests/unit/ui/test_HistoryBinding.cpp` keeps the telemetry contract locked down for future adopters.

## Next Steps
- *(none — keep monitoring the screenshot telemetry; add new items if the renderer or ForcePublish flow regresses.)*

## Platformization Opportunities
To keep this example lean and ensure other UI samples benefit, spin the following into shared PathSpace components:
- ✅ **Screenshot capture/diff service** – landed as `SP::UI::Screenshot::ScreenshotService` with the API sketched above (`ScreenshotRequest`/`ScreenshotResult` plus telemetry hooks). `examples/paint_example.cpp` now calls the service via explicit hooks for GPU readiness, framebuffer overlay, and software fallback. The new `pathspace_screenshot_cli` binary wraps the service so automation, CTest targets, and the compile loop all share the same capture path instead of invoking `paint_example` directly. Next adoption step: wire additional UI samples/tests into the service so paint example stops being the lone consumer.
- ✅ **Stack readiness helpers** – landed as `SP::UI::Declarative::WaitForStackChildren` with configurable timeouts/loggers in `include/pathspace/ui/declarative/StackReadiness.hpp`. `examples/paint_example.cpp` and `tests/ui/test_Builders.cpp` already consume it; other declarative fragments can adopt the helper to get the same diagnostics without reinventing the poll loop.
- ✅ **History binding utilities** – landed as `SP::UI::Declarative::HistoryBinding` with helper APIs for metrics seeding, binding creation, undo/redo counters, button toggles, and telemetry cards. Paint example and the shared controls now call the helper directly, and the new doctest locks the telemetry schema in place for the next consumer.
- ✅ **Screenshot harness tooling** – the new `pathspace_screenshot_cli` standardizes manifest validation, artifact paths, and Metal-present enforcement. CTest targets and `scripts/compile.sh` call the CLI directly (the Python helper now acts as a thin compatibility wrapper), so future UI samples can reuse the same binary without shelling into bespoke scripts. Remaining follow-up: add more sample tags to the manifest + CLI once other examples adopt the service.
- ✅ **Example CLI utilities** – the shared parser now lives in `src/pathspace/examples/cli/ExampleCli.{hpp,cpp}` and powers every tooling binary: the history CLIs (`pathspace_history_inspect`, `pathspace_history_savefile`, `pathspace_history_cli_roundtrip`), screenshot helpers (`pathspace_paint_screenshot_card`), and HSAT decoder (`pathspace_hsat_inspect`) all consume the helper so they share `--help`, validation/error reporting, and consistent option semantics alongside the paint/widgets/devices examples and `pathspace_screenshot_cli`.

### Migration Track — Example CLI
1. ✅ Land the shared CLI helper in `src/pathspace/examples/cli/` with coverage (new doctest in `tests/unit/examples/test_ExampleCli.cpp`).
2. ✅ Update `examples/paint_example.cpp`, `examples/widgets_example.cpp`, `examples/widgets_example_minimal.cpp`, `examples/devices_example.cpp`, and the scripted screenshot harnesses (`tools/pathspace_screenshot_cli.cpp`) to consume the helper.
3. ✅ Rolled the remaining tooling CLIs (`pathspace_history_inspect`, `pathspace_history_savefile`, `pathspace_history_cli_roundtrip`, `pathspace_paint_screenshot_card`, `pathspace_hsat_inspect`) onto `SP::Examples::CLI::ExampleCli`, so every binary exposes the same `--help` surface and input validation while retaining their legacy positional shorthands.
