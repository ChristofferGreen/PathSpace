# Plan: Declarative Screenshot Single-Call API

## Goal
Expose declarative screenshots as a single public call while keeping the slot/token machinery internal, and update the button example to use the one-liner API.

## Scope
- Public surface: keep `CaptureDeclarative(...)` as the primary entry; add a convenience wrapper (`CaptureDeclarativeSimple`) that takes only space/scene/window/output (+optional size) and applies sane defaults.
- Internals: wrapper still uses the slot/token path under the hood; no new caller-facing primitives.
- Examples: rewrite `examples/declarative_button_example.cpp` to use the simple one-liner; remove legacy flag wiring and direct option construction.

## Work items
1) API: Add `CaptureDeclarativeSimple` (or similar) in `DeclarativeScreenshot.hpp/.cpp`:
   - Params: `PathSpace&`, `ScenePath`, `WindowPath`, `std::filesystem::path output_png`, optional `width/height`.
   - Defaults: `capture_mode=next_present`, `require_present=true`, `present_before_capture=true`, `allow_software_fallback=true`, `force_software=false`, token/slot timeouts as today.
   - Returns `Expected<ScreenshotResult>`.
2) Button example: rewrite to call the simple API; drop bespoke CLI flag parsing and manual option setup. Keep `--screenshot` path + `--screenshot_exit`, but route through the one-liner.
3) Docs: update `WidgetDeclarativeAPI.md` to mention the simple call and clarify that the slot/token path is internal.
4) Tests: extend `tests/ui/test_DeclarativeScreenshot.cpp` with a simple-call invocation to ensure it exercises the slot path and returns the same hash as the advanced call.
5) Validation: run the required looped suite (`./scripts/compile.sh --loop=5 --timeout=20` or current requirement) after changes.

## Success criteria
- Public usage reduced to a single function call for common cases (documented).
- Button example uses only the simple call; no direct `CaptureDeclarativeOptions` wiring.
- Tests cover the simple call; hashes stay stable.
- Full test loop passes.
