# Plan: Declarative Screenshot Helper API

## Motivation
- `examples/paint_example_new.cpp` and sibling samples currently spend ~150 LOC preparing screenshots (scene readiness, force-publish, request wiring, telemetry logging). This boilerplate obscures the declarative quickstart the examples are meant to showcase.
- The low-level `SP::UI::Screenshot::ScreenshotService::Capture` API forces every caller to replicate identical readiness + capture logic. As more demos, tests, and automation hooks (e.g., `pathspace_screenshot_cli`) adopt screenshots, the duplication becomes a maintenance risk.
- We need a first-class helper that turns “capture a declarative scene to PNG” into a single call, while still allowing advanced callers to override timeouts/layout.

## Objectives
1. Design and implement a high-level screenshot helper (`CaptureDeclarative` working name) that:
   - Accepts a `ScenePath`, `WindowPath`, and concise `ScreenshotOptions`.
   - Performs declarative readiness checks (widgets, lifecycle metrics, runtime health) with sane defaults.
   - Optionally forces a publish and waits for the resulting revision before capturing.
   - Internally calls `ScreenshotService::Capture` and returns its `ScreenshotResult` / `SP::Expected<void>`.
2. Update the declarative samples (`examples/paint_example_new.cpp`, `examples/paint_example.cpp`, `pathspace_screenshot_cli`, future tests) to use the helper so screenshot code shrinks to ≤ 10 LOC per caller.
3. Document the new API in `docs/WidgetDeclarativeAPI.md` (Screenshots section) and reference it from onboarding/quickstart docs.
4. Extend the test suite to cover the helper: positive capture, forced publish path, error propagation (missing window/view, failing readiness, etc.).

## Non-Objectives
- Rewriting the underlying `ScreenshotService` capture pipeline (IOSurface readback, PNG encoding). We only wrap it.
- Changing the paint example’s scripted stroke logic or baseline manifest.
- Solving headless GPU presenter issues beyond wiring `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE` through the helper.

## Current State Snapshot
- `ScreenshotService::Capture(ScreenshotRequest)` expects callers to populate every field, including telemetry namespace and readiness gating.
- `PathSpaceExamples::ensure_declarative_scene_ready` already packages readiness logic, but each example wires it manually.
- `paint_example_new.cpp` and `paint_example.cpp` replicate: readiness → force publish → wait for revision → create request → capture → log result.
- Tests (`PaintExampleScreenshot*`, `PathSpaceUITests` pointer flows) call into the samples, so the screenshot boilerplate impacts CI readability.

## Proposed API Surface
```cpp
namespace SP::UI::Screenshot {

struct DeclarativeScreenshotOptions {
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::filesystem::path> output_png;
    std::optional<std::filesystem::path> compare_png;
    std::optional<std::filesystem::path> diff_png;
    std::optional<std::filesystem::path> metrics_json;
    double max_mean_error = 0.0015;
    bool require_present = false;
    bool force_publish = true;
    std::chrono::milliseconds readiness_timeout{3000};
    bool wait_for_runtime_metrics = true;
    std::string telemetry_namespace; // auto-filled if empty
};

auto CaptureDeclarative(SP::PathSpace& space,
                        SP::UI::ScenePath const& scene,
                        SP::UI::WindowPath const& window,
                        DeclarativeScreenshotOptions const& options = {})
    -> SP::Expected<ScreenshotResult>;
```
- Helper responsibilities: call `ensure_declarative_scene_ready`, optionally `SceneLifecycle::ForcePublish`, compute target width/height from surface desc, fill `ScreenshotRequest`, call `ScreenshotService::Capture`, and propagate logs/errors.

