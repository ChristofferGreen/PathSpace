# PathSpace Window Manager Plan

> **Drafted:** November 21, 2025 — initial proposal for a PathSpace-native window manager sitting on top of the declarative UI stack.

## Motivation
- Declarative applications already publish windows under the canonical UI schema, but there is no shared way to orchestrate them (move, stack, minimize) once multiple apps run inside the same PathSpace root.
- Experiments with ad-hoc controllers duplicate effort, lack consistency, and ignore affordances such as docks or window buttons.
- A cohesive, NextStep-inspired window manager gives us a single surface for multi-app orchestration, system-wide chrome, and app launching without tying behavior to a single demo.

## Goals
1. Build a window manager service that observes `windows/<id>/...` nodes and manipulates them via the existing UI schema (no private hooks).
2. Provide standard chrome controls (move, resize, minimize, maximize, close) along with the button widgets/wiring required to trigger those actions.
3. Deliver a NextStep-style look: title bars, traffic-light buttons, and a persistent dock with live icons (icons rendered by their owning apps when desired).
4. Implement a dock that can launch applications, pin running apps, and host live-updating mini surfaces (apps that render directly into their icon slot).
5. Keep all behavior declarative: window manager UIs, docks, and overlays publish scenes/surfaces via PathSpace, so tooling/tests can reason about state.
6. Integrate with existing focus/input pipelines so window chrome participates in the same WidgetEventTrellis and bindings as other UI elements.

## Non-Goals (v1)
- Compositing external OS windows. We stay inside PathSpace-rendered surfaces.
- Building a tiling-only manager; v1 focuses on overlapping/floating windows.
- Replacing app-defined shortcuts/menus (future extensions may hook menu bars).

## Dependencies
- Declarative widget runtime (`docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`) for chrome controls and dock widgets.
- UI renderer/presenter stack (`docs/finished/Plan_SceneGraph_Renderer_Finished.md`).
- Standard path schema (`AI_PATHS.md`) — window manager must only touch documented nodes.

## Architecture Overview
```
App roots --> windows/<id>/...    (existing UI schema)
          \-> window_manager service (PathSpace nodes)
              |- chrome overlays per window (title bar, buttons)
              |- dock scene + renderer target
              |- controller tasks (watchers for window state, dock actions)
```
- Chrome overlays attach via declarative fragments mounted under each window’s scene; actions write to control nodes (`windows/<id>/state/*`, `present/params/*`).
- Dock lives in its own scene/surface; entries reference app descriptors (paths to launchers). Live icons subscribe to app-published thumbnail paths or render mini scenes directly.
- Controller tasks run as PathSpace callables: they watch dock button queues, manage window z-order metadata, and route minimize/restore requests.

## Path & Data Requirements
- **Window metadata:** extend (or reuse) `windows/<id>/meta/*` for `state` (`normal|minimized|maximized|closed`), z-index tracking, and dock pinning.
- **Chrome nodes:** mount under `windows/<id>/chrome/*` describing button widgets, drag handles, etc.
- **Dock state:** `/system/window_manager/dock/{entries,active,launchRequests}` with live icon subpaths.
- **Controller queues:** `window_manager/ops/{move,minimize,maximize,close,launch}` for deterministic task processing.

## UX & Visual Targets
- Title bars mimic NextStep: left-aligned icon, centered title, right-aligned control buttons.
- Dock on screen edge with shadowed icons; active apps display indicator; icons can be live surfaces (mini scenes) or static bitmaps.
- Window snapping/movement uses the same pointer events as widgets; dragging the title bar rewrites `windows/<id>/layout/position`.

## Testing & Tooling
- UITests cover chrome button wiring (minimize/maximize/close) and dock launches.
- Screenshot baselines for dock + default window chrome.
- Fuzz or scripted pointer sequences ensure drag/resize interactions stay deterministic.

## Roadmap
1. **Research & schema updates**
   - Audit `AI_PATHS.md` for window metadata; propose additional nodes as needed.
   - Document controller queues and chrome expectations.
2. **Chrome MVP**
   - Declarative fragments for title bar + buttons.
   - Controller tasks that update window state and z-order.
3. **Dock MVP**
   - Static dock entries launching demo apps.
   - Pin/unpin behavior; active indicator.
4. **Live icons & animations**
   - Allow apps to publish mini scenes for icons.
   - Provide fallback thumbnails when no live feed is present.
5. **Polish & theming**
   - Theme hooks (colors, metrics) + NextStep styling defaults.
   - Accessibility (focus, keyboard shortcuts for controls).
6. **Future extensions**
   - Menu bar integration, multiple workspaces, tiling/stacking presets.

## Open Questions
- Should z-order be centralized under the window manager, or should each app supply hints?
- How do we arbitrate sizing constraints between apps (min/max sizes) and manager actions?
- What persistence do we need for dock layouts/pinned apps across sessions?
- Do we allow scripts/apps to automate window placement via window manager APIs?
