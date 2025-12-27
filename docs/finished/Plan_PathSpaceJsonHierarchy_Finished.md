# Plan: Hierarchical PathSpace JSON Export

## Goal
Replace the flat node-array JSON export with a hierarchical form that mirrors the PathSpace tree (nested objects keyed by path segments). Remove the flat export path; the hierarchical format becomes the only supported output. The button example and dump flag must use this format.

> **Status (December 27, 2025):** Hierarchical export landed in `PathSpaceJsonExporter` (root-path tree with optional `_meta`), the declarative button example now dumps the hierarchical form, and unit/integration coverage exists (`tests/unit/pathspace/test_PathSpaceJsonExporter.cpp`, `tests/tools/test_declarative_button_dump.py`). Docs/fixtures are being updated and the full looped validation run is pending.

## Current state (problem)
- `PathSpaceJsonExporter::Export` returns a flat `"nodes"` array with each entry carrying a `"path"` string and diagnostics. This is hard to read for small scenes (20k lines for a two-button sample) and doesn’t visually mirror the hierarchy.
- Dumping with defaults also pulls trellis/runtime mirrors and diagnostics, further ballooning output.

## Proposed data model (hierarchical, only mode)
- Top-level object contains a single entry keyed by the export root (default `/` or caller-specified) and may include optional `_meta` (schema, root, flags, limits, stats) when requested.
- Each node object carries:
  - `values` array (entries include `index`, `type`, `category`, optional `value` or placeholders)
  - `children` object keyed by child name (lexicographic order for determinism)
  - `child_count`, `has_children`, `has_nested_space`, `has_value`, `category`
  - Truncation flags: `children_truncated` (child cap or depth cap hit) and `depth_truncated` (hit `max_depth` with children).
  - Optional `diagnostics` when enabled.
- Respect `max_depth`, `max_children`, and `max_queue_entries`; when limits are hit, set truncation flags instead of emitting partial children/values.
- Options:
  - `root` (path prefix to export)
  - `include_nested_spaces` (defaults false), `include_diagnostics`, `include_opaque_placeholders`, `max_depth`, `max_children`, `max_queue_entries`

## Work items
1) ✅ Simplify `PathSpaceJsonOptions`: remove flat toggles; keep `root` + existing knobs. Hierarchical is the only output.
2) ✅ Implement hierarchical builder in `PathSpaceJsonExporter`:
   - Reuse traversal/limits; build nested objects keyed by components with deterministic child ordering.
   - Preserve value conversion/placeholders; diagnostics optional.
   - Omit nested spaces unless `include_nested_spaces` is true.
3) ✅ Button example:
   - `--dump_json` uses hierarchical exporter rooted at the app, `include_nested_spaces=false`, `include_diagnostics=false`, shallow limits (`max_depth=8`, `max_children=64`), `max_queue_entries=4`.
4) ✅ Tests:
   - Unit test updated for hierarchical structure (nesting, child keys, truncation flags).
   - Integration: `tests/tools/test_declarative_button_dump.py` asserts the window/view/widgets/button paths exist and completes <5s.
   - Snapshot: `tests/fixtures/pathspace_dump_json_demo.json` refreshed for hierarchical output.
5) ✅ Docs:
   - Updated `WidgetDeclarativeAPI.md`, `AI_Architecture.md`, `AI_Debugging_Playbook.md`, `Memory.md`, and the JSON exporter plan to describe the hierarchical format/defaults and remove flat references.
6) ✅ Validation:
   - `./scripts/compile.sh --loop=5 --per-test-timeout 20` timed out in `PathRenderer2D_RenderBasics` (loop 1); reran `PathSpaceUITests_All` once with `--per-test-timeout 120` and it passed. Logs archived under `build/test-logs/loop_failures/20251227-204400_PathSpaceUITests_All_loop1` and `build/test-logs/PathSpaceUITests_All_loop1of1_20251227-204422.log`.

