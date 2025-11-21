# Plan: Paint Example Layout & Screenshot Loop

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
1. **Palette layout polish** – Palette buttons now share a fixed height/rounded style, yet they still inherit theme defaults. Decide whether to expose a declarative palette theme token or hard-code contrast colors for accessibility.
2. **Undo/Redo wiring** – Buttons now log the missing binding and stay disabled until `history_binding` resolves, but we still need telemetry (metrics node + inspector surfacing) so regressions are obvious outside screenshot runs.
3. **Screenshot drift** – Every visual change forces a new baseline PNG. Keep following the documented capture loop and track checksum/date updates inside this plan so review history stays obvious.
4. **Pre-push loop instability** – `./scripts/compile.sh --clean --test --loop=15 --release` still flakes inside `PathSpaceUITests` (runs 1‑5 pass, loop ≥6 dies with `233 passed | 1 failed` yet no concrete failure is logged). Single-test invocations pass, so the looped runner needs deeper instrumentation/log capture to unblock pushes.

## Progress — November 20, 2025
- Controls column now mounts the status label, brush label, slider, palette grid, and undo/redo row as independent stack panels. Each nested stack sets an `active_panel`, preventing invisible children.
- `PaintLayoutMetrics` adds dedicated padding + palette button height so the status label no longer hugs the window border and palette rows stay evenly sized.
- `wait_for_stack_children` plus `PAINT_EXAMPLE_DEBUG_LAYOUT=1` log the missing panel IDs whenever the controls stack fails to populate, making screenshot flakes easier to diagnose.
- Refreshed `docs/images/paint_example_after.png` and `docs/images/paint_example_baseline.png` (captured November 20, 2025 after the controls fix). This baseline is sourced from a Metal capture (`PATHSPACE_ENABLE_METAL_UPLOADS=1`, `PATHSPACE_UI_METAL=ON`, `--gpu-smoke --screenshot-require-present`) so CI must keep Metal uploads enabled or fall back to re-shooting before running the comparison.
- Verified `scripts/check_paint_screenshot.py --build-dir build` succeeds when the Metal baseline is in place; failure was previously caused by diffing a GPU capture against the deprecated software-render baseline.
- Loop harness remains red because `PathSpaceUITests` exits with `1 failed` despite every individual case passing outside the loop; logs live under `build/test-logs/PathSpaceUITests_loop{6,8}of15_20251120-2345xx.log`. Need to expand the loop runner to preserve the full doctest output (the truncated summary hides which test tripped SIGTERM) and possibly rerun with `--success` to narrow it down.

## Progress — November 21, 2025
- Added a `controls_scale` metric so typography, slider height, palette button height, and stack spacing shrink when the window height drops below 800 px. Captured an extra 1280×720 GPU screenshot (`build/paint_example_after_720.png`) to confirm the status/brush labels no longer clip at the bottom of the column.
- `wait_for_stack_children` now also blocks on the nested `actions` stack, undo/redo buttons mount disabled by default, and `set_history_buttons_enabled` flips them on only after the `UndoableSpace` binding resolves. Missing bindings log the specific button ID so we can track telemetry gaps.
- Re-ran `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON PAINT_EXAMPLE_DEBUG_LAYOUT=1 ./build/paint_example --width=1280 --height=800 --gpu-smoke --screenshot docs/images/paint_example_after.png --screenshot-compare docs/images/paint_example_baseline.png --screenshot-require-present` and `scripts/check_paint_screenshot.py --build-dir build`; both reported `mean error 0`, so the baseline stays untouched.
- `./scripts/compile.sh --clean --test --loop=15 --release` still tripped the known `PathSpaceUITests` flake on loop 6 (`loop_run.log` + `build/test-logs/PathSpaceUITests_loop6of15_20251121-001616.artifacts`). No individual doctest output survives, reinforcing the need for richer loop logging.
- Captured a Metal baseline at 1280×720 (`docs/images/paint_example_720_baseline.png`, mean error 0.00127974 vs live capture) and a supplementary 1024×600 reference (`docs/images/paint_example_600_reference.png`). These runs used `PATHSPACE_ENABLE_METAL_UPLOADS=1 PATHSPACE_UI_METAL=ON --gpu-smoke --screenshot-require-present`.
- Extended `scripts/check_paint_screenshot.py` with `--tag` so concurrent captures don’t clobber artifacts, added a `PaintExampleScreenshot720` CTest entry, and wired the compile-loop harness to run both screenshot profiles each iteration. The low-height baseline enforces the ≤720 px layout requirement documented in Outstanding Issue #1.

