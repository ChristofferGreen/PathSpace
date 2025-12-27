# Plan: PathSpace JSON Exact Dump (no filters)

## Goal
Emit a hierarchical JSON that is an exact representation of PathSpace, with no implicit include/exclude filters. The current `out.json` (258 lines) still has `_meta` filters that drop renderers, scenes, surfaces, widget subpaths, etc. Remove that filtering so the dump reflects all paths (subject only to explicit depth/child limits if the caller sets them).

## Problems seen in out.json
- `_meta.flags.exclude_path_prefixes` skips renderers, scenes, surfaces, diagnostics, widget runtime/log/metrics paths.
- Button labels/props are missing because child truncation and filters cut off deeper widget nodes.
- `_meta` carries filter metadata that shouldn’t be needed for an exact dump.

## Desired behavior
- Hierarchical JSON; no include/exclude path filters by default.
- Defaults: include all values; diagnostics optional (default off); nested spaces optional but enabled should show trellis/runtime too.
- Only pruning comes from explicit `max_depth`/`max_children` chosen by the caller; defaults should be generous (or “no cap”).
- `_meta` should be minimal or optional; no filter lists when none are applied.

## Work items
1) Exporter
   - Remove path filter machinery (include/exclude prefixes, omitNoisyBranches, mode presets).
   - Provide simple options: `root`, `include_nested_spaces`, `include_values`, `include_diagnostics`, `include_opaque_placeholders`, `max_depth`, `max_children`, `max_queue_entries`.
   - Default: no filters, large/no depth cap, include values, diagnostics=false, nested_spaces=false.
   - Make `_meta` optional; when present, list only root/limits/settings (no filters if none).
2) Example (`declarative_button_example`)
   - `--dump_json` calls exporter with defaults: no filters, no depth cap; include values, diagnostics off.
   - Keep `--screenshot` path unchanged.
3) Tests
   - Dump run finishes <5s, contains deep widget props (button labels).
   - Optional test with `max_depth` set to verify truncation behaves as expected (explicit, not implicit).
4) Docs
   - Update exporter docs and `WidgetDeclarativeAPI.md` to state there are no automatic filters; pruning only via explicit limits.
5) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20`.

## Acceptance criteria
- `./build/declarative_button_example --dump_json` produces a hierarchical JSON with all paths (including button labels), no filter lists in `_meta`, and completes without hang/crash.
- Exporter API exposes only simple options; no implicit filtering.
- Tests and full loop pass.
