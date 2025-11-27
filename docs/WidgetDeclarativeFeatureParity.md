# Widget Declarative Feature Parity

> **Updated:** November 25, 2025 — baseline audit for `Plan_WidgetDeclarativeAPI` Phase 3.

This document cross-references the legacy imperative widget builders (`src/pathspace/ui/WidgetBindings*.cpp`, `src/pathspace/ui/WidgetReducers*.cpp`, legacy samples) with the declarative runtime introduced in 2025 (`src/pathspace/ui/declarative/**`, updated samples, and the new runtime workers). Use it to spot gaps before deprecating the legacy builders. Source references point to the files or plan sections that guarantee each capability.

## 1. Interaction Coverage

| Feature | Legacy builders | Declarative runtime | Status / Notes |
| --- | --- | --- | --- |
| Buttons & toggles (pointer + keyboard/gamepad) | `WidgetBindings.cpp`, `WidgetReducers.cpp`, and `tests/ui/test_Builders.cpp` cover hover/press/activate plus Space/Enter fallbacks. | `CreateWidgetEventTrellis` + `CreateInputTask` (docs/Plan_WidgetDeclarativeAPI.md:242-255) mutate state, mirror ops into queues, and invoke handlers; keyboard/gamepad parity is regression-tested in `tests/ui/test_WidgetEventTrellis.cpp`. | ✅ Parity; handler bindings moved to `HandlerBinding` registry with telemetry. |
| Sliders | Legacy reducers map pointer drags + arrow keys to value changes (`WidgetReducers_Slider.cpp`). | Trellis emits `SliderBegin/Update/Commit`, updates state/value/dragging, and feeds handlers; keyboard/gamepad steps covered in `tests/ui/test_WidgetEventTrellis.cpp` (docs/Plan_WidgetDeclarativeAPI.md:251-254). | ✅ Parity including discrete step handling and dirty rect propagation. |
| Lists | `WidgetBindings_List.cpp` + UI tests manage hover/select/activate with reducer mirroring. | Declarative trellis mirrors hover/select/activate before handler dispatch (`docs/Plan_WidgetDeclarativeAPI.md:251-254`); `tests/ui/test_WidgetEventTrellis.cpp` asserts keyboard navigation + Enter activation. | ✅ Parity with handler queues and telemetry. |
| Trees | Legacy tree reducers manage expand/collapse/select (`WidgetReducers_Tree.cpp`). | Declarative worker now mutates hover/expanded state, emits `TreeToggle/Select`, and handles keyboard + D-pad actions (docs/Plan_WidgetDeclarativeAPI.md:251-254). | ✅ Parity; tests cover pointer + keyboard paths. |
| Stack / gallery panels | Imperative stacks rely on `WidgetDrawablesDetailStack` to highlight active panels. | Declarative descriptors persist `layout/{style,children,computed}` and trellis emits `StackSelect` with handler routing (docs/Plan_WidgetDeclarativeAPI.md:219-220,255-260). | ✅ Active panel highlighting, handler dispatch, and layout metadata match legacy behaviour. |
| Input fields | Legacy text widgets translate pointer/keyboard into reducer ops (`WidgetReducers_TextInput.cpp`). | Trellis synthesizes `TextDelete`, `TextMoveCursor`, `TextSubmit`, and mirrors them into queues before handlers (docs/Plan_WidgetDeclarativeAPI.md:252-254); regression coverage lives in `tests/ui/test_WidgetEventTrellis.cpp`. | ✅ Parity for focus, cursor, delete, and submit flows. |
| Paint surface & drawing history | Legacy paint builder wired pointer ops into `PaintSurfaceRuntime` plus `UndoableSpace`. | Declarative `PaintSurface::Create`, `PaintControls`, and `PaintSurfaceRuntime` record strokes (`state/history/*`), maintain versions, rasterize to `assets/texture`, and stream metrics/logs (docs/Plan_WidgetDeclarativeAPI.md:170-183,257-270; docs/Memory.md:5-41). | ✅ Pointer/GPU parity plus undo/redo integration; screenshot + diagnostics harness guard regressions. |

## 2. Rendering, Focus, and State Management

