# Plan: IO Pump & Input Trellis (Finished)

> **Status (November 17, 2025):** Plan completed after landing telemetry/throttling controls (Phase 4). Future updates should reference this document for historical context and use the runtime APIs described below.

## Motivation
- Today each sample app (or the macOS window bridge) owns an infinite loop that polls PathIO providers and hand-wires hit testing. Declarative widgets never see input unless every app reimplements that plumbing.
- PathSpace’s design goal is to merge heterogeneous streams into a coherent pipeline using Trellis, but device events currently bypass Trellis entirely.
- We need a reusable runtime component that turns OS/device feeds into canonical declarative events without forcing every app to write custom loops.

## Objectives
1. Introduce a PathSpace-managed **IO Trellis** that normalizes mouse, keyboard, and gamepad events into shared canonical queues.
2. Provide a lightweight **IO Pump** entry point that keeps those queues flowing (blocking on the Trellis outputs and forwarding events downstream).
3. Feed a declarative **routing Trellis** that performs scene hit testing, emits `WidgetOp`s, and fans events to reducers/diagnostics/accessibility.
4. Add opt-in telemetry & throttling so providers can push without overwhelming slow consumers.
5. Document the pipeline and surface configuration hooks (per-provider throttles, telemetry toggles, launch options).

## Non-Objectives
- Replacing the existing declarative reducers/handler bindings (those arrive in Phase 1 item 5 of `Plan_WidgetDeclarativeAPI`).
- Rewriting PathIO providers to depend on platform-specific event loops (they still expose standard `insert`/`take`).
- Introducing new input device kinds beyond pointer/text/gamepad (those can piggyback later).

## Architecture Snapshot
1. **Device Mirrors** — existing PathIO providers expose `/system/devices/in/<class>/<id>/events` plus optional state nodes.
2. **IO Trellis** — new Trellis graph converts raw device payloads into normalized structs and publishes them under `/system/io/events/{pointer,text,button}`. All discrete button presses (keyboard keys, mouse buttons, gamepad buttons, physical phone buttons, etc.) map to the shared `ButtonEvent { source, device_id, button_code, pressed, modifiers, timestamp }` stream so downstream consumers do not need per-device code paths. Providers stay passive until a subscriber registers interest (via `/system/devices/in/<class>/<id>/config/push/enabled`); once enabled they coalesce samples according to per-provider rate/queue limits.
3. **IO Pump** — `CreateIOPump` spins one or more workers that block on the Trellis outputs and forward events to `/system/widgets/runtime/events/<window>/<stream>`. Optional `/system/io/events/pose` streams (headset/body pose) bypass the widget pipeline but share the same pump machinery for XR integrations.
4. **WidgetEventTrellis (routing)** — UI-layer Trellis (`CreateWidgetEventTrellis`) that listens to the pump outputs, enriches events with window/scene context, performs hit testing, and writes `WidgetOp`s to `widgets/<id>/ops/inbox/queue`.
5. **Reducers & Handlers** — existing reducers drain the ops queues; Phase 1 item 5 will add handler invocation.
6. **Telemetry & Throttling** — per-provider config nodes gate push/telemetry behavior; global `/_system/telemetry/start|stop` commands fan out to *all* PathSpace classes (not just input providers) so any subsystem can opt out of emitting counters/logs unless telemetry is explicitly enabled. Providers also honor `/system/devices/in/<class>/<id>/config/push/subscribers/*` so nothing pushes unless at least one subscriber opts in.

## Phases & Status
- ### Phase 0 – Foundations
  - ✅ (November 16, 2025) Defined canonical normalized event structs + paths in `include/pathspace/io/IoEvents.hpp` and updated `/system/io/events/{pointer,button,text,pose}` + `/system/devices/in/<class>/<id>/config/push/*` documentation (AI_Paths/AI_Architecture). Providers/tests reference the shared types going forward.
  - ✅ (November 16, 2025) Added provider config nodes (enable/rate_limit/max_queue/telemetry/subscribers) via `DevicePushConfigNodes`, wiring mouse/keyboard/gamepad PathIO layers plus sample apps; tests cover the new surfaces.
  - ✅ (November 17, 2025) Documented IO runtime launch options and provider environment flags in `docs/AI_ARCHITECTURE.md` (“IO runtime launch & environment knobs”) and `docs/AI_PATHS.md` (Device IO + declarative runtime sections), clarifying defaults and per-node behavior.
  - ✅ (November 17, 2025) Renamed `EnsureInputTask` → `CreateInputTask` in headers/sources/tests and updated docs to consistently refer to future `CreateIOPump` helpers so all runtime-start APIs follow the `Create*` convention.
