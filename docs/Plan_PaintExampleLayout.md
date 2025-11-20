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

## Outstanding Issues
1. **Clipped status text** – The new padding/typography should stop glyph clipping at 1280×800, but we still need to validate smaller (≤900 px tall) windows before marking this closed. Adjust `PaintLayoutMetrics` if clipping remains.
2. **Palette layout polish** – Palette buttons now share a fixed height/rounded style, yet they still inherit theme defaults. Decide whether to expose a declarative palette theme token or hard-code contrast colors for accessibility.
3. **Undo/Redo wiring** – Buttons rely on `history_binding` setup that happens after the controls stack mounts. Add explicit telemetry/log entries when the binding is missing and consider greying out the buttons until history is ready.
4. **Screenshot drift** – Every visual change forces a new baseline PNG. Keep following the documented capture loop and track checksum/date updates inside this plan so review history stays obvious.

## Progress — November 20, 2025
- Controls column now mounts the status label, brush label, slider, palette grid, and undo/redo row as independent stack panels. Each nested stack sets an `active_panel`, preventing invisible children.
- `PaintLayoutMetrics` adds dedicated padding + palette button height so the status label no longer hugs the window border and palette rows stay evenly sized.
- `wait_for_stack_children` plus `PAINT_EXAMPLE_DEBUG_LAYOUT=1` log the missing panel IDs whenever the controls stack fails to populate, making screenshot flakes easier to diagnose.
- Refreshed `docs/images/paint_example_after.png` and `docs/images/paint_example_baseline.png` (captured November 20, 2025 after the controls fix). See the Checklist for the capture command.

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
3. `./build/paint_example --screenshot docs/images/paint_example_after.png --width=1280 --height=800` for visual inspection (set `PAINT_EXAMPLE_DEBUG_LAYOUT=1` when controls look empty).
4. If accepted, copy `paint_example_after.png` → `paint_example_baseline.png` and record the date/commit here.
5. Run `./scripts/compile.sh --clean --test --loop=15 --release` and ensure `PaintExampleScreenshot` passes (mean error ≤ 0.0015).
6. Update this plan + docs with any new decisions or baseline hashes.

## Next Steps
1. Finish wiring the controls column so all widgets render and respond (owner: Codex).
2. Re-shoot the baseline PNG once the UI reflects the intended layout, then document the new checksum/date here.
3. Evaluate whether we should split palette/slider/undo sections into dedicated declarative components for reuse in other samples.
