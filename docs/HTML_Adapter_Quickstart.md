# PathSpace HTML Adapter Quickstart & Troubleshooting

> **Handoff note (October 22, 2025):** This guide documents the current HTML adapter harness, HSAT tooling, and common failure signatures so the next session can resume UI verification without rediscovering the workflow. Keep it updated when harness flags, output paths, or diagnostics change.

## 1. Prerequisites
- Build tree configured in Release mode:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ```
- Node.js available on `PATH` (required for the headless verification scripts).
- `build/tests/html_canvas_dump` and `build/pathspace_hsat_inspect` produced by the build step above.
- Optional: set `PATHSPACE_ENABLE_METAL_UPLOADS=1` when you explicitly want GPU uploads during local HTML runs; the harness defaults to the software path so runs stay deterministic.

## 2. HTML Adapter Quickstart
1. **Run the headless replay harness (Canvas + DOM parity):**
   ```bash
   ctest -R HtmlCanvasVerify --output-on-failure
   ```
   - Executes `scripts/verify_html_canvas.js` against `html_canvas_dump`.
   - Scenarios covered: `widgets-default` (default theme) and `widgets-sunset` (alternate palette). Colors, command ordering, radii, and structural fields must match the native renderer output.
   - Automatically skips (exit code 0) when Node.js is unavailable; treat the skip as a warning and install Node before the next loop run.

2. **Inspect HSAT payload decoding:**
   ```bash
   ctest -R HtmlAssetInspect --output-on-failure
   ```
   - Invokes `scripts/verify_hsat_assets.js <build/pathspace_hsat_inspect>`.
   - Generates a synthetic HSAT payload, confirms magic/version (`0x48534154`, `0x0001`), validates MIME-specific kind detection, and checks byte previews/reference flags.
   - Verifies the inspector’s aggregate output (kind/mime summaries, empty-asset counts, duplicate logical paths) so dataset regressions fail fast.

3. **Manual spot-check:**
   ```bash
   node scripts/verify_html_canvas.js build/tests/html_canvas_dump
   node scripts/verify_hsat_assets.js build/pathspace_hsat_inspect
   ```
   Use the manual commands when debugging a single harness without running the full CTest suite. Pass `--prefer-dom` or `--scenario basic|widgets-default|widgets-sunset` to `html_canvas_dump` to narrow the reproduction.

4. **Review adapter outputs in PathSpace:**
   - Command stream and DOM/CSS live under `renderers/<rid>/targets/<tid>/output/v1/html/{commands,dom,css}`.
   - Mode metadata: `output/v1/html/{mode,usedCanvasFallback,commandCount,domNodeCount}`.
   - Residency/metrics mirror into `renderers/<rid>/targets/<tid>/diagnostics/metrics` and `windows/<win>/diagnostics/metrics/live/views/<view>/present`.

## 3. Common Failure Signatures & Fixes
- **`Usage: node verify_html_canvas.js <html-canvas-dump>`** — The harness could not locate `html_canvas_dump`. Rebuild the tree (`cmake --build build -j`) and ensure you pass the binary path (CTest does this automatically).
- **`Invocation 'html_canvas_dump …' produced no output`** — The helper crashed or could not publish a snapshot. Re-run `html_canvas_dump` manually with `--scenario basic` and inspect stderr; verify the builders can author scenes (`ctest -R PathSpaceUITests --output-on-failure`).
- **`missing color` / `expected rgba(...)` errors** — Theme digests diverged. Confirm the widgets gallery produces the expected colors by rerunning `html_canvas_dump --scenario widgets-default` and diffing the generated JSON against `build/test-logs/html_canvas_dump_*.json`. Recent style changes require updating both the renderer palette and the DOM compositor.
- **`Canvas command array is empty` or structural field assertions** — Typically indicates a regression in `SceneSnapshotBuilder` or adapter traversal. Check `renderers/<rid>/targets/<tid>/output/v1/common/frameIndex` to confirm the render completed, then inspect `scenes/<sid>/builds/<rev>/drawables` for missing buckets.
- **`usedCanvasFallback=true` unexpectedly** — DOM mode exceeded `HtmlTargetDesc::max_dom_nodes` or DOM emission failed. Increase `max_dom_nodes`, set `allow_canvas_fallback=false` during local testing to surface a hard error, and watch `diagnostics/errors/live` for adapter error details.
- **HSAT decode errors (`magic mismatch`, `assetCount` mismatch)** — Generated payload is stale or corrupted. Regenerate by rerunning any HTML present (e.g., `html_canvas_dump`), then re-run `pathspace_hsat_inspect --input <payload.hsat>` to confirm the header and per-asset lengths. If assets are missing bytes, verify `Renderer::RenderHtml` publishes hydrated payloads under `output/v1/html/assets/*`.
- **`HSAT inspection verified` missing or Node exits with `ENOENT`** — Node.js not installed or not on `PATH`. Install Node ≥18 or adjust your environment; without it the HSAT harness is skipped and gaps may hide.

## 4. Workflow Reminders
- Always rerun the full loop before handoff:
  ```bash
  ./scripts/compile.sh --test --loop=5 --per-test-timeout=20
  ```
  This wrapper will also trigger `HtmlCanvasVerify` and `HtmlAssetInspect` when Node is available.
- When HTML outputs change intentionally, update:
  1. `docs/finished/Plan_SceneGraph_Renderer_Finished.md` (fidelity, HSAT schema, mode semantics).
  2. `docs/Plan_SceneGraph.md` (status snapshot + follow-ups).
  3. This quickstart with new commands, environment variables, or failure signatures.
- Capture notable regressions in `docs/AI_Todo.task` with acceptance criteria so the next cycle can prioritize the fix.

## 5. Related Tools & References
- `tests/tools/html_canvas_dump.cpp` — Generates deterministic scenes for the harness.
- `scripts/verify_html_canvas.js` — Parity checker for DOM/CSS/Canvas outputs (themes, geometry, assets).
- `tools/hsat_inspect.cpp` — HSAT decoder used by both the CLI and verification script.
- `docs/AI_Debugging_Playbook.md` §5 — Command reference for running the harnesses alongside the standard test loop.
- `docs/finished/Plan_SceneGraph_Renderer_Finished.md` — Source of truth for adapter options (`prefer_dom`, `max_dom_nodes`, `allow_canvas_fallback`) and HSAT framing details.

Keep this document lightweight but current—future render/adapter changes should land here first so incoming maintainers can validate HTML fidelity without spelunking.