- ### Phase 1 – IO Trellis
  - ✅ (November 17, 2025) Landed `CreateIOTrellis(PathSpace&, IoTrellisOptions const&)` + `IoTrellisHandle` (`include/pathspace/io/IoTrellis.hpp`, `src/pathspace/io/IoTrellis.cpp`). The worker auto-discovers `/system/devices/in/{pointer,text,keyboard,gamepad}/*`, enables push/subscriber config, drains provider queues, and writes canonical `PointerEvent`/`ButtonEvent`/`TextEvent` payloads into `/system/io/events/{pointer,button,text}`. Regression coverage lives in `tests/unit/io/test_IoTrellis.cpp`.
  - ✅ (November 17, 2025) Added opt-in telemetry: `/_system/telemetry/io/events_enabled` gates publishing, and `CreateIOTrellis` emits counters under `/system/io/events/metrics/{pointer_events_total,button_events_total,text_events_total}` once enabled.
- ### Phase 2 – IO Pump (Action Layer)
  - ✅ (November 17, 2025) Landed `CreateIOPump(PathSpace&, IoPumpOptions const&)` + `ShutdownIOPump` inside `include/pathspace/runtime/IOPump.hpp` / `src/pathspace/runtime/IOPump.cpp`. The worker watches `/system/io/events/{pointer,button,text}`, tracks window subscriptions under `/system/widgets/runtime/windows/<token>/subscriptions/{pointer,button,text}/devices`, and fans events into per-window queues at `/system/widgets/runtime/events/<token>/{pointer,button,text}/queue` with a global fallback.
  - ✅ (November 17, 2025) Extended `/system/widgets/runtime/input/metrics/*` with `pointer_events_total`, `button_events_total`, `text_events_total`, `events_dropped_total`, and `last_pump_ns` so telemetry mirrors the InputTask worker.
- ### Phase 3 – WidgetEventTrellis (Routing)
  - ✅ (November 17, 2025) Added `CreateWidgetEventTrellis(PathSpace&, WidgetEventTrellisOptions const&)` / `ShutdownWidgetEventTrellis` plus per-window pointer state so the worker drains `/system/widgets/runtime/events/<token>/{pointer,button}` queues, runs `Scene::HitTest`, and emits `WidgetOp`s for button/toggle widgets (hover/press/release/activate/toggle). Remaining work: extend routing to slider/list/tree/text widgets and sync reducer hooks once handler bindings land.
  - ✅ (November 17, 2025) Routing diagnostics land under `/system/widgets/runtime/events/log/errors/queue` with telemetry at `/system/widgets/runtime/events/metrics/*` (`pointer_events_total`, `button_events_total`, `widget_ops_total`, `hit_test_failures_total`, `last_dispatch_ns`) plus `/system/widgets/runtime/events/state/running`.
- ### Phase 4 – Handler Integration & Tooling
  - ✅ (November 17, 2025) Wired the declarative input task to handler bindings: the worker now resolves `events/<event>/handler = HandlerBinding` entries, looks up the in-memory registry, invokes button/toggle/slider/list/tree/input callbacks, and records results under `/system/widgets/runtime/input/metrics/{handlers_invoked_total,handler_failures_total,handler_missing_total,last_handler_ns}` with failures mirrored into `/system/widgets/runtime/input/log/errors/queue`.
  - ✅ (November 17, 2025) Introduced `CreateTelemetryControl` / `TelemetryControlOptions`. The worker watches `/_system/telemetry/start|stop/queue`, `/_system/io/push/subscriptions/queue`, and `/_system/io/push/throttle/queue`, toggles `/_system/telemetry/io/state/running`, and fans out device push/telemetry updates across `/system/devices/in/<class>/<id>/config/push/*`. Troubleshooting docs now describe the command flow plus the shared error log at `/_system/telemetry/log/errors/queue`.

