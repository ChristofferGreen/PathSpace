# Plan: Declarative Widget Scene API

## Motivation
- Current widget workflow forces applications to assemble `DrawableBucketSnapshot`s manually and orchestrate focus/layout plumbing in user code.
- Minimal examples are burdened with render-packet manipulation, hiding true business logic behind boilerplate.
- Input routing and focus management require applications to mirror widget hierarchy details that belong in PathSpace.

## Objectives
1. Introduce a declarative widget API where widgets are defined entirely by values stored under canonical paths (state, styling, event handlers, render lambdas).
2. Move layout composition, focus management, and scene `DrawableBucketSnapshot` generation fully into PathSpace runtime tasks.
3. Ship the new API alongside existing widget builders, with equivalent sample applications and tests demonstrating parity.
4. Migrate documentation/examples/tests to the new API and retire legacy imperative bucket builders once parity is achieved.

## Non-Objectives
- Removing low-level bucket utilities used by advanced users (they remain as escape hatches).
- Re-designing non-widget UI components outside the declarative widget domain.
- Overhauling renderer back-ends; goal is to reuse existing preview/bucket infrastructure internally.

## User-Facing Design Sketch
```cpp
#include <pathspace/system/Standard.hpp>
#include <pathspace/app/Application.hpp>
#include <pathspace/ui/declarative/Button.hpp>
#include <pathspace/ui/declarative/Label.hpp>
#include <pathspace/ui/declarative/List.hpp>
#include <pathspace/ui/declarative/Scene.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

using namespace SP;
using namespace SP::UI;

auto main(int /*argc*/, char** /*argv*/) -> int {
    PathSpace space;
    System::LaunchStandard(space);
    const auto appRoot = App::Create(space, "hello_widgets");
    const auto windowPath = Window::Create(space, appRoot, "Hello Widgets");

    const auto buttonPath = Button::Create(space,
                                       windowPath,
                                       "hello_button",
                                       "Say Hello!",
                                       [](ButtonContext&) {
                                           std::cout << "Hello, world!" << std::endl;
                                       });

    const auto listPath = List::Create(space,
                                     windowPath,
                                     "greetings_list",
                                     ListArgs{
                                         .layout = ListLayout::Vertical,
                                         .spacing = 8.0f,
                                         .children = {
                                             {"hello_item", Label::Fragment("Hello")},
                                             {"bonus_button", Button::Fragment("Bonus", [](ButtonContext&) {
                                                 std::cout << "Bonus button clicked!" << std::endl;
                                             })},
                                         },
                                     });

    // Mix in a child at runtime using the Create overload
    const auto hiItemPath = Label::Create(space, listPath, "hi_item", "Hi there");

    const auto scenePath = Scene::Create(space, appRoot, windowPath);
    App::RunUI(scenePath);
    return 0;
}
```
- Widgets publish state under deterministic paths once mounted.
- `Button::Create(space, parent, name, args)` (and similar for other widgets) mounts the widget and returns its canonical path.
- Fragment helpers (e/g., `Button::Fragment`, `Label::Fragment`) produce self-contained specs for container children, with shorthand overloads for the common cases.
- Container arguments (e/g., `ListArgs::children`) accept named child fragments before mounting.
- Runtime tasks watch widget paths, synthesize render buckets from cached fragments, and the renderer simply composites cached buckets.
- `SP::System::LaunchStandard` bootstraps default themes, renderer targets, input queues, and background tasks (outside UI namespace).
- `SP::App::Create` registers the application under `/system/applications/<name>` and returns the canonical `AppRootPath`.
- `SP::Window::Create` registers a window under the application root and returns the concrete window path.
- `SP::Scene::Create` ensures the widget scene namespace exists, attaches the window, and returns the scene path.
- `SP::App::RunUI` runs the UI loop for the provided scene path, abstracting renderer/presenter coordination.

## Technical Approach

Declarative widgets now rely on the IO Pump feeds introduced in `docs/finished/Plan_IOPump_Finished.md` Phase 2: OS/device providers write raw events under `/system/devices/in/...`, `CreateIOTrellis` normalizes them into `/system/io/events/{pointer,button,text}`, and `CreateIOPump` republishes those events into `/system/widgets/runtime/events/<window-token>/{pointer,button,text}/queue` based on each window’s subscriptions stored under `/system/widgets/runtime/windows/<token>/subscriptions/{pointer,button,text}/devices`. Phase 3 landed `CreateWidgetEventTrellis`, which drains the per-window streams, runs `Scene::HitTest`, and now emits `WidgetOp`s for buttons, toggles, sliders, lists, trees, and text inputs (hover/press/release/drag/select/toggle/focus). Paint-surface gestures and stack panel interactions remain TODO items until those widgets grow declarative handlers—see “Remaining TODOs” below for the follow-up scope.

