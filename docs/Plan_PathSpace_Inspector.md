# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for the PathSpace inspector (live web-based introspection).

# PathSpace Inspector Plan

## Goals
- Provide a browser-based inspector that visualizes PathSpace subtrees in real time.
- Support developers debugging native or remote PathSpace applications without instrumenting code.
- Start as read-only (Phase 0/1); later consider limited write/edit actions with explicit safeguards.

## Dependencies
- **JSON Serialization Support:** PathSpace now exports subtrees via `PathSpaceBase::toJSON`/`writeJSONToFile` (backed by `PathSpaceJsonExporter`). Inspector endpoints should call the helper instead of duplicating trie access.
- **Distributed PathSpace Mounts:** Web server should access remote application roots securely (Plan_Distributed_PathSpace.md).
- **Web Server Infrastructure:** Inspector API rides on the web server adapter; implementation begins after baseline web endpoints (auth, SSE, `/api/path`) are in place.
- **Auth/ACLs:** Reuse web server sessions; ensure inspector respects app-root boundaries. Allow an explicit “root” role for trusted users to view the entire `/` tree.
- **Undo History Telemetry:** `_history/stats/*` nodes expose versioned binary-backed metrics; the inspector should surface these values and document the codec linkage so on-disk history aligns with UI diagnostics. See `docs/finished/Plan_PathSpace_UndoHistory_Finished.md` (“Tooling & Debugging”) for sample payloads and CLI references. `pathspace_history_inspect` now exports JSON helpers (`historyStatsToJson`, `lastOperationToJson`) so the backend can stream telemetry without re-implementing the codec, and `scripts/history_cli_roundtrip_ingest.py` produces `build/test-logs/history_cli_roundtrip/index.json` plus a matching `dashboard.html` (charts + PSJL deep links) for dashboards and inspector bootstrapping.
- **Undo History Telemetry:** `_history/stats/*` nodes expose versioned binary-backed metrics; the inspector should surface these values and document the codec linkage so on-disk history aligns with UI diagnostics. See `docs/finished/Plan_PathSpace_UndoHistory_Finished.md` (“Tooling & Debugging”) for sample payloads and CLI references. `pathspace_history_inspect` now exports JSON helpers (`historyStatsToJson`, `lastOperationToJson`) so the backend can stream telemetry without re-implementing the codec, and `scripts/history_cli_roundtrip_ingest.py` produces `build/test-logs/history_cli_roundtrip/index.json` plus a matching `dashboard.html` (charts + PSJL deep links) for dashboards and inspector bootstrapping. Declarative paint samples also emit `widgets/<id>/metrics/history_binding/*` (state, timestamps, undo/redo counters, last error) plus a compiled `card` (`HistoryBindingTelemetryCard`) so the inspector UI can light up a ready/error badge without replaying screenshots.
- **Paint Example Screenshot Telemetry:** `examples/paint_example.cpp` now mirrors its baseline manifest (`width`, `height`, `renderer`, `captured_at`, `commit`, `notes`, `sha256`, `tolerance`) and `last_run/*` stats (`status`, timestamp, hardware capture flag, mean error, diff artifact) under `diagnostics/ui/paint_example/screenshot_baseline/*` every time the screenshot harness runs (see `scripts/check_paint_screenshot.py`). Each run also writes the same payload to JSON via `--screenshot-metrics-json`, and `scripts/paint_example_diagnostics_ingest.py` aggregates the files into dashboard/inspector-ready summaries (`tests/tools/test_paint_example_diagnostics_ingest.py` guards the ingest path). The inspector should surface this card so regressions are visible without diffing PNGs.
- **Screenshot Card Builder:** `src/pathspace/inspector/PaintScreenshotCard.{hpp,cpp}` exposes `SP::Inspector::BuildPaintScreenshotCard` to read `/diagnostics/ui/paint_example/screenshot_baseline/*` (with JSON fallbacks) and classify severity/tolerance. `pathspace_paint_screenshot_card --metrics-json build/test-logs/paint_example/diagnostics.json` exercises the same code path for CLI/dashboards; reuse the helper whenever a panel needs to surface the card, but it’s fine to keep relying on the existing Python proxy until a broader web adapter backlog is prioritized.

