# Plan: Reliable JSON Dump for Declarative Button Example

## Status (December 27, 2025)
- Completed: `--dump_json` now runs a single `RunUI` frame (`run_once=true`) after forcing a scene publish and pinning the window to the software renderer. It then shuts down the declarative runtimes and exports PathSpace JSON rooted at `/system/applications/declarative_button_example` with values/diagnostics enabled, `max_children=128`, `max_depth=12`, and `max_queue_entries=4`.
- Export happens post-shutdown to avoid runtime locks; readiness uses `force_scene_publish` to materialize buckets without entering the long UI loop.
- Regression test added: `DeclarativeButtonJsonDump` (python) invokes the example in dump mode, enforces a <5 s runtime, and asserts the JSON contains the app, window, scene, stack, and hello/goodbye button nodes.
- Docs updated (`WidgetDeclarativeAPI.md`) with the dump workflow (one-frame present, shutdown, export options).
- Validation: PathSpaceTests passed 5× manual loop. PathSpaceUITests 5× manual loop hit the pre-existing `test_DeclarativePaintSurface.cpp` flake (`pending->empty()` check) in 2/5 runs (logs: `build/test-logs/PathSpaceUITests_manual_loop2of5_20251227-014104.log`, `_loop3of5_20251227-014129.log`); a subsequent single run succeeded.

## Goal
Make `./build/declarative_button_example --dump_json` reliably emit the full scene state at program exit without hanging or crashing, even when UI runtimes are active. The dump should reflect the final rendered scene (after at least one present) and the change must pass the standard test loop.

## Current symptoms
- `--dump_json` often hangs or times out.
- When it returns, it can crash with `bad_expected_access` or produce an empty/partial dump because values were read with `.value()` when setup failed.
- Dump happens while runtimes are still live, so `PathSpaceJsonExporter::Export` can block on runtime locks.

## Approach
1) **Deterministic one-frame flow for dump**  
   - When `--dump_json` is set (and no screenshot), drive exactly one present to materialize drawables, then stop UI runtimes and export JSON.
   - Avoid entering the long-running `RunUI` loop for dump-only mode.
2) **Exporter safety with live runtimes**  
   - Either stop declarative runtimes before export or snapshot via a read-only/export-safe API that does not wait on runtime locks. Prefer stopping runtimes for the example path.
3) **Error-safe setup**  
   - Replace `.value()` usage in the example with checked `Expected` handling; bail with a clear message on failure.
4) **Test coverage**  
   - Add a test that runs the button example in dump mode (single frame), asserts it finishes <5s, and that the JSON contains expected nodes (window, scene, stack/buttons).
   - Keep existing screenshot tests intact.
5) **Docs**  
   - Document the dump workflow in `WidgetDeclarativeAPI.md` (flags, one-frame present, shutdown-then-export).
6) **Validation**  
   - Run `./scripts/compile.sh --loop=5 --timeout=20` after changes.

## Detailed work items
1) Example changes (button):
   - On `--dump_json` (no screenshot):
     - Build present handles; if missing, log and exit nonzero.
     - Call `PresentWindowFrame` once to materialize the scene.
     - Immediately `ShutdownDeclarativeRuntime`.
     - Call `PathSpaceJsonExporter::Export` with options `{include_values=true, include_diagnostics=true, include_nested_spaces=true, include_opaque_placeholders=true, max_children=256, max_queue_entries=4}` and print.
   - Screenshot/no flags: keep `RunUI`; `run_once` only for `--screenshot_exit`.
   - Replace all `.value()` reads (`surface`, `resolve_app_relative`, `SetScene`, `Stack::Create`) with checked `Expected` handling and early return on error.
2) Exporter safety:
   - Verify `PathSpaceJsonExporter` does not depend on running trellises; if it can block on live runtimes, always export **after** `ShutdownDeclarativeRuntime` in the example path.
   - If needed, add a `PathSpaceJsonOptions` preset for “safe_dump” to cap traversal and avoid long waits.
3) Tests:
   - Add a unit/integration test that invokes the button example (or a harness) in dump mode, ensures completion <5s, and asserts JSON contains keys for `/system/applications/declarative_button_example`, window, scene, and stack/button nodes.
   - Keep existing screenshot tests unchanged; add a dump-mode regression to UITests if lightweight.
4) Docs:
   - Update `WidgetDeclarativeAPI.md` with the exact `--dump_json` flow (one present, shutdown, export), and note that errors are logged and exit code is nonzero on failure.
5) Validation:
   - Run `./scripts/compile.sh --loop=5 --timeout=20`; fix any regressions in UITests/PathSpaceTests.

## Success criteria
- `./build/declarative_button_example --dump_json` completes without hang/crash and emits JSON containing window/scene/stack/button nodes.
- Screenshot path remains single-call + UI run; behavior unchanged.
- Tests for dump mode pass; full looped suite green.
- No `.value()`-driven crashes remain in the example path.

## Remaining TODOs
- Track the flaky `test_DeclarativePaintSurface.cpp` pending-queue failure noted during the 5× PathSpaceUITests manual loop; rerun the loop once the upstream fix lands.