| Area | Legacy builders | Declarative runtime | Status / Notes |
| --- | --- | --- | --- |
| Scene lifecycle & cached buckets | `SceneSnapshotBuilder` consumed legacy `DrawableBucketSnapshot`s per widget. | `SceneLifecycle` trellis rebuilds buckets from descriptors, caches them under `scene/structure/widgets/*`, and publishes revisions/metrics (`docs/Plan_WidgetDeclarativeAPI.md:234-269`). | ✅ Same publish pipeline with telemetry; dirty widgets notify lifecycle worker instead of bespoke reducers. |
| Focus controller | Imperative apps maintained focus order lists manually. | Declarative runtime computes depth-first `focus/order`, mirrors `focus/current`, and exposes `Widgets::Focus::Move` so keyboard/gamepad traversal is automatic (`docs/Plan_WidgetDeclarativeAPI.md:237-241`). | ✅ Parity plus per-widget focus metrics; multi-window focus mirrors under `structure/window/<id>/focus/current`. |
| Theme resolution/editing | Legacy builders embedded theme data per widget. | Declarative descriptors walk widget → window → app theme, honor inheritance chains, and `Theme::{Create,SetColor,RebuildValue}` keep serialized values + invalidations in sync (docs/Plan_WidgetDeclarativeAPI.md:273-280; docs/Widget_Schema_Reference.md:27-50). | ✅ Declarative flow now matches (and extends) legacy behaviour with tooling/tests. |
| Handler registration & telemetry | Legacy stored `std::function` payloads directly, making persistence opaque. | Declarative `HandlerBinding` registry stores `{registry_key, kind}` (docs/Plan_WidgetDeclarativeAPI_EventRouting.md:5-31), mirrors enqueue/drop counts (`/system/widgets/runtime/input/metrics/*`), and exposes per-widget handler logs (`widgets/<id>/metrics/handlers/*`). | ✅ Richer telemetry plus legacy-equivalent execution semantics. |
| History binding | Legacy samples hand-wired `UndoableSpace` aliases per widget. | Declarative `HistoryBinding` helper seeds metrics, buttons, and telemetry cards for paint/history-aware widgets (docs/Memory.md:21-24). | ✅ Declarative paint sample + helpers deliver the same undo stack semantics without bespoke glue. |

## 3. Tooling, Samples, and Diagnostics

| Capability | Legacy builders | Declarative runtime | Status / Notes |
| --- | --- | --- | --- |
| Sample coverage | Legacy `widgets_example`/`paint_example` relied on imperative builders. | Updated samples (`examples/widgets_example*.cpp`, `examples/paint_example*.cpp`, `examples/declarative_hello_example.cpp`, `examples/declarative_theme_example.cpp`) all launch through `SP::System::LaunchStandard` and declarative widgets (docs/Plan_WidgetDeclarativeAPI.md:257-287; docs/Memory.md:5-34). | ✅ Every teaching sample now uses the declarative API, showing parity in practice. |
| Screenshot & readiness automation | Legacy paint screenshot tooling depended on bespoke scripts. | Declarative samples gate on `PathSpaceExamples::ensure_declarative_scene_ready`, run screenshot capture through `SP::UI::Screenshot::ScreenshotService`, and store manifests/baselines (`docs/Memory.md:5-41`). | ✅ Deterministic capture/diff workflow; CI harnesses enforce visual parity. |
| Telemetry & diagnostics | Legacy widgets emitted limited per-widget metrics. | Declarative runtime publishes schema/focus/input/render telemetry plus per-widget logs (`docs/Plan_WidgetDeclarativeAPI.md:242-272`, `docs/Plan_WidgetDeclarativeAPI_EventRouting.md:5-62`). | ✅ Broader coverage (handlers, focus, lifecycle, GPU uploads) and shared logging roots. |

The deterministic `widget_pipeline_benchmark` (`benchmarks/ui/widget_pipeline_benchmark.cpp`) now measures the declarative pipeline exclusively—mutations, bucket synthesis, and paint GPU uploads—and feeds the `widget_pipeline` scenario in `scripts/perf_guardrail.py`. The captured tolerances live in `docs/perf/performance_baseline.json`, so the pre-push guardrail halts declarative regressions automatically without referencing legacy builders.

Declarative loops, screenshot capture, and `SP::App::RunUI` now rely on
`SP::UI::Declarative::{BuildPresentHandles,ResizePresentSurface,PresentWindowFrame,PresentFrameToLocalWindow}`
for presenter plumbing. Those wrappers keep the renderer/surface/target wiring
inside the declarative runtime, so top-level code and helpers no longer include
`<pathspace/ui/BuildersShared.hpp>` just to resize a surface or display a frame.

## 4. Outstanding Gaps / Follow-ups

1. **Inspector/consumer migration tracking:** Status now lives in `docs/WidgetDeclarativeMigrationTracker.md`. Keep that file current (owners, telemetry, verification dates) so we can see exactly which inspector/web/consumer surfaces still depend on legacy builders.
2. **Legacy builder telemetry:** `/_system/diagnostics/legacy_widget_builders/<entry>/usage_total` must stay at zero before we enable `PATHSPACE_LEGACY_WIDGET_BUILDERS=error` by default. Keep CI scraping these counters and record any non-zero usage in both the next status update and the migration tracker so the support-window timeline stays credible.

The matrix above should be refreshed whenever new widgets land or when telemetry/worker contracts change. Update this file and the Phase 3 checklist inside `docs/Plan_WidgetDeclarativeAPI.md` together to keep planning artifacts in sync.