### Remaining TODOs (WidgetEventTrellis)
- Finish the paint-surface and stack panel routes so draw/drag gestures emit structured `WidgetOp`s (currently only logging placeholder events).
- Reuse the binding helpers (`Widgets::Bindings::Dispatch*`) or equivalent state-mutating utilities so routed events continue to update widget state/dirty hints instead of relying solely on handlers.
- Layer keyboard/gamepad routing (leveraging the focus controller) so declarative apps no longer need bespoke loops for those device classes; IME composition queues still need parity with legacy flows.

### Phase 0 – Foundations

#### Canonical Path Schema

| Node | Base Path | Key Properties | Notes |
|---|---|---|---|
| **Application** | `/system/applications/<app>` | `state/title`, `windows/<windowId>`, `scenes/<sceneId>`, `themes/default`, `events/lifecycle/handler` | Namespace returned by `App::Create`; seeds default runtime tasks and themes. |
| **Window** | `/system/applications/<app>/windows/<window>` | `state/title`, `state/visible`, `style/theme`, `widgets/<widgetName>`, `events/close/handler`, `events/focus/handler`, `render/dirty` | Widget helpers mount under `widgets/`; focus routing starts here. |
| **Scene** | `/system/applications/<app>/scenes/<scene>` | `structure/widgets/<widgetPath>`, `snapshot/<rev>`, `metrics/*`, `events/present/handler` | Scene buckets are composed from cached widget buckets and attach to a window during `Scene::Create`. |
| **Theme** | `/system/applications/<app>/themes/<name>` | `colors/<token>`, `typography/<token>`, `spacing/<token>`, `style/inherits` | Widgets reference theme names; runtime merges theme stacks. |
| **Button** | `/system/applications/<app>/windows/<window>/widgets/<name>` | `state/label`, `state/enabled`, `style/theme`, `events/press/handler`, `children/<childName>`, `render/synthesize`, `render/bucket`, `render/dirty` | `children` host nested widgets (labels, lists, etc.); `render/synthesize` stores the bucket callable. |
| **Toggle** | same as Button | `state/checked`, `style/theme`, `events/toggle/handler`, `children/<childName>`, `render/*` | Parent theme + metadata; toggles can host nested widgets (icon, label). |
| **Slider** | same as Button | `state/value`, `state/range/min`, `state/range/max`, `style/theme`, `events/change/handler`, `children/<childName>`, `render/*` | `state/dragging` tracked internally by runtime. |
| **List** | same as Button | `layout/orientation`, `layout/spacing`, `state/scroll_offset`, `style/theme`, `events/child_event/handler`, `children/<childName>`, `render/*` | Children widgets mount under `children/` and render sequentially. |
| **Tree** | same as Button | `nodes/<id>/state`, `nodes/<id>/children`, `style/theme`, `events/node_event/handler`, `render/*` | Tree nodes own child widget subtrees; metadata lives under `nodes/<id>`. |
| **Stack/Gallery** | same as Button | `panels/<id>/state`, `state/active_panel`, `style/theme`, `events/panel_select/handler`, `children/<childName>`, `render/*` | Panels can host nested widgets; active panel tracked in `state/active_panel`. |
| **Text Label** | same as Button | `state/text`, `style/theme`, `events/activate/handler`, `render/*` | Non-interactive by default; handler optional for advanced use. |
| **Input Field** | same as Button | `state/text`, `state/placeholder`, `state/focused`, `style/theme`, `events/change/handler`, `events/submit/handler`, `render/*` | Focus controller maintains `state/focused`; handlers fire directly on input events. |
| **Paint Surface** | same as Button | `state/brush/size`, `state/brush/color`, `state/stroke_mode`, `state/history/<id>`, `events/draw/handler`, `render/buffer`, `render/dirty`, `assets/texture` | Runtime-owned picture buffer accepts circle/line/pixel operations; history enables undo/redo. |