## Pseudo-code Sketch

```cpp
// Normalized events emitted by the IO Trellis
// (window/scene context is attached later by WidgetEventTrellis once
// the pump forwards the device events to per-window streams)
enum class ButtonModifiers : uint32_t {
    None    = 0,
    Shift   = 1u << 0,
    Control = 1u << 1,
    Alt     = 1u << 2,
    Command = 1u << 3,
    Function= 1u << 4,
};

struct Pose {
    float position[3];      // meters in app/world space
    float orientation[4];   // quaternion (x, y, z, w)
};

struct StylusInfo {
    float pressure;   // 0..1
    float tilt_x;     // radians (0 if unsupported)
    float tilt_y;
    float twist;      // radians around stylus axis
    bool eraser;      // true when stylus reports eraser end
};

struct PointerEvent {
    std::string device_path; // e.g., /system/devices/in/pointer/default
    std::uint64_t pointer_id; // per-device contact ID (0 for single-pointer devices)
    struct Motion {
        float delta_x;
        float delta_y;
        float absolute_x;
        float absolute_y;
        bool absolute;
    } motion;
    PointerType type; // Mouse, Stylus, Touch, GamepadStick, VRController, etc.
    std::optional<Pose> pose; // VR controllers / headsets populate this
    std::optional<StylusInfo> stylus; // populated for stylus-capable devices
    struct Meta {
        ButtonModifiers modifiers;
        std::chrono::nanoseconds timestamp;
    } meta;
};

enum class ButtonSource {
    Mouse,
    Keyboard,
    Gamepad,
    VRController,
    PhoneButton,
    Custom,
};

struct ButtonEvent {
    ButtonSource source;
    std::string device_path; // full provider path for traceability
    uint32_t button_code;    // HID-style code or platform keycode (for keyboards)
    int button_id;           // logical index (e.g., 0=left mouse, 1=right mouse, ASCII for keyboards when available)
    struct State {
        bool pressed;
        bool repeat;
        float analog_value;
    } state;
    struct Meta {
        ButtonModifiers modifiers;
        std::chrono::nanoseconds timestamp;
    } meta;
};

// Gamepad stick mapping note: the IO Trellis emits PointerEvent entries with
// type = PointerType::GamepadStick, pointer_id identifying the stick (0 = left,
// 1 = right, etc.), motion.absolute_{x,y} = normalized stick deflection [-1,1],
// and motion.delta_{x,y} = per-sample changes. Triggers/analog buttons continue
// to use ButtonEvent::state.analog_value.

// VR controller note: controllers publish PointerEvent entries with type
// PointerType::VRController, pointer_id = hand index, pose populated with the
// 6DoF transform, and stylus omitted. Headset pose can optionally be emitted as
// standalone Pose events under /system/io/events/pose for renderer consumption;
// that stream is outside the declarative widget pipeline but shares the same
// IO Trellis infrastructure.

struct TextEvent {
    std::string device_path;
    char32_t codepoint;
    ButtonModifiers modifiers;
    bool repeat;
    std::chrono::nanoseconds timestamp;
};

auto CreateIOTrellis(PathSpace& space, IoTrellisOptions const& opts) -> SP::Expected<IoTrellisHandle> {
    PathSpaceTrellis trellis{space, "/system/io/events"};
    trellis.on("/system/devices/in/pointer/*/events", [&](RawPointerEvent const& raw) {
        if (!subscription_enabled(raw.device_path)) {
            return;
        }
        auto normalized = normalize_pointer(raw, opts.pointer_options);
        space.insert("/system/io/events/pointer", normalized);
    });
    trellis.on("/system/devices/in/keyboard/*/events", [&](RawKeyboardEvent const& raw) {
        space.insert("/system/io/events/button", convert_keyboard_button(raw));
        if (raw.type == RawKeyboardEvent::Type::Text) {
            space.insert("/system/io/events/text", convert_text(raw));
        }
    });
    trellis.on("/system/devices/in/gamepad/*/events", [&](RawGamepadEvent const& raw) {
        space.insert("/system/io/events/button", convert_gamepad_button(raw));
    });
    return IoTrellisHandle{std::move(trellis)};
}

auto CreateIOPump(PathSpace& space, IoPumpOptions const& opts) -> SP::Expected<bool> {
    if (pump_running(space)) {
        return false;
    }

    auto block_timeout = opts.block_timeout.value_or(std::chrono::microseconds{200});

    auto lambda = [&, block_timeout, stop = opts.stop_flag] () {
        while (!stop->load()) {
            if (auto pointer = take<PointerEvent>(space, "/system/io/events/pointer", block_timeout)) {
                auto routed = route_pointer(space, *pointer);
                publish_widget_ops(space, routed);
            }
            if (auto button = take<ButtonEvent>(space, "/system/io/events/button", block_timeout)) {
                auto routed = route_button(space, *button);
                publish_widget_ops(space, routed);
            }
            if (auto text = take<TextEvent>(space, "/system/io/events/text", block_timeout)) {
                publish_text_ops(space, *text);
            }
            update_metrics(space, opts.metrics_paths);
        }
    };

    space.insert("/system/io/pump/run", lambda, In{} & Immediate{});
    return true;
}
```