## Workstreams
1. **API & Plumbing** — ✅ November 28, 2025
   - `DeclarativeScreenshotOptions` + `CaptureDeclarative` now live in
     `src/pathspace/ui/screenshot/DeclarativeScreenshot.{hpp,cpp}`. The helper
     wraps readiness, force-publish, surface-dimension discovery, telemetry
     defaults, and delegates to `ScreenshotService::Capture` with optional
     hooks.
   - Readiness utilities (`DeclarativeReadinessOptions`,
     `ensure_declarative_scene_ready`, bucket/revision waits, manual pump
     metrics) moved from `examples/declarative_example_shared.hpp` into the new
     `src/pathspace/ui/declarative/SceneReadiness.{hpp,cpp}` module so library
     code can reuse them without including the example header. The header keeps
     inline wrappers for legacy call sites.
   - `CaptureDeclarative` derives `telemetry_namespace` from the app component
     portion of the window path when the caller leaves it empty and accepts
     optional overrides for telemetry root, screenshot hooks, and readiness
     options.

2. **Callsite Updates** — ✅ November 28, 2025
   - `examples/paint_example.cpp`, `examples/paint_example_new.cpp`, and
     `pathspace_screenshot_cli` ride the helper already. The declarative gallery
     (`examples/widgets_example.cpp`) and devices demo (`examples/devices_example.cpp`)
     now share a headless CLI implemented in
     `<pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>` that parses
     `--screenshot*` flags, resolves telemetry defaults, and calls
     `CaptureDeclarative` through a single helper.
   - `examples/declarative_hello_example.cpp` intentionally remains the literal
     quickstart (no CLI, no screenshot automation) per the onboarding docs, so
     new contributors see the smallest possible declarative app without extra
     flags.
   - The demos that need screenshots can now capture a PNG (with optional
     compare/diff/metrics arguments) in ≤10 LOC, and the shared helper enforces
     readiness + telemetry consistency automatically.

3. **Testing**
   - Add targeted unit/UITest coverage: e.g., `tests/ui/test_ScreenshotHelper.cpp` exercising success, readiness timeout, forced publish error.
   - Update `PaintExampleScreenshot*` CTests to validate they still pass (loop harness already covers them).

4. **Documentation**
   - Update `docs/WidgetDeclarativeAPI.md` (New section: “Capturing Screenshots”) with sample code showing the one-call helper.
   - Mention the helper in `docs/AI_Onboarding.md` and `docs/Memory.md` for future maintainers.

## Validation Plan
- Local runs: `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1 ./scripts/compile.sh --clean --test --loop=5 --release` after callsite changes.
- Manual smoke: `./build/paint_example_new --screenshot out.png --screenshot-compare docs/images/paint_example_baseline.png` to ensure CLI options flow correctly.
- CI: rely on existing screenshot CTests plus new helper-centric test.

### Status (November 28, 2025)
- ✅ `CaptureDeclarative` helper landed with telemetry defaults, readiness plumbing, and optional screenshot hooks.
- ✅ `examples/paint_example.cpp`, `examples/paint_example_new.cpp`, and any consumers that route through `PathSpaceExamples::RunPaintExample` now invoke the helper instead of recreating readiness/force-publish logic inline.
- ✅ Declarative demos now expose shared screenshot CLIs where needed:
  `widgets_example` and `devices_example --paint-controls-demo` both parse the
  standard `--screenshot*` flags and call `CaptureDeclarative` through the
  helper, while `declarative_hello_example` intentionally remains the
  screenshot-free quickstart sample referenced in the docs.
- 🔄 Remaining follow-ups: land the focused helper test suite and document the
  shared CLI helper in the migration tracker/Widget API guide (docs work is
  underway here, but the dedicated UITest still needs to be written).

## Risks & Mitigations
- **Readiness regressions**: consolidate readiness logic into one helper to avoid divergence; keep `DeclarativeReadinessOptions` override hooks exposed via `DeclarativeScreenshotOptions` if specialized tests need them.
- **API surface creep**: keep options minimal; direct callers needing bespoke flows can still call `ScreenshotService::Capture`.
- **Telemetry drift**: ensure helper defaults are documented and optionally allow overriding `telemetry_namespace`.

## Deliverables & Timeline
1. Helper implementation + unit test scaffold (1 day).
2. Update samples/CLI/tests to the helper (1–2 days depending on diff size).
3. Documentation refresh + Memory entry (0.5 day).
4. Final validation via looped tests + screenshot CTests (0.5 day).

Once complete, all declarative screenshot consumers collapse to a few lines of code, fulfilling the “simple example” requirement without sacrificing diagnostics.