Within each widget subtree we store focus metadata (`focus/order`), layout hints (`layout/<prop>`), event handler callables (`events/<event>/handler`), cached bucket metadata (`render/bucket`, `render/dirty`), and composition (`children/<childName>` subtrees). Event handlers live under `events/.../handler` as callable entries executed directly by the input task. Each widget exposes a render synthesis callable (`render/synthesize`). When widget state changes, the runtime invokes the callable, caches the resulting bucket, propagates `render/dirty` to children if needed, and the renderer consumes cached buckets—avoiding per-frame scenegraph traversal.

Undo/redo integrations must keep all data for a logical command inside a single widget root. For paint surfaces that means colocating stroke history, layout/index metadata, and any ancillary bookkeeping under `widgets/<id>/state/...`. Editors that need to update shared indexes should funnel those writes through a command-log subtree owned by the same root before enabling history; duplicate `HistoryOptions::sharedStackKey` values across siblings are now rejected by `enableHistory`, so regrouping is required before opting-in.

#### Event Dispatch Contract

- Widget bindings enqueue `WidgetOp` items describing the widget path and event kind (e.g., `Press`, `Toggle`, `Draw`).
- The dispatcher resolves `<widget>/events/<event>/handler` and, when present, invokes the stored callable inline on the task thread handling the op. Handlers use the signature `void()` and read any required context from PathSpace state (e.g., op queues or widget state nodes).
- Missing handlers are treated as no-ops; future telemetry work will decide whether to mirror these drops under `<widget>/log/events`.
- Scenes that need higher-level interception can swap the handler node entirely (write a different callable) or wrap the existing callback in a delegating lambda—no separate routing tables are required.

Canonical namespaces stay consistent across widgets: `state/` for mutable widget data, `style/` for theme references and overrides, `layout/` for layout hints, `events/` for handler callables and their logs, `children/` for nested widget mount points, and `render/` for cached rendering artifacts.

#### Fragment Mount Lifecycle

- Treat fragments as widgets already instantiated inside a private subspace. Mounting relocates that subspace under the parent widget tree without schema translation.
- Simple mounts only require inserting the fragment root into the parent PathSpace (`Widgets::MountFragment` performs a subtree insert). The runtime then marks `render/dirty` and enqueues an auto-render event so the new widget is visible immediately.
- Callable nodes (lambdas, callbacks) live under `callbacks/<id>` inside the fragment. During mount, their captured widget paths are rewritten to the destination path and the handlers are copied into the destination `events/<event>/handler`. Shared callbacks use `std::shared_ptr` delegates so relocation preserves ownership semantics.
- Containers that accept child fragments (lists, stacks) enumerate `children/*` in the fragment subspace and insert each child subtree in order under the parent widget path. Dirtiness/bubble rules follow the standard widget schema; no fragment-specific namespaces exist once mounted.
- When fragment and parent both define `events/<event>/handler`, the parent win strategy applies: the fragment mount preserves the parent handler unless explicitly overridden. Document the override semantics so tooling can flag accidental handler replacement.

#### Scene & Window Wiring

- Publish window bindings under `structure/window/<window-id>`:
  - `views/<view-id>/scene` — app-relative pointer to the scene root.
  - `views/<view-id>/renderer` — canonical renderer target (`renderers/<rid>/targets/<kind>/<name>`).
  - `views/<view-id>/present` — execution the presenter calls to display the current snapshot.
  - `views/<view-id>/dirty` — per-window dirty mirror so slow presenters do not stall others.
  - `state/attached` — bool flag signalling whether the window actively presents the scene.
- LaunchStandard/Window::Create now populate `views/<view-id>/{surface,renderer,scene}` plus the matching `structure/window/<window-id>/{surface,renderer,present}` entries automatically, wiring each view to the default declarative renderer/target pair seeded under `renderers/widgets_declarative_renderer`.
- Reattachment flow: set `state/attached = false`, flush pending presents, update renderer bindings, then set `state/attached = true`, mark `render/dirty = true`, and enqueue an auto-render request to force a fresh frame whenever a window is recreated or reassigned.
- Multi-window support: allow multiple `structure/window` entries; input routing keys off the window ID so focus and pointer events remain scoped. Each presenter watches its own `views/<view-id>/dirty` flag and auto-render queue.
- Paint surface resize semantics: expanding the layout grows `render/buffer` (and `assets/texture` when present) to the new bounds and republishes the full stroke history; shrinking retains the existing buffer and clips presentation to the visible rect so strokes persist for future expansions.
- Diagnostics: log reattachment and paint-buffer resize events under `structure/window/<window-id>/log/events` for QA visibility.