Key helpers referenced above:
- `subscription_enabled(path)` checks `/system/devices/in/<class>/<id>/config/push/subscribers/*`.
- `route_*` looks up the current window/scene binding, runs `Scene::HitTest`, and returns one or more `WidgetOp` payloads.
- `publish_widget_ops` writes into `widgets/<widget>/ops/inbox/queue` and logs failures under `widgets/<widget>/log/events`.
- `update_metrics` keeps `/system/widgets/runtime/input/metrics/*` in sync (widgets processed, events routed, drops, last_pump_ns).

## Deliverables
- `include/pathspace/runtime/IOPump.hpp` + `src/pathspace/runtime/IOPump.cpp` exporting `CreateIOPump`/`ShutdownIOPump` plus `MakeRuntimeWindowToken`.
- IO Trellis + routing Trellis helpers (header + source) inside `src/pathspace/ui/declarative`.
- Updated docs: `Plan_WidgetDeclarativeAPI.md`, `Plan_WidgetDeclarativeAPI_EventRouting.md`, `AI_PATHS.md`, `AI_ARCHITECTURE.md`, plus this plan.
- Telemetry/throttling config nodes under `/system/devices/in/<class>/<id>/config/*` with shared documentation.
- Runtime subscription surface: `/system/widgets/runtime/windows/<token>` stores `window` (absolute window path) and `subscriptions/{pointer,button,text}/devices` vectors so OS/window bridges can register interested device paths.
- Per-window queues live under `/system/widgets/runtime/events/<token>/{pointer,button,text}/queue` with `global/*` fallbacks for tools that have not registered their windows yet.
- Telemetry control commands: Queue a `TelemetryToggleCommand` on `/_system/telemetry/start/queue` or `/_system/telemetry/stop/queue` to flip `/_system/telemetry/io/events_enabled`, submit `DevicePushCommand` payloads to `/_system/io/push/subscriptions/queue` to opt specific devices/subscribers in or out (with optional telemetry mirroring), and push `DeviceThrottleCommand` entries through `/_system/io/push/throttle/queue` to set `config/push/{rate_limit_hz,max_queue}` for one device, a prefix (e.g., `/system/devices/in/pointer/*`), or every device via `device="*"`. All activity is logged under `/_system/telemetry/log/errors/queue`, and the controller exposes its lifecycle flag at `/_system/telemetry/io/state/running` for health checks.

## Open Questions / Risks
- **Blocking sources in Trellis** — today Trellis expects tree mutations as triggers. We may need a lightweight watcher that copies device events into the tree before Trellis can react. This plan assumes we can stage that work without blocking the entire Trellis thread.
- **Backpressure** — we must ensure provider push mode honors consumer rate limits; otherwise busy devices could starve the pump. The per-provider config + telemetry toggles are meant to mitigate this.
- **Multithreading** — IO Pump workers run outside Trellis; we must coordinate shutdown with `SP::System::ShutdownDeclarativeRuntime`.

## Tracking
- Reference in `docs/Plan_Overview.md` under the UI/runtime section so new contributors can monitor progress alongside the declarative widget plan.
- Update status checkboxes as each phase lands (✅ for complete, 🔜 for in-progress, ☐ for pending).