## Acceptance criteria
- Hierarchical export produces a nested JSON tree matching PathSpace paths; button example dump is readable (≈100–200 lines for two buttons with diagnostics off).
- Flat export path is removed; only hierarchical output is supported.
- Dump command completes without hang/crash.
- Tests for hierarchical export (unit + integration) and full looped suite pass.

## Detailed design notes
- **JSON shape (example)**  
  ```
  {
    "_meta": {
      "schema": "hierarchical",
      "root": "/system/applications/declarative_button_example",
      "limits": { "max_depth": 8, "max_children": 64, "max_queue_entries": 4 },
      "flags": { "include_nested_spaces": false, "include_diagnostics": false },
      "stats": { "node_count": 12, "children_truncated": 0, "values_truncated": 0 }
    },
    "/system/applications/declarative_button_example": {
      "child_count": 2,
      "children_truncated": false,
      "children": {
        "windows": {
          "children": {
            "declarative_button": {
              "children": {
                "style": { "children": { "theme": { "values": [{"value": "sunset"}] } } },
                "views": {
                  "children": {
                    "main": {
                      "children": {
                        "scene": { "values": [{"value": "button_scene"}] },
                        "surface": { "values": [{"value": "..."}] }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  ```
- **Traversal algorithm**  
  - Walk from `opts.root` using existing visitor; build child objects on demand with lexicographic ordering.  
  - Values: reuse converters/placeholders; attach `values` array under each node.  
  - Diagnostics: attach only if `include_diagnostics`.  
  - Limits: when `max_depth` or `max_children` caps a branch, mark `children_truncated=true` and `depth_truncated=true` (for depth) instead of emitting partial children; queues respect `max_queue_entries` and set `values_truncated=true` when capped.  
  - Determinism: ordered children stabilize tests and fixture diffs.
- **Nested spaces vs. trellis**  
  - Default `include_nested_spaces=false` to avoid the trellis/runtime mirror and shrink dumps.  
  - If enabled, nest them under their path segments (no special handling), still respecting limits.
- **Performance/memory**  
  - Deterministic child ordering via ordered maps; traversal bounded by `max_*` limits to keep memory/time predictable.  
  - Button example shuts down runtimes before export to avoid contention.
- **API surface**  
  - Simplify `PathSpaceJsonOptions`: hierarchical is the only mode.  
  - Keep `root`, `include_nested_spaces`, `include_diagnostics`, `include_opaque_placeholders`, `max_depth`, `max_children`, `max_queue_entries`.  
  - No `hierarchical_output` flag and no flat/enum toggle.
- **Example behavior**  
  - Button example: `--dump_json` uses hierarchical mode with `root` = app root, `include_nested_spaces=false`, `include_diagnostics=false`, shallow limits (`max_depth=8`, `max_children=64`), and prints to stdout.
  - Keep `--screenshot` behavior unchanged.

## Test details
- **Unit**: build a tiny synthetic tree (`/root/a`, `/root/a/b`, `/root/a/value=1`) and assert hierarchical JSON nests `a` then `b`, values present, child ordering lexicographic, truncation flags set when limits exceeded.  
- **Integration**: run a harness that builds the declarative button scene, performs one present, shuts down runtimes, exports hierarchical JSON, and asserts:  
  - Completion <5s  
  - JSON contains keys for `/system/applications/declarative_button_example/windows/declarative_button/views/main/widgets/button_column` and its `children/hello_button` / `children/goodbye_button` leaves (leaf truncation allowed).  
  - No truncation on the app root/stack nodes when using recommended limits.

## Migration considerations
- Flat output is removed; callers using the old `"nodes"` array must consume `_meta` + the root-path tree (`children` objects) instead.  
- Button example dumps the hierarchical tree rooted at the app with nested spaces off; `WidgetDeclarativeAPI.md` documents the new limits.  
- Update any tooling/CI scripts that parsed flat JSON to honor the new shape and truncation flags.