#### Focus Controller

- Focus eligibility defaults to the widget tree structure: any widget with an interaction handler (`events/<event>/handler`) is considered focusable unless `focus/disabled = true`.
- Focus order follows depth-first traversal of the mounted widget subtree. Developers control traversal order by arranging children in the desired sequence; no additional ordering metadata is required.
- *Update (November 15, 2025):* the runtime now writes zero-based `focus/order` values for every widget in the window’s depth-first traversal. `WidgetFocus::BuildWindowOrder` exposes the computed order for tooling/tests.
- Navigating forward advances to the next widget in depth-first order within the same window. Navigating backward walks the inverse order. By default the controller wraps, so advancing past the final widget jumps back to the first focusable widget (and vice-versa when moving backward). Containers may override this by setting `focus/wrap = false` under their root to clamp within their subtree.
- When focus changes, the controller updates `focus/current` under the scene, mirrors the target path under `widgets/<id>/focus/current`, and emits a `WidgetOp`/`WidgetAction` so reducers can react (e.g., render focus rings).
- *Update (November 15, 2025):* `widgets/<id>/focus/current` is now a runtime-managed boolean flag, and `structure/window/<window-id>/focus/current` mirrors the absolute widget path when a declarative scene is attached to the window.
- Pointer activation (`HandlePointerDown`) syncs focus to the pointed widget before dispatching the widget op. Keyboard/gamepad navigation invokes `Widgets::Focus::Cycle` helpers that produce the same depth-first traversal.
- Multi-window scenes keep independent focus state per window under `structure/window/<window-id>/focus/current`; the controller never crosses window boundaries during traversal.
- Traversal requires enumerating child nodes within a widget subtree. `PathSpace::listChildren()` (documented in `docs/finished/Plan_PathSpace_Finished.md`) returns `std::vector<std::string>` component names for the current cursor or a supplied subpath without decoding payloads. The focus controller uses this API to perform depth-first traversal without widget-specific knowledge.

#### Paint Surface Resolution

- Effective buffer size comes from the widget’s computed layout bounds: read `widgets/<id>/layout/computed/size` and multiply by the active window DPI stored at `structure/window/<window-id>/metrics/dpi` to determine pixel dimensions.
- Persist the resolved metrics under `widgets/<id>/render/buffer/metrics/{width,height,dpi}`; presenters and reducers use these values when staging textures or mapping pointer coordinates.
- When the layout expands, allocate a new buffer at the larger resolution, replay stroke history from `state/history/<seq>` into the new buffer, and refresh `assets/texture` if GPU staging is enabled. Mark `render/dirty = true` and enqueue an auto-render event.
- When the layout shrinks, retain the existing buffer; update `render/buffer/viewport` to describe the visible rectangle and let presenters clip to that region so future expansions can reuse the preserved data.
- Input processing normalises pointer coordinates to layout space before writing stroke entries; render tasks multiply by the stored metrics to convert back to pixel coordinates. Stylus overrides can request higher DPI by writing `render/buffer/metrics/dpi_override` before the next expansion.
- Add resize regression tests that grow/shrink the widget, verifying buffer metrics, viewport clipping, and stroke replay maintain fidelity without data loss.

The paintable surface widget reuses the same namespaces but adds `render/buffer` to store the current picture (CPU-readable texture) and an optional `assets/texture` node for GPU residency. Draw events append stroke metadata under `state/history/<id>` so undo/redo tasks can rebuild the buffer on demand.

Undo/redo mechanics for stroke history and child enumeration are defined in `docs/finished/Plan_PathSpace_Finished.md`; paint widgets consume those PathSpace features rather than implementing custom versions.
History diagnostics rely on `_history/stats/*` telemetry published by the undo layer; downstream tooling should read those counters (versioned binary persistence) when presenting paint history/retention state.

#### GPU Staging

