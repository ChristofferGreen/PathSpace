# Plan: Minimal PathSpace JSON Export

## Goal
Produce a concise, human-friendly JSON dump of PathSpace that includes only structure and values, not low-level diagnostics/metrics flags. Default output for dumps (e.g., button example) should be small (≈100–200 lines for the two-button scene) and easy to read. A separate opt-in “debug” mode can include diagnostics if needed.

## Status (2025-12-27)
- Minimal preset implemented as the default: structure/diagnostics placeholders stripped, metadata omitted, shallow limits (depth=6, children=64, queue=2), nested spaces off, opaque placeholders off, and no exporter-side path filters (tree is expected to be lean by construction).
- Debug preset available via `PathSpaceJsonOptions::Mode::Debug` / `--debug` / `--dump_json_debug`, restoring structure fields, diagnostics, placeholders, metadata, and the broader traversal knobs.
- `declarative_button_example --dump_json` now uses the minimal preset (rooted at the app, depth=10, children=64, queue=2); `--dump_json_debug` emits the verbose view.
- `pathspace_dump_json` defaults to minimal (no filters) with optional `--include-meta`; fixture refreshed.
- Tests updated: minimal vs debug unit coverage plus the button integration test; loop run (`ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20`) now passes everywhere except `PathSpaceUITests_All`, which times out in this sandbox due to `/bin/ps` restrictions (known harness issue).

## Current pain
- Even with hierarchical export, dumps include diagnostics (has_children, has_value, queue_depth, raw_bytes, etc.) and runtime/metrics nodes, yielding huge output.
- Redundant metadata (`has_children`, `children_truncated`) can be inferred from the structure and limits.
- Metrics/diagnostics are rarely needed for a scene snapshot but bloat the dump.
- Runtime/trellis mirror (`.../runtime/lifecycle/trellis/...`) doubles the tree.
- Renderer/metrics/targets nodes add noise.
- Theme blobs (`WidgetTheme` serialized) are opaque and large.

## Target output (minimal mode)
- Nested object keyed by path segments (hierarchical tree).
- Per node: `children` object and optional `values` array (with type/category).
- No diagnostics fields (`has_children`, `has_value`, `children_truncated`, `depth_truncated`, queue/bytes).
- Default excludes runtime/trellis mirrors, renderer metrics, and live diagnostics.
- Defaults: `include_nested_spaces=false`, `include_diagnostics=false`, shallow limits (depth ~6, children ~64), `include_opaque_placeholders=false` unless values can’t be converted.
- Default `root` = app root; skip renderer/metrics/targets unless explicitly enabled.

## Work items
1) Exporter options
   - Add a “minimal” preset (or make it the default) that disables diagnostics, nested spaces, and metrics/renderer/trellis trees.
   - Keep a “debug” toggle to re-enable diagnostics if explicitly requested.
   - Allow root path selection and limits (depth/children/queue) as before.
2) Exporter behavior
   - In minimal mode, omit diagnostic fields entirely.
   - Skip known noisy subtrees by default: runtime/trellis mirror, renderer metrics, live diagnostics. Allow opt-in via `include_nested_spaces`/flags.
   - When values are opaque, either omit them or emit a short placeholder without diagnostics.
3) Example (button)
   - `--dump_json` uses minimal preset (no diagnostics, no nested spaces, tight limits).
   - Provide an optional `--dump_json_debug` to emit full diagnostics if needed.
4) Tests
   - Unit: minimal export of a tiny tree contains only `children`/`values`, no diagnostics keys.
   - Integration: button example minimal dump finishes <5s and stays under a reasonable size (assert node count or string length threshold), contains window/scene/stack/button.
   - Debug mode test: enabling diagnostics includes the extra fields.
5) Docs
   - Update exporter and `WidgetDeclarativeAPI.md` to describe minimal vs debug, defaults, and flags.
6) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20`.

## Acceptance criteria
- Minimal mode produces a compact hierarchical JSON without diagnostics/metrics; button dump is small and readable.
- Debug mode available but opt-in.
- Dump completes without hang/crash.
- Tests pass; full looped suite green.

## Trim list (derived from current dumps, e.g., `out.json` from the button example)
- Drop per-node diagnostics: `child_count`, `children_truncated`, `depth_truncated`, `has_children`, `has_nested_space`, `has_value`, `values_sampled`, `values_truncated`, queue/bytes.
- Exclude runtime/trellis mirror by default (`.../runtime/lifecycle/trellis/...`).
- Exclude renderer/metrics/targets and present diagnostics by default.
- Handle themes minimally: keep the active theme name; omit or summarize `WidgetTheme` blobs (no opaque payloads in minimal mode).
- Omit opaque placeholders unless conversion fails and no value can be shown.
