# Plan: Minimal PathSpace Core (no noisy branches)

> **Status:** Complete — default runtime/exporter/theme writes are minimal by default and button/example fixtures reflect the lean tree (December 27, 2025).  
> **Archive:** Moved from `docs/Plan_PathSpaceMinimalCore.md` after meeting all acceptance criteria; retained here for historical context.

## Goal
Make PathSpace intrinsically minimal: stop writing non-essential renderer/runtime/theme/diagnostic nodes so any dump (hierarchical) naturally stays small and readable without filters. Minimalism applies globally—not just in examples or via export options.

## Problems today (seen in `out.json`)
- Renderer targets/diagnostics/metrics under `/renderers/.../targets/...` bloat the tree.
- Trellis/runtime logs and revision history (including “InputTask mailbox…” warnings) are stored in PathSpace.
- Opaque `WidgetTheme` blobs are inserted under `/config/theme/*/value`.
- Default diagnostics/present/renderer/htmlTarget branches appear under windows/views.
- Exporter `_meta` and filter machinery are used to hide these rather than eliminating them.

## Principles
- Only store what the running scene needs: window/view/scene/surface, widget hierarchy, active theme name.
- No implicit logging/diagnostic/state writes into PathSpace unless explicitly requested by a tool.
- Exports require no include/exclude filters; the tree itself stays lean.

## Work items
1) Renderer/runtime writes
   - ✅ Default runtime now skips renderer target hints, diagnostics, and present/residency metrics unless `PATHSPACE_UI_DEBUG_TREE=1` (aliases: `PATHSPACE_UI_DEBUG_DIAGNOSTICS`, `PATHSPACE_UI_DEBUG_PATHSPACE`).
   - ✅ Trellis/input/scene lifecycle metrics, revision logs, and mailbox warnings are gated by the same flag; minimal runs leave those branches empty.
2) Theme storage
   - ✅ Avoid storing `WidgetTheme` blobs (`/config/theme/*/value`) by default; keep only the active theme name string. Theme values are now persisted only when callers explicitly write theme data (e.g., `Theme::Create`/`SetColor`); runtime boot leaves the theme subtree absent aside from `config/theme/active`.
3) Window/view defaults
   - ✅ Window/view bindings now avoid writing renderer/htmlTarget/present branches unless explicitly requested: view renderer links come from the surface target, html targets are only stored when attached, and present policy nodes are opt-in.
4) ✅ Exporter cleanup
   - Filters/presets removed (no include/exclude prefixes or noisy-branch skipping); exporter walks the whole tree.
   - `_meta` is opt-in (`includeMetadata` / `--include-meta` or implied by debug); defaults emit only the root tree.
5) ✅ Examples/tools
   - Button example exports the lean tree directly (no filters), bumps depth to keep widget labels, collapses scene snapshots to the latest revision, and routes `--dump_json_debug` to the debug preset.
6) ✅ Tests
   - Declarative button dump test expects labels to be present and enforces the slim output budget; dump fixtures remain aligned with the minimal exporter.
7) ✅ Validation
   - `./scripts/compile.sh --release --loop=5 --per-test-timeout 20` green; logs under `build/test-logs/`.

## Acceptance criteria
- Default runtime no longer writes renderer/metrics/trellis/theme-blob/diagnostic nodes unless explicitly enabled.
- Dumps of the button example show the full widget hierarchy (including labels) and remain small without filters.
- Exporter emits the tree without `_meta` filters; any remaining limits are explicit.
- All tests pass; full looped suite green.