- Optional staging is controlled by `widgets/<id>/render/gpu/enabled` (default `false`). When enabled, a background upload task watches `render/buffer` and publishes GPU resources under `widgets/<id>/assets/texture`.
- Synchronization state lives under `widgets/<id>/render/gpu/state` with values `Idle`, `Uploading`, `Ready`, and `DirtyPartial`. Uploads set `Uploading`, record timestamps at `render/gpu/fence/start` and `/end`, then flip back to `Ready` when the texture is current.
- Sub-rectangle uploads reuse dirty hints appended to `render/gpu/dirtyRects`. The uploader processes each rect, clears the list on success, or logs to `render/gpu/log/events` and performs a full upload on failure.
- Presenter integration: window targets check `render/gpu/state`. If `Ready`, they bind `assets/texture`; otherwise they sample the CPU buffer. Multiple windows share the same staging metadata to avoid redundant uploads.
- Metrics (last upload duration, bytes staged, partial update count) surface at `render/gpu/stats`. Regression tests should exercise full uploads, incremental updates, toggling GPU staging at runtime, and fallback behaviour when uploads fail.

#### Accessibility (macOS)

- Publish accessibility metadata under `widgets/<id>/accessibility/{role,label,hint,value}`. Roles map to AppKit constants (e.g., `AXButton`, `AXStaticText`, `AXSlider`), while labels/hints provide localized strings for VoiceOver.
- The macOS window bridge (`PathWindowView.mm`) mirrors this metadata into native `NSAccessibilityElement` instances. Each widget path produces a stable accessibility identifier (`accessibility/id`) so VoiceOver can reference elements across updates.
- Changes to accessibility nodes trigger targeted notifications via `structure/window/<window-id>/accessibility/dirty`, prompting the bridge to refresh the corresponding elements without rebuilding the entire tree.
- Input routing remains in sync: when VoiceOver activates an element, the bridge emits a declarative event (`WidgetOpKind::AccessibilityActivate`) that flows through the standard routing pipeline.
- Testing: add VoiceOver-driven smoke tests that tab through declarative widgets, validate labels/roles via the Accessibility Inspector, and ensure actions fire through the same handler-wrapping logic. Document troubleshooting steps for developers configuring localized strings and custom roles.

Fragment helpers (e/g., `Label::Fragment`, `Button::Fragment`) provide convenience overloads (`Label::Fragment("Hello")`, `Button::Fragment("Label", on_press)`) alongside structure-based overloads (`Label::Fragment(LabelArgs{...})`, `Button::Fragment(ButtonArgs{...})`) so simple cases remain ergonomic while complex cases stay expressive. **Update (November 14, 2025):** `include/pathspace/ui/declarative/Widgets.hpp` now exposes `WidgetFragment`, `Widgets::Mount`, widget-specific `Create` helpers, and update utilities. Fragments encapsulate both their populate lambda and child fragments so containers can bulk-mount declarative children without re-authoring PathSpace writes.

1. **Schema definition**
   - ✅ (November 14, 2025) Canonical schema captured in `include/pathspace/ui/declarative/Schema.hpp`; docs updated (`docs/AI_PATHS.md`) to publish the shared namespaces and widget overlays.
   - Inventory widgets and document required/optional fields, including `children`, `render/*`, `state/*`, and event nodes.
   - Update `docs/AI_PATHS.md` with canonical layout.
   - Ensure composition rules are defined (parents adopting child buckets/paths).
2. **Event nodes**
   - Map each widget event to canonical `events/<event>/handler` nodes (single callable per event) and document defaults.
   - Provide helpers for installing/uninstalling handlers directly via PathSpace inserts so fragments and scenes can replace lambdas safely.
   - Document how fragments publish handlers during mount and how scenes override/restore them.
   - Add tests covering registration, invocation, and propagation.
   - ✅ (November 14, 2025) `events/<event>/handler` now stores a `HandlerBinding { registry_key, kind }`. Declarative helpers register handlers with an in-memory registry so runtime dispatch can resolve the user lambda without storing `std::function` payloads in PathSpace. Removal cleans up registry entries, and tests exercise button/list slider bindings.
3. **Render synthesis contract**
   - Define callable signature (inputs: widget path/context; output: `DrawableBucketSnapshot`), including conventions for composing child widget buckets.
   - Provide caching semantics (`render/bucket`, `render/dirty`) and cascade rules (parents mark descendants dirty when shared state changes).
   - Add tests comparing new bucket output vs. current builder output.
   - ✅ (November 14, 2025) `render/synthesize` nodes store a lightweight `RenderDescriptor` (widget kind enum) instead of opaque callables. Declarative setters flip `render/dirty` whenever state changes; the runtime synthesizer will read the descriptor and rebuild buckets from the stored state/style.
