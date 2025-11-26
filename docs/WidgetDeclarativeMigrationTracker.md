# Widget Declarative Migration Tracker

_Updated: November 26, 2025_

## Purpose
Phase 3 of `docs/Plan_WidgetDeclarativeAPI.md` requires us to prove that every downstream tool consuming widget data has migrated (or has an explicit plan to migrate) off the legacy `SP::UI::Builders::*` surface before the February 1, 2026 support-window cutoff. This tracker records the status of each consumer named across the active plans (`Plan_PathSpace_Inspector.md`, `Plan_WebServer_Adapter.md`, `Plan_PathSpaceWindowManager.md`, `Plan_PathSpaceTerminal.md`, and related docs) so we can see, at a glance, which teams still rely on legacy paths and what telemetry guards their progress.

## Adoption Scorecard
| Consumer / Tooling Surface | Plan Doc(s) | Declarative status | Legacy touch points & exit criteria | Telemetry / Verification | Last check |
| --- | --- | --- | --- | --- | --- |
| **PathSpace Inspector (web)** | `docs/Plan_PathSpace_Inspector.md` | **Pending:** design drafted; backend/UI not yet implemented. Prototypes rely on the `scripts/paint_example_inspector_panel.py` bridge only. | Inspector MVP must consume declarative widget state (`scene/structure/widgets/*`, `widgets/<id>/metrics/*`) directly. Cut over once JSON serialization + distributed mounts ship. | - Monitor `/_system/diagnostics/legacy_widget_builders/status/usage_total` per `SP::UI::Builders::*` entry.<br>- Inspector backend must read from `renderers/widgets_declarative_renderer` outputs without invoking legacy builders.<br>- Track open issues in `docs/Plan_PathSpace_Inspector.md#Next Actions`. | 2025‑11‑26 |
| **Web Server Adapter / HTML delivery** | `docs/Plan_WebServer_Adapter.md`, `docs/Plan_Distributed_PathSpace.md` | **Pending:** plan assumes declarative scenes but implementation not started. | Adapter must serve HTML targets sourced from declarative `SceneLifecycle` buckets and never call legacy builders to synthesize DOM. Requires distributed mounts + JSON serializer. | - Watch `renderers/<rid>/targets/html/<view>/output/v1/common/*` to confirm revisions come from declarative runtime.<br>- Keep legacy usage counters at zero.<br>- Add integration tests in adapter repo once HTML path exists. | 2025‑11‑26 |
| **PathSpace Window Manager + Dock** | `docs/Plan_PathSpaceWindowManager.md` | **Planned:** chrome/dock designs assume declarative fragments; no runtime yet. | When prototypes land, ensure chrome widgets mount via `SP::UI::Declarative::*` and do not import `WidgetBuilders.cpp`. Legacy usage should remain zero because the feature is greenfield. | - Code review checklist: `rg WidgetBuilders src/pathspace/window_manager` must stay empty.<br>- UITests should verify focus/telemetry via declarative metrics. | 2025‑11‑26 |
| **PathSpace Terminal / Carta Linea bridge** | `docs/Plan_PathSpaceTerminal.md`, `docs/Plan_CartaLinea.md` | **Planned:** terminal UI not yet implemented. | Terminal widgets, scrollback, and embedded viewers must use declarative helpers; Carta Linea visualizers should read declarative telemetry from launched apps. | - Reuse declarative readiness helper (`PathSpaceExamples::ensure_declarative_scene_ready`).<br>- Verify terminal scenes only depend on `widgets/<id>/...` namespaces defined in `docs/AI_PATHS.md`. | 2025‑11‑26 |
| **Paint example + screenshot + diagnostic dashboards** | `docs/Plan_WidgetDeclarativeAPI.md`, `docs/Memory.md` | **Complete:** declarative paint sample, screenshot harness, and inspector panel already rely on declarative runtime. | Keep `examples/paint_example*.cpp`, `scripts/check_paint_screenshot.py`, and `pathspace_paint_screenshot_card` pinned to declarative namespaces; no remaining legacy usage. | - CTest target `PaintExampleScreenshot` must pass under the 5× loop.<br>- `/diagnostics/ui/paint_example/screenshot_baseline/*` + `widgets/<id>/metrics/history_binding/*` stay populated during runs. | 2025‑11‑26 |

## Watchpoints & Alerts
- **Legacy builder telemetry:** `/_system/diagnostics/legacy_widget_builders/<entry>/usage_total` must remain at **0** before we flip `PATHSPACE_LEGACY_WIDGET_BUILDERS=error` globally (deadline: February 1, 2026). Investigate any non-zero spike immediately and update this tracker with the offending consumer.
- **Scene readiness guard:** Downstream tools should depend on `PathSpaceExamples::ensure_declarative_scene_ready` (documented in `docs/Memory.md`) to guarantee buckets exist before presenting. Note which consumers do not yet invoke the helper and file work items.
- **JSON / distributed prerequisites:** The inspector and web adapter rows stay “pending” until the JSON serializer and distributed mount phases listed in their respective plans land. Keep their blockers in sync with `docs/Plan_Distributed_PathSpace.md` milestones.

## Update Process
1. When a consumer changes status (prototype lands, migration complete, blocker discovered), update the row above and record the new verification date.
2. If a new consumer appears in any plan, add a row referencing the plan section and describe how we will detect legacy usage.
3. Keep `docs/Plan_WidgetDeclarativeAPI.md` Phase 3 in sync with this tracker (add a changelog bullet with the new verification date).
4. Mention tracker updates in `docs/Memory.md` so future maintainers know why a status changed.

_Questions or new consumers to track? Mention them in `docs/Plan_WidgetDeclarativeAPI.md` Phase 3 and link back here so the list stays authoritative._
