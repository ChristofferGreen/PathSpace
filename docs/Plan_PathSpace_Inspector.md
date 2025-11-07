# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for the PathSpace inspector (live web-based introspection).

# PathSpace Inspector Plan

## Goals
- Provide a browser-based inspector that visualizes PathSpace subtrees in real time.
- Support developers debugging native or remote PathSpace applications without instrumenting code.
- Start as read-only (Phase 0/1); later consider limited write/edit actions with explicit safeguards.

## Dependencies
- **JSON Serialization Support:** PathSpace must export subtrees as JSON (see task “JSON Serialization Support”).
- **Distributed PathSpace Mounts:** Web server should access remote application roots securely (Plan_Distributed_PathSpace.md).
- **Web Server Infrastructure:** Inspector API rides on the web server adapter; implementation begins after baseline web endpoints (auth, SSE, `/api/path`) are in place.
- **Auth/ACLs:** Reuse web server sessions; ensure inspector respects app-root boundaries. Allow an explicit “root” role for trusted users to view the entire `/` tree.
- **Undo History Telemetry:** `_history/stats/*` nodes expose versioned binary-backed metrics; the inspector should surface these values and document the codec linkage so on-disk history aligns with UI diagnostics. See `docs/Plan_PathSpace_UndoHistory.md` (“Tooling & Debugging”) for sample payloads and CLI references. `pathspace_history_inspect` now exports JSON helpers (`historyStatsToJson`, `lastOperationToJson`) so the backend can stream telemetry without re-implementing the codec, and `scripts/history_cli_roundtrip_ingest.py` produces `build/test-logs/history_cli_roundtrip/index.json` plus a matching `dashboard.html` (charts + PSHD deep links) for dashboards and inspector bootstrapping.

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
1. Finalize JSON schema and SSE diff format (coordinate with serialization task).
2. Implement Phase 0 backend endpoints and minimal UI.
3. Add backlog entries per phase (P1 for Phase 0 & 1).
4. Update Plan Overview with inspector plan entry when implementation begins.