4. **Fragment specification**
   - Define the `WidgetFragment` structure (scratch PathSpace + metadata) returned by helper functions like `Button::Fragment`, `List::Fragment`, etc.
   - Document how event handlers/render lambdas are re-bound when fragments are mounted under a concrete path.
   - Provide utilities for merging fragments via container argument structs (e/g., `ListArgs::children`) and ensure copying/moving semantics are well-defined.
   - ✅ (November 14, 2025) `WidgetFragment` now owns its populate lambda and child fragments; helpers expose strongly-typed args, automatically register handlers, and mount children via `Widgets::Mount`.
5. **Builder input abstraction**
   - ✅ (November 15, 2025) Introduced `include/pathspace/ui/declarative/Descriptor.hpp` plus the loader/synthesizer in `src/pathspace/ui/declarative/Descriptor.cpp`. Buttons, toggles, sliders, lists, trees, and labels now load their sanitized state/style directly from PathSpace, convert to a `WidgetDescriptor`, and rebuild buckets via the existing preview builders. New doctest coverage (`tests/ui/test_DeclarativeWidgets.cpp`) compares descriptor-built buckets with the legacy `Build*Preview` output to guard regressions.
   - ✅ (November 15, 2025) Descriptor coverage now includes `Stack`, `InputField`, and `PaintSurface`. Input fields derive their style from the active theme (widget → parent → window → application default) and feed the text-field bucket builder; stack/paint surfaces currently publish metadata and synthesize empty buckets until their layout/paint buffers land in later phases.
   - Declarative runtime can now take `render/synthesize` → `WidgetDescriptor` without storing opaque lambdas; container children stay in canonical `children/` paths for later traversal. *Follow-ups:* wire stack layout metadata + panel visibility into descriptor buckets, and plug paint-surface stroke/buffer emitters into the synthesizer so the placeholder buckets gain real content.
6. **Documentation updates**
   - ✅ (November 15, 2025) Expanded `docs/AI_PATHS.md` with the declarative namespace summary and dirty/lifecycle flow. Authored `docs/Widget_Schema_Reference.md` as the long-form appendix mirroring the schema headers so maintainers have a single reference for widget node contracts.
   - Keep both docs in sync whenever schema headers change; future widget additions should update the appendix and the high-level summary in `AI_PATHS.md`.

### Phase 1 – New Declarative Runtime
0. **Runtime bootstrap**
   - ✅ (Nov 14, 2025) Added `SP::System::LaunchStandard`, `SP::App::Create`, `SP::Window::Create`, and `SP::Scene::Create` under `include/pathspace/ui/declarative/Runtime.hpp`. The helpers seed `/system/themes`, normalize `/system/applications/<app>`, and wire `windows/<id>/views/<view>` to `scenes/<scene>` under the declarative schema so later phases can assume the canonical nodes exist.
   - ✅ (Nov 14, 2025) Runtime now starts the `/system/widgets/runtime/input` pump (draining `widgets/<widget>/ops/inbox/queue` into `ops/actions/...`), tracks health metrics, and auto-attaches a default renderer/surface binding so every window view has a populated `surface`/`renderer` pair plus `structure/window/<id>/{surface,renderer,present}` mirrors. Scene + window helpers also stamp the active theme onto `style/theme`.
1. **API surface**
   - ✅ (November 14, 2025) Declarative helpers now ship in `include/pathspace/ui/declarative/Widgets.hpp`. `Widgets::Mount` consumes `WidgetFragment`s, every widget exposes `Create` + `Fragment` helpers, and update utilities (`Button::SetLabel`, `Slider::SetValue`, `List::SetItems`, `Label::SetText`, etc.) mark `render/dirty` when state changes. Removal is currently logical (`state/removed = true`) so runtime consumers can detach widgets without blowing away history; the `Move` helper stubs out with `Error::UnimplementedFeature` and is tracked below so we can preserve arbitrary subtree state when reparenting. `PaintSurface::Create` seeds brush/gpu metadata and registers draw handlers using the new registry.
   - ✅ (November 15, 2025) `Widgets::Move` now relocates existing widget subtrees by remounting the trie nodes, re-binding event handlers, and marking both the widget and its parents dirty so the lifecycle worker rebuilds buckets on the next pass. Moving between containers no longer requires recreating widgets or re-registering handlers.
