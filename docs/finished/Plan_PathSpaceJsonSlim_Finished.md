# Plan: Slim PathSpace JSON Export (targeting out.json bloat)

> **Update (December 27, 2025):** The slim preset and include/exclude filters were removed as part of the Minimal PathSpace Core work—exporter no longer filters paths and metadata is opt-in. This plan is retained for historical context only.

## Goal
Shrink the hierarchical JSON dump to a human-readable size (~100–200 lines for the two-button example) by pruning noise (renderer/metrics/trellis mirrors, diagnostics, opaque blobs) while keeping the scene/window/widget structure. Use `out.json` from the button example as the baseline to slim.

## Baseline (out.json, ~3156 lines)
- Contains renderer/targets/hints/diagnostics under `renderers/widgets_declarative_renderer/...`
- Contains runtime/trellis revision history and control paths
- Stores opaque `WidgetTheme` blob (`/config/theme/sunset/value`) and only the active theme name “sunset”
- Includes diagnostic strings (“InputTask mailbox subscriptions missing…”) and various meta nodes
- Per-node diagnostics already removed, but subtree content is still large

## Target output (slim mode)
- Hierarchical JSON
- Only keep:
  - App root
  - `windows/<window>` → `views/<view>` → `scene`, `surface`
  - Widget subtree under `views/<view>/widgets/...` (stack + two buttons)
  - Active theme string (`/config/theme/active` and window `style/theme` if present)
- Exclude by default:
  - Renderer/targets/metrics/present diagnostics
  - Runtime/trellis mirrors and revision history
  - Opaque blobs (e.g., `WidgetTheme`); if needed, replace with a short placeholder or skip
  - Informational/diagnostic strings unrelated to structure
- Defaults: `include_nested_spaces=false`, diagnostics off, depth ~6, children ~64, root set to app root

## Work items
1) Exporter filtering
   - Add optional include/exclude path filters (prefix-based) and apply defaults to drop renderer, runtime/trellis, metrics, and diagnostics paths in slim mode.
   - Skip opaque blobs unless a converter exists; for themes, keep only the active name string.
2) Exporter options/preset
   - Add a “slim” preset (used by the button example dump): root=app, no nested spaces, no diagnostics, no opaque placeholders, filtered paths as above.
   - Keep a “full”/debug preset to allow complete dumps when needed.
3) Example integration
   - `--dump_json` uses the slim preset by default.
   - Provide `--dump_json_full` (or env) to bypass filters if full dump is desired.
4) Tests
   - Slim dump test: run the button scene once, export slim JSON, assert completion <5s and line count below a threshold (e.g., <400) and presence of window/scene/widget nodes.
   - Full dump test (optional): ensure full mode still works and contains renderer/runtime nodes.
5) Docs
   - Update exporter docs and `WidgetDeclarativeAPI.md` to describe slim vs full, defaults, and flags; mention `out.json` as the pre-slim baseline.
6) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20`.

## Status (2025-12-27)
- Implemented include/exclude prefix filters with wildcard components and a new `Mode::Slim` preset. Slim defaults keep app/config/theme/active + window/view {scene,surface,widgets} while dropping renderer/trellis/diagnostics paths and theme blobs; meta now records filter lists.
- `declarative_button_example --dump_json` now uses the slim preset (~258 lines for the two-button run). `--dump_json_full` disables filters (minimal view) and `--dump_json_debug` keeps the verbose export. `pathspace_dump_json` gained `--slim` plus repeatable `--include-prefix/--exclude-prefix` flags; fixture refreshed for the added meta flags.
- Tests updated: declarative button dump asserts slim mode/line budget/noisy-path pruning; exporter unit test covers include/exclude filters.
- Docs refreshed: `AI_ARCHITECTURE.md` documents Minimal/Slim/Debug presets and CLI filters; `WidgetDeclarativeAPI.md` records the slim button dump shrink from the ~3156-line `out.json` baseline to ~250 lines. Added Memory entry.
- Validation: `ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20` passes every test except `PathSpaceUITests_All`, which times out at 20s because `/bin/ps` is blocked in this sandbox. A rerun of just that test reproduces the timeout; all other suites (including `DeclarativeButtonJsonDump`) pass.
## Acceptance criteria
- `./build/declarative_button_example --dump_json` (slim mode) produces a hierarchical JSON focused on window/scene/widgets, under ~400 lines, no renderer/trellis/diagnostic clutter.
- Full mode remains available via explicit flag.
- Dumps complete without hang/crash.
- Tests for slim (and full, if kept) pass; full looped suite green.