## Status Update — November 26, 2025
- `InspectorSnapshot` (C++ API + JSON serializer) now walks declarative PathSpace trees with depth/child limits so we can serve canonical widget and diagnostics nodes without touching legacy builders.
- **Update (November 30, 2025):** The snapshot builder is fully backed by `PathSpaceBase::visit`/`ValueHandle`, so the inspector no longer calls `listChildren` or touches `Node*` internals. Nested spaces inherit the same traversal limits, keeping the HTTP endpoints aligned with the public API.
- **Update (November 30, 2025):** `pathspace_dump_json` ships as a supported CLI (demo seeding flag, depth/child/value toggles) plus a fixture-backed regression test, giving the inspector/web plans a canonical JSON exporter to reference outside the server.
- `InspectorHttpServer` wraps the snapshot builder plus `BuildPaintScreenshotCard` behind `/inspector/tree`, `/inspector/node`, and `/inspector/cards/paint-example` HTTP endpoints (blocking GET, JSON responses, percent-encoded `root` support). The server embeds cleanly inside any process that already owns a `PathSpace`.
- The bundled SPA now ships with the server: `/` serves an inline tree/detail UI (root/depth controls, node viewer, paint screenshot card) that talks directly to the JSON endpoints. Custom assets can be mounted with `InspectorHttpServer::Options::ui_root` or `pathspace_inspector_server --ui-root <dir>`, and `--no-ui` disables static serving when embedding the server behind an existing site.
- `pathspace_inspector_server` is the thin CLI host (localhost binding, Ctrl+C shutdown) that seeds demo data when launched with no arguments. Apps should embed the server directly instead of shelling into the CLI, but the executable keeps tests and demos simple.
- Phase 0 scope is complete: backend JSON + manual refresh endpoints exist, the SPA is served directly by `InspectorHttpServer`, configs live in code (`InspectorHttpServer::Options`), and tests cover both the snapshot builder and HTTP surface. Phase 1 onwards track SSE/search/distributed improvements.

### Immediate Follow-ups
1. Layer SSE + incremental diff handling into the SPA once `/inspector/stream` lands so live updates do not require manual refreshes.
2. Add search/watchlist affordances to the SPA (front-end work can start now; backend indexing lives under Phase 1).
3. Wire downstream dashboards to `/inspector/cards/paint-example` and retire the Python proxy once consumers migrate.

## Architecture Overview

```
PathSpace App ──(distributed mount)──> Web Server Inspector API ──SSE/REST──> Browser Inspector UI
```

### Backend (Inspector API)
- Lives alongside the web server adapter (and ships after its initial implementation). Expose:
  - `GET /inspector/tree?root=<path>&depth=N` — initial JSON snapshot.
  - `GET /inspector/node/<encoded-path>` — full detail for a node.
  - `GET /inspector/stream?root=<path>` — SSE channel streaming incremental updates (add/update/remove).
  - Optional `GET /inspector/search?q=...` for finding nodes by path/type/value.
- Uses JSON serializer to encode nodes (`{ path, type, value_summary, metadata, diagnostics, children }`).
- Subscribes to PathSpace wait/notify to push live updates through SSE.
- Rate-limit SSE per user and per root to prevent flooding; queue updates to maintain order.

### Frontend (Browser)
- Single-page JS app (keep dependencies light): renders tree, detail pane, search box.
- Components:
  - **Tree View:** virtualized list for large hierarchies; shows basic info (path segment, type, last update).
  - **Detail Panel:** displays full value (JSON), metadata (timestamps, waiters, diagnostics).
  - **Search/Filter:** quick filter by path or type; highlight matches in tree.
  - **Live Indicators:** highlight nodes updated in the last X seconds.
- SSE client applies incremental changes (add child, update node, remove node) with fallbacks to full refresh if a diff fails.
- Provide manual refresh button, auto-refresh toggle, and “pin”/“watch” list for frequently monitored paths.

## Phases