2. **Scene lifecycle**
   - ✅ (November 15, 2025) `Scene::Create` now spins up a trellis-backed lifecycle worker per scene. Widgets publish dirty events under `render/events/dirty`, the worker fans them into `scene/runtime/lifecycle/trellis`, rebuilds buckets via `WidgetDescriptor`, writes them to `scene/structure/widgets/<widget>/render/bucket`, and exposes metrics/state under `scene/runtime/lifecycle/*`. `SP::Scene::Shutdown` tears the worker down, and doctest coverage (`tests/ui/test_DeclarativeSceneLifecycle.cpp`) exercises the end-to-end flow.
   - ✅ (November 15, 2025) Scene lifecycle worker now caches per-widget buckets, aggregates them into `SceneSnapshotBuilder` revisions, exposes publish/queue metrics, and reacts to theme + focus invalidations. Removed widgets trigger trellis deregistration, cache eviction, and `structure/widgets/.../render/bucket` cleanup so lifecycle metrics stay accurate.
3. **Focus controller**
   - Design focus metadata and build focus graph automatically.
   - Integrate with existing bindings so focus/activation events propagate transparently.
   - ✅ (November 15, 2025) Focus controller now assigns depth-first `focus/order` indices, mirrors active widgets via `widgets/<id>/focus/current`, and writes the active widget path to `structure/window/<window-id>/focus/current`. Declarative helpers rebuild the order whenever focus changes, so keyboard/gamepad traversal no longer requires app-authored lists. `tests/ui/test_DeclarativeWidgets.cpp` exercises the metadata + window mirror flow end-to-end.
   - ✅ (November 15, 2025) Wired the declarative dispatcher into the focus controller: `Widgets::Focus::Move(space, config, Direction)` now derives traversal order from the runtime metadata, so keyboard/gamepad routing simply calls the controller without maintaining bespoke focus lists. UITests cover the new overload and the auto-render telemetry it triggers during Tab/Shift+Tab and gamepad hops.
4. **Event dispatch**
   - ✅ (November 17, 2025) InputTask now mirrors every reduced `WidgetAction` into `widgets/<id>/events/inbox/queue` plus per-event queues (`events/press/queue`, `events/toggle/queue`, `events/change/queue`, etc.), publishes enqueue telemetry under `/system/widgets/runtime/input/metrics/{events_enqueued_total,events_dropped_total}`, and logs enqueue failures to `/system/widgets/runtime/input/log/errors/queue`.
   - ✅ (November 17, 2025) The runtime keeps enqueue lightweight by reusing reducer payloads; handler dispatch resolves the same canonical queue before invoking `events/.../handler` callables, so tooling can read the queue without touching reducers.
   - Ensure fragment mounts relocate handler callables when they are inserted, and provide helpers for scenes to wrap/replace handlers on demand.
5. **Input processing node**
   - Runtime pump now drains widget ops into `WidgetAction` queues; follow-ups must extend it to invoke widget handlers and commit resultant state updates.
   - Ensure the task runs safely on existing task pool, honors the handler return enum, records failures to `<widget>/log/events`, and exposes logging hooks.
   - For paint surfaces, translate pointer/touch input into stroke primitives (lines, circles, pixel edits) and append them to `state/history/<id>` while triggering buffer updates. Mount the paint buffer beneath an `UndoableSpace` wrapper so stroke edits participate in undo history automatically.
   - Consume device events from existing PathIO providers (`/system/devices/in/pointer/<id>/events`, `/system/devices/in/text/<id>/events`, `/system/devices/in/gamepad/<id>/events`) using `PathIOPointerMixer` for pointer aggregation. No new IO queues are introduced; the task transforms device events into `WidgetOp` records and handles backpressure using the mixers' built-in depth/timeout policies.
   - ✅ (November 17, 2025) `CreateInputTask` now resolves `HandlerBinding` records, invokes the bound button/toggle/slider/list/tree/input handlers, and publishes metrics at `/system/widgets/runtime/input/metrics/{handlers_invoked_total,handler_failures_total,handler_missing_total,last_handler_ns}` while logging dispatch failures to `/system/widgets/runtime/input/log/errors/queue`.
   - ✅ (November 17, 2025) `CreateWidgetEventTrellis` now emits `WidgetOp`s for sliders (begin/update/commit), lists (hover/select/activate), trees (hover/toggle/select), and text inputs (focus/input) in addition to the existing button/toggle route. Regression coverage lives in `tests/ui/test_WidgetEventTrellis.cpp` so declarative apps no longer need bespoke loops for those widgets.
   - **Remaining TODOs:** wire paint-surface and stack-panel dispatch contexts once their events exist, and surface per-widget success counters/telemetry for handler debugging.