## Work Breakdown
1. **Controls Column MVP**
   - Mount status & brush labels with typography + padding.
   - Mount slider with width tied to `controls_width`.
   - Build palette grid via nested horizontal stacks; ensure each button fragment inherits consistent spacing.
   - Mount undo/redo row and confirm history binding availability.
2. **Layout Metrics & Styling**
   - Tune `PaintLayoutMetrics` padding/spacing values for 1280×800 default while scaling down gracefully (≥800×600).
   - Add optional theme overrides (colors, corner radii) so controls visually contrast against the dark background.
3. **Screenshot Discipline**
   - Document the exact capture commands (`./build/paint_example --screenshot …`, `scripts/compile.sh …`) and store them near the plan (see Checklist below).
   - Record baseline SHA in this doc whenever we update `docs/images/paint_example_baseline.png`.
4. **Validation Hooks**
   - `PAINT_EXAMPLE_DEBUG_LAYOUT=1` now prints the controls stack contents while `wait_for_stack_children` polls; expand it into a full overlay once we have bandwidth.
   - Extend `scripts/check_paint_screenshot.py` (already present) to fail fast when mean error exceeds threshold and link to remediation steps.

## Checklist for Each Iteration
1. Implement UI/layout change in `examples/paint_example.cpp`.
2. `cmake --build build -j` to refresh binaries.
3. `./build/paint_example --screenshot docs/images/paint_example_after.png --width=1280 --height=800` for visual inspection (set `PAINT_EXAMPLE_DEBUG_LAYOUT=1` when controls look empty). Use `--gpu-smoke --screenshot-require-present --screenshot-compare docs/images/paint_example_baseline.png` whenever the baseline needs to stay Metal-accurate, and repeat with `--height=720 --screenshot docs/images/paint_example_720_baseline.png` after UI changes to keep the low-height asset current.
4. If accepted, copy `paint_example_after.png` → `paint_example_baseline.png`, update the 720 baseline when needed, note the capture date + commit hash here, and run `scripts/check_paint_screenshot.py --tag paint_720 --height 720 --baseline docs/images/paint_example_720_baseline.png` to confirm mean error ≤ 0.0015 (diffs may remain when `max_channel_delta > 0`).
5. Run `./scripts/compile.sh --clean --test --loop=15 --release` (and bump `--per-test-timeout 40` if needed). If the loop fails due to the known `PathSpaceUITests` flake, archive `loop_run.log` plus the offending `build/test-logs/PathSpaceUITests_loop*.log` so the maintainer can inspect the truncated doctest summary. This loop now executes both `PaintExampleScreenshot` and `PaintExampleScreenshot720`.
6. Update this plan + docs with any new decisions or baseline hashes (including the supplemental 1024×600 reference if it changes).

## Next Steps
1. Add undo/redo binding telemetry (metrics node + inspector surfacing) so disabled buttons and missing bindings are obvious outside the screenshot workflow.
2. Evaluate whether we should split palette/slider/undo sections into dedicated declarative components for reuse in other samples.
3. Decide whether to add a second low-height automation (1024×600) or treat `docs/images/paint_example_600_reference.png` as a manual audit artifact only.

## Platformization Opportunities
To keep this example lean and ensure other UI samples benefit, spin the following into shared PathSpace components:
- **Screenshot capture/diff service** – move the GPU smoke wait, `Window::Present` capture, overlay, and baseline comparison logic out of `examples/paint_example.cpp` into a reusable UI-layer helper so every sample and test can trigger deterministic screenshots without bespoke plumbing.
- **Stack readiness helpers** – promote `wait_for_stack_children` (plus the `PAINT_EXAMPLE_DEBUG_LAYOUT` logging) into a declarative runtime utility that any stack fragment can call to verify child panels and emit consistent diagnostics.
- **History binding utilities** – provide a builder-level hook for wiring `UndoableSpace` instances (alias creation, options, enable, telemetry surfaces) instead of re-implementing the binding flow inside each example.
- **Screenshot harness tooling** – evolve `scripts/check_paint_screenshot.py` and the CTest glue into a first-class PathSpace CLI/module, standardizing artifact paths, tags, and Metal-present enforcement so new samples inherit the discipline automatically.