### Phase 0 — Local Read-Only Prototype
- Inspector API serves initial snapshot and manual refresh only (no SSE yet).
- UI renders tree + detail, supports manual refresh and path selection.
- Auth via existing session; assume local PathSpace.
- Document setup in README (e.g., `./scripts/run_inspector.sh`).
- Include the paint example screenshot card by default: reuse `SP::Inspector::BuildPaintScreenshotCard`, showing severity, tolerance, last-run metadata, and artifact links. When a live PathSpace isn’t mounted, fall back to the aggregated JSON written by `scripts/paint_example_diagnostics_ingest.py` (the CLI already covers this flow).
- Local preview now lives at `http://localhost:8765/` via `tools/inspector_server`. `/` serves the bundled SPA, `/inspector/*` exposes JSON endpoints, and `--ui-root <dir>` allows hot-reloading custom assets. The legacy `scripts/paint_example_inspector_panel.py` still proxies the paint screenshot card for SSE prototyping but is no longer required for standard workflows.
- ✅ (November 26, 2025) `InspectorHttpServer`, the SPA, and the `pathspace_inspector_server` demo host now cover the entirety of Phase 0. Apps embed the server alongside their declarative PathSpace roots, the CLI seeds demo data for quick manual testing, and documentation now points at the bundled UI instead of the Python proxy.

### Phase 1 — Live Updates & Search
- Add SSE streaming for incremental updates (insert/update/remove).
- UI highlights updates, provides “recent changes” view, and basic search/filter.
- Implement rate limiting/backpressure on SSE.
- Add tests (Playwright/headless) to verify incremental updates and search.

### Phase 2 — Remote Mounts & Multi-App
- Integrate distributed mounts so inspector can target remote app roots.
- UI allows selecting which app/root to inspect (dropdown or query parameter).
- Enforce ACLs per exported subtree; handle unauthorized access gracefully.
- Expand diagnostics view (e.g., error counts, waiters).

### Phase 3 — Developer Tools
- Watchlist/bookmarks for critical paths.
- Snapshot capture/export to JSON file for bug reports.
- Diff mode: compare two snapshots or revisions.
- Optional limited write actions (e.g., toggling flags) gated behind explicit opt-in and logging (scoped as future optional feature; keep default read-only).

### Phase 4 — Polish & Performance
- Optimize tree rendering (virtualization, lazy loading).
- Improve accessibility (keyboard navigation, ARIA labels).
- Refine logging/metrics for inspector usage (update via Observability section).
- Document workflows in `docs/AI_Debugging_Playbook.md`.

## Security Considerations
- Inspector routes require authenticated sessions; optionally restrict by role (e.g., `inspector` role).
- Support an explicit “root”/admin role that can inspect the full `/` tree; default users remain scoped to their app roots.
- When a session ends (logout, timeout, or connection drop), terminate inspector streams and allow remote applications to shut down before a new session is established.
- All responses read-only by default; future write actions must require extra confirmation or session flag.
- Log inspector actions to `app_root/io/log/web` with user, path, action.
- Limit per-user watchlists and search frequency to prevent scanning entire PathSpace without authorization.

## Observability & Testing
- Metrics: inspector requests latency, SSE events delivered, queue depth, dropped updates.
- Automated tests: backend unit tests (JSON serialization sanity), integration tests (CLI + headless browser).
- Add inspector section to `docs/AI_Debugging_Playbook.md` with usage examples.

## Integration Points
- Update `docs/Plan_WebServer_Adapter.md` once inspector endpoints exist.
- Reference inspector availability in `docs/Plan_Distributed_PathSpace.md` (client tools).
- Add README snippet showing how to launch the inspector and connect to a running app.

## Next Actions
1. Finalize JSON schema + SSE diff format (coordinate with serialization task) so `/inspector/stream` can mirror `/inspector/tree`.
2. Extend the SPA with search/watchlist panes and wire it to the upcoming SSE endpoint (fall back to polling when SSE is disabled).
3. Add backlog entries per phase (P1 for Phase 1 work items: SSE, search, watchlists).
4. Update Plan Overview with inspector availability once the UI + SSE path lands and document how to embed `InspectorHttpServer` inside apps (including `--ui-root` guidance).