6. **Rendering pipeline**
   - Renderer consumes cached widget buckets without traversing widget trees each frame.
   - Aggregator combines updated buckets into the scene snapshot when notified of dirty widgets.
   - Updated buckets swap in incrementally so presents pick up refreshed data on the next frame.
   - Publish revisions via `SceneSnapshotBuilder` to preserve presenter compatibility.
   - Paint surface synthesis must translate stroke/history data into texture updates (CPU + optional GPU staging) and expose incremental dirty regions for efficient redraw.
7. **Telemetry / logging**
   - Add debug logging for schema loads, focus transitions, input processing, and render updates.
   - Track perf counters comparing declarative vs. legacy pipeline.

### Phase 1/1 – Theme Runtime
1. **Theme resolution helpers**
   - ✅ (November 15, 2025) Theme resolver walks widget → parent widget → window → app default (`/themes/default`) and caches the resolved `WidgetTheme` so descriptors can hydrate styles without duplicating `meta/style` payloads.
   - 🔜 Extend the helper to honor per-theme `style/inherits` chains once the editing API lands.
2. **Theme editing API**
   - Provide `Theme::Create`, `Theme::SetColor`, etc., validating resources.
3. **Examples & tests**
   - Test inheritance behavior and update examples to define themes declaratively.

### Phase 2 – Examples & Tests
1. **Examples**
   - Port minimal and advanced widget examples to the declarative API.
   - Provide the simple “hello button/list” example.
   - Replace the legacy paint example with a declarative version showcasing `PaintSurface` drawing circles/lines/pixels.
   - Add undo/redo affordances (buttons + keyboard shortcuts) backed by the `UndoableSpace` history so the paint demo highlights history integration end-to-end.
2. **Tests**
   - Add parity tests comparing declarative buckets vs. legacy.
   - Cover focus navigation (including list/tree selections).
   - Extend fuzz harnesses for random widget trees and callback firing.
3. **Docs**
   - Document the workflow (`docs/WidgetDeclarativeAPI/md`).
   - Update onboarding/checklists and provide migration notes.

### Phase 3 – Migration & Parity
1. **Feature audit**
   - Matrix of widget features vs. legacy; fill gaps.
2. **Performance validation**
   - Benchmark both pipelines and optimize if necessary.
3. **Documentation alignment**
   - Promote declarative API in high-level docs; mark legacy APIs deprecated.
4. **Consumer migration**
   - Port internal tools/examples; offer migration aids.

### Phase 4 – Deprecation & Removal
1. **Deprecation notice**
   - Announce timeline; add diagnostics for legacy usage.
2. **Support window**
   - Maintain compatibility during transition.
3. **Removal**
   - Delete legacy builders/samples after window; simplify remaining docs.
4. **Post-mortem**
   - Record lessons learned and perf comparisons.

## Deliverables
- Declarative widget runtime (`src/pathspace/ui/declarative/...`).
- Helper headers exposing `Create` functions per widget namespace (Button, List, Slider, etc.).
- Updated renderer focus/input pipeline and declarative input processing task.
- Updated examples/tests demonstrating parity.
- `PaintSurface` widget implementation with incremental paint buffer integration.
- Documentation describing schema, API usage, migration path.

## Risks & Mitigations
- **Performance regressions:** ensure render callables cache buckets; benchmark vs. legacy.
- **Callback lifetimes:** verify callable nodes clean up when widgets are removed; document ownership expectations.
- **Event backlog:** ensure input task handles backpressure gracefully.
- **Incremental rollout:** keep legacy API available until parity confirmed.

## Rollout Criteria
- Declarative examples pass UI test suite + parity tests.
- Manual QA confirms focus, keyboard, pointer interactions with no app-side plumbing.
- Event routing tables documented and enforced by the declarative runtime, with legacy fallback coverage validated.
- Documentation/migration guides published.
- Maintainers agree to start legacy deprecation countdown.
