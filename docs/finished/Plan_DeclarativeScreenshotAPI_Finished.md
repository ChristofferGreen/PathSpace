# Plan: Declarative Screenshot Helper API

## Motivation
- `examples/paint_example_new.cpp` and sibling samples currently spend ~150 LOC preparing screenshots (scene readiness, force-publish, request wiring). This boilerplate obscures the declarative quickstart the examples are meant to showcase.
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
- `ScreenshotService::Capture(ScreenshotRequest)` expects callers to populate every field, including readiness gating.
- `PathSpaceExamples::ensure_declarative_scene_ready` already packages readiness logic, but each example wires it manually.
- `paint_example_new.cpp` and `paint_example.cpp` replicate: readiness → force publish → wait for revision → create request → capture → log result.
- Tests (`PaintExampleScreenshot*`, `PathSpaceUITests` pointer flows) call into the samples, so the screenshot boilerplate impacts CI readability.

## Top Priority: Unblock Headless Declarative Captures
The env-driven screenshot hooks in the tiny declarative samples (e.g., `declarative_button_example`) currently emit the deterministic fallback PNG instead of the live UI because no framebuffer ever exists when `CaptureDeclarative` runs. To restore determinism we need the helper to guarantee that a framebuffer is produced (hardware or software) before it calls `ScreenshotService`.

### Action Plan
1. **Always enable framebuffer capture before screenshot mode**
   - `CaptureDeclarative` (or the shared helper it uses) must set `/present/params/capture_framebuffer=true` before invoking `ScreenshotService::Capture`. Today the responsibility is on callers, so small examples forget to toggle it and the software fallback path has nothing to read.
   - Add a helper (`EnsureFramebufferCaptureEnabled`) under `src/pathspace/ui/declarative/Runtime.cpp` so both CLI helpers and env flows can reuse the same code.

2. **Produce at least one present frame before capture**
   - Extend `CaptureDeclarative` with an optional `PresentBeforeCapture` step that issues `PresentWindowFrame` once (respecting timeouts) to seed the `/output/v1/software/framebuffer` queue. When screenshots run headless, there is no loop to drive presents, so without this step `Window::Present` just times out.
   - If `PresentWindowFrame` fails, fall back to the deterministic PNG but surface a clear error so we know captures are relying on the fallback.

3. **Update minimal examples + docs**
   - Switch `examples/declarative_button_example.cpp` (and friends) to rely on the improved helper instead of manually toggling capture flags.
   - Document the requirement in `docs/WidgetDeclarativeAPI.md` so future samples know the canonical flow.

4. **Test coverage**
   - Add a unit/functional test that boots a tiny declarative scene, requests `--screenshot`, and asserts the PNG contains >N unique colors. This guards against regressions where we silently fall back to the deterministic art again.

This headless-capture fix is the top priority for the plan until the minimal example and screenshot CLI produce live UIs without resorting to the fallback renderer.

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
     wraps readiness, force-publish, surface-dimension discovery, and delegates
     to `ScreenshotService::Capture` without needing per-call telemetry or hook
     plumbing.
   - Readiness utilities (`DeclarativeReadinessOptions`,
     `ensure_declarative_scene_ready`, bucket/revision waits, manual pump
     metrics) moved from `examples/declarative_example_shared.hpp` into the new
     `src/pathspace/ui/declarative/SceneReadiness.{hpp,cpp}` module so library
     code can reuse them without including the example header. The header keeps
     inline wrappers for legacy call sites.

2. **Callsite Updates** — ✅ November 28, 2025
   - `examples/paint_example.cpp`, `examples/paint_example_new.cpp`, and
     `pathspace_screenshot_cli` ride the helper already. The declarative gallery
     (`examples/widgets_example.cpp`) and devices demo (`examples/devices_example.cpp`)
     now share a headless CLI implemented in
     `<pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>` that parses
     `--screenshot*` flags and calls `CaptureDeclarative` through a single helper.
   - `examples/declarative_hello_example.cpp` stays the literal quickstart
     (still no CLI parsing) so new contributors see the smallest declarative
     app, but it now honors `PATHSPACE_HELLO_SCREENSHOT=<png>` (plus the optional
     `PATHSPACE_HELLO_SCREENSHOT_FORCE_SOFTWARE=1`) and funnels the request
     through `CaptureDeclarative` to dump the reference PNG after readiness. When
     `Window::Present` cannot deliver a framebuffer, the env hook renders a
     deterministic fallback PNG so docs always have a screenshot to reference.
   - The demos that need screenshots can now capture a PNG (with optional
     compare/diff/metrics arguments) in ≤10 LOC, and the shared helper enforces
     readiness consistently.

