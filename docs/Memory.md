## Repository Memory
This file is for remembering architecture details about the PathSPace project so it becomes easier to get up to speed about details later.
No need to store details about a worklog of dates when things where added.

- 2025-11-18: WidgetEventTrellis mutates declarative widget state (button hover/press, toggle checked, slider value/dragging, list hover/selection, tree hover/expanded) and flips `render/dirty` before emitting each `WidgetOp`, so declarative apps stay in sync without handlers touching `widgets/<id>/state/*`.
- 2025-11-18: Keyboard + gamepad events now flow through the declarative dispatcher (Tab/D-pad shoulders call `Widgets::Focus::Move`, arrows/D-pad feed slider/list/tree ops, and Space/Enter/A emit the same press/toggle/list-activate ops as mouse input) and InputTask exposes per-widget handler telemetry at `widgets/<id>/metrics/handlers/{invoked_total,failures_total,missing_total}` to pinpoint flaky/missing callbacks.
- 2025-11-18: Declarative paint surfaces persist `state/history/<stroke-id>/{meta,points}`, track `render/buffer/revision`, and synthesize stroke buckets so SceneLifecycle no longer needs opaque render lambdas; GPU uploads remain a TODO.
- 2025-11-18: Declarative paint-surface tests must shut down the runtime after launching (`RuntimeGuard`) to avoid leaving worker threads around for subsequent suites.