3. **Testing** — ✅ December 2, 2025
   - `tests/ui/test_DeclarativeScreenshotHelper.cpp` exercises `CaptureDeclarative` end-to-end: it captures a software framebuffer and asserts the PNG contains multiple colors, forces a readiness timeout via an invalid window-component override, and stops the scene lifecycle to verify force-publish errors bubble up.
   - `PaintExampleScreenshot*` CTests remain in the compile loop to make sure the screenshot CLI stays green alongside the helper-specific coverage.

4. **Documentation** — ✅ November 28, 2025
   - `docs/WidgetDeclarativeAPI.md`, `docs/AI_Debugging_Playbook.md`, and
     `docs/AI_Onboarding_Next.md` now describe the shared CLI helper plus the
     env-driven capture hook in `declarative_hello_example`.
   - `docs/Memory.md` tracks the rollout so future maintainers can discover the
     helper without chasing commit history.

## Validation Plan
- Local runs: `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1 ./scripts/compile.sh --clean --test --loop=5 --release` after callsite changes.
- Manual smoke: `./build/paint_example_new --screenshot out.png --screenshot-compare docs/images/paint_example_baseline.png` to ensure CLI options flow correctly.
- CI: rely on existing screenshot CTests plus new helper-centric test.

### Status (November 28, 2025)
- ✅ `CaptureDeclarative` helper landed with readiness plumbing.
- ✅ `examples/paint_example.cpp`, `examples/paint_example_new.cpp`, and any consumers that route through `PathSpaceExamples::RunPaintExample` now invoke the helper instead of recreating readiness/force-publish logic inline.
- ✅ Declarative demos now expose shared screenshot CLIs where needed:
  `widgets_example` and `devices_example --paint-controls-demo` both parse the
  standard `--screenshot*` flags and call `CaptureDeclarative` through the
  helper, while `declarative_hello_example` remains CLI-free but now honors the
  `PATHSPACE_HELLO_SCREENSHOT` env var (complete with a deterministic fallback)
  to route through the same helper for the quickstart PNG. The hello sample is
  intentionally minimal again—it only mounts a single button so the docs can
  point at the smallest possible declarative program.
- ✅ Declarative presents re-read `/present/params/*` (including
  `capture_framebuffer`) and `ScreenshotService::Capture` always runs a present
  pass—even when `force_software` is set—so env-driven captures now emit the
  real framebuffer instead of falling back to the placeholder PNG. The
  deterministic fallback still triggers only when both hardware and software
  readbacks fail.
- ✅ `CaptureDeclarative` now reuses the framebuffer from the warm-up
  `PresentWindowFrame` when `present_before_capture` is enabled, passing those
  pixels directly into `ScreenshotService`. This removes the redundant second
  render path, so screenshot PNGs represent the exact “next” frame that the UI
  would show instead of diverging in theme or state.
- ✅ 2025-12-26 update: the default helper path no longer forces a present;
  with `present_before_capture=false` it reuses the latest cached framebuffer
  (and errors clearly when none is available) while callers can still opt into
  the warm-up-present flow when they need a fresh frame.
- See also: `docs/finished/Plan_DeclarativeScreenshot_NoPresent_Finished.md` for the cached-frame default follow-up.
- ✅ Helper test suite landed: `tests/ui/test_DeclarativeScreenshotHelper.cpp` covers
  live framebuffer captures (unique color threshold), readiness timeouts via
  bogus window-component overrides, and force-publish failures after shutting
  down the scene lifecycle; screenshot CTests continue to run in the compile
  loop for CLI parity.

## Risks & Mitigations
- **Readiness regressions**: consolidate readiness logic into one helper to avoid divergence; keep `DeclarativeReadinessOptions` override hooks exposed via `DeclarativeScreenshotOptions` if specialized tests need them.
- **API surface creep**: keep options minimal; direct callers needing bespoke flows can still call `ScreenshotService::Capture`.
- **Readiness drift**: ensure helper defaults stay in sync with declarative lifecycle changes.

## Deliverables & Timeline
1. Helper implementation + unit test scaffold (1 day).
2. Update samples/CLI/tests to the helper (1–2 days depending on diff size).
3. Documentation refresh + Memory entry (0.5 day).
4. Final validation via looped tests + screenshot CTests (0.5 day).

Once complete, all declarative screenshot consumers collapse to a few lines of code, fulfilling the “simple example” requirement without sacrificing diagnostics.
