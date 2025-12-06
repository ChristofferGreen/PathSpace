# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for the PathSpace inspector (live web-based introspection).
> **Status (December 2, 2025):** Phases 0–4 are complete; this plan now lives under `docs/finished/` as the historical record for the inspector rollout.

# PathSpace Inspector Plan

Running /bin/bash -lc './scripts/ci_inspector_tests.sh --loop=1' in /Users/chrgre01/src/PathSpace appears to hang, avoid it.

## Goals
- Provide a browser-based inspector that visualizes PathSpace subtrees in real time.
- Support developers debugging native or remote PathSpace applications without instrumenting code.
- Start as read-only (Phase 0/1); later consider limited write/edit actions with explicit safeguards.

## Dependencies
- **JSON Serialization Support:** PathSpace now exports subtrees via `PathSpaceBase::toJSON` (backed by `PathSpaceJsonExporter`). Inspector endpoints should call the helper instead of duplicating trie access.
- **Distributed PathSpace Mounts:** Web server should access remote application roots securely (`docs/finished/Plan_Distributed_PathSpace_Finished.md`).
- **Web Server Infrastructure:** Inspector API rides on the web server adapter; implementation begins after baseline web endpoints (auth, SSE, `/api/path`) are in place.
- **Auth/ACLs:** Reuse web server sessions; ensure inspector respects app-root boundaries. Allow an explicit “root” role for trusted users to view the entire `/` tree.
- **Undo History Telemetry:** `_history/stats/*` nodes expose versioned binary-backed metrics; the inspector should surface these values and document the codec linkage so on-disk history aligns with UI diagnostics. See `docs/finished/Plan_PathSpace_UndoHistory_Finished.md` (“Tooling & Debugging”) for sample payloads and CLI references. `pathspace_history_inspect` now exports JSON helpers (`historyStatsToJson`, `lastOperationToJson`) so the backend can stream telemetry without re-implementing the codec, and `scripts/history_cli_roundtrip_ingest.py` produces `build/test-logs/history_cli_roundtrip/index.json` plus a matching `dashboard.html` (charts + PSJL deep links) for dashboards and inspector bootstrapping.
- **Undo History Telemetry:** `_history/stats/*` nodes expose versioned binary-backed metrics; the inspector should surface these values and document the codec linkage so on-disk history aligns with UI diagnostics. See `docs/finished/Plan_PathSpace_UndoHistory_Finished.md` (“Tooling & Debugging”) for sample payloads and CLI references. `pathspace_history_inspect` now exports JSON helpers (`historyStatsToJson`, `lastOperationToJson`) so the backend can stream telemetry without re-implementing the codec, and `scripts/history_cli_roundtrip_ingest.py` produces `build/test-logs/history_cli_roundtrip/index.json` plus a matching `dashboard.html` (charts + PSJL deep links) for dashboards and inspector bootstrapping. Declarative paint samples also emit `widgets/<id>/metrics/history_binding/*` (state, timestamps, undo/redo counters, last error) plus a compiled `card` (`HistoryBindingTelemetryCard`) so the inspector UI can light up a ready/error badge without replaying screenshots.
- **Paint Example Screenshot Telemetry:** `examples/paint_example.cpp` now mirrors its baseline manifest (`width`, `height`, `renderer`, `captured_at`, `commit`, `notes`, `sha256`, `tolerance`) and `last_run/*` stats (`status`, timestamp, hardware capture flag, mean error, diff artifact) under `diagnostics/ui/paint_example/screenshot_baseline/*` every time the screenshot harness runs (see `scripts/check_paint_screenshot.py`). Each run also writes the same payload to JSON via `--screenshot-metrics-json`, and `scripts/paint_example_diagnostics_ingest.py` aggregates the files into dashboard/inspector-ready summaries (`tests/tools/test_paint_example_diagnostics_ingest.py` guards the ingest path). The inspector should surface this card so regressions are visible without diffing PNGs.
- **Screenshot Card Builder:** `src/pathspace/inspector/PaintScreenshotCard.{hpp,cpp}` exposes `SP::Inspector::BuildPaintScreenshotCard` to read `/diagnostics/ui/paint_example/screenshot_baseline/*` (with JSON fallbacks) and classify severity/tolerance. `pathspace_paint_screenshot_card --metrics-json build/test-logs/paint_example/diagnostics.json` exercises the same code path for CLI/dashboards; reuse the helper whenever a panel needs to surface the card, but it’s fine to keep relying on the existing Python proxy until a broader web adapter backlog is prioritized.

## Status Update — December 1, 2025
- `InspectorSnapshot` (C++ API + JSON serializer) now walks declarative PathSpace trees with depth/child limits so we can serve canonical widget and diagnostics nodes without touching legacy builders.
- **Update (November 30, 2025):** The snapshot builder is fully backed by `PathSpaceBase::visit`/`ValueHandle`, so the inspector no longer calls `listChildren` or touches `Node*` internals. Nested spaces inherit the same traversal limits, keeping the HTTP endpoints aligned with the public API.
- **Update (November 30, 2025):** `pathspace_dump_json` ships as a supported CLI (demo seeding flag, depth/child/value toggles) plus a fixture-backed regression test, giving the inspector/web plans a canonical JSON exporter to reference outside the server.
- **Update (December 1, 2025):** `/inspector/metrics/search` (GET + POST) now records query totals, latency, and truncation counts alongside watchlist status mixes under `/inspector/metrics/search/*`. The SPA reports each search/watch evaluation, polls the endpoint for badges, and highlights ACL hits or truncated result sets without scraping logs.
- **Update (December 1, 2025):** Search/watch metrics now keep `watch.total` aligned with the SPA by excluding `out_of_scope`, and `BuildInspectorStreamDelta` collapses descendant removals before emitting SSE deltas. The inspector doctests no longer flag duplicate removals or mismatched totals once the server + tests honor the revised semantics.
- **Update (December 1, 2025):** Persistent watchlists/bookmarks landed. The backend stores saved sets under `/inspector/user/<id>/watchlists/*` (retiring entries relocate to `/inspector/user/<id>/watchlists_trash/*`) and exposes CRUD/import/export endpoints (`/inspector/watchlists`, `/inspector/watchlists/export`, `/inspector/watchlists/import`). The SPA now lets operators save/load/delete/export watchlists, import JSON exports, and syncs the saved sets directly in the watchlist panel.
- **Update (December 6, 2025):** Each saved watchlist now owns a nested `PathSpace` at `/watchlists/<id>/space` (`/meta/{id,name,count,created_ms,updated_ms,version}` + `/paths`). Legacy JSON nodes auto-migrate, and `DELETE /inspector/watchlists?id=<id>` moves the nested `/space` via `take<std::unique_ptr<PathSpace>>` so trash entries retain the structured layout.
- **Update (December 1, 2025):** Admin write toggles + auditing shipped — `InspectorHttpServer::Options::write_toggles` defines allowed flag flips/resets, `/inspector/actions/toggles` enforces allowed roles and confirmation headers, and the SPA adds an opt-in “Enable session writes” gate. Every attempt is recorded under `/diagnostics/web/inspector/audit_log/{total,last/*,events/*}` with role/user/client metadata so rollbacks reference a single PathSpace subtree instead of scraping HTTP logs.
- `InspectorHttpServer` wraps the snapshot builder plus `BuildPaintScreenshotCard` behind `/inspector/tree`, `/inspector/node`, and `/inspector/cards/paint-example` HTTP endpoints (blocking GET, JSON responses, percent-encoded `root` support). The server embeds cleanly inside any process that already owns a `PathSpace`.
- ✅ (December 1, 2025): `/inspector/stream` now emits snapshot/delta SSE events using the finalized JSON schema, and the bundled SPA autoconnects, applies incremental diffs, and surfaces streaming status so manual refreshes are no longer required for steady-state monitoring. Details live in the “`/inspector/stream` JSON Schema” section below.
- ✅ (December 1, 2025): The SPA’s new search + watchlist panes consume the SSE feed (with manual-refresh fallback when EventSource is unavailable), cap search results at 200 matches, and classify watched paths as live/missing/truncated/out-of-scope so teams can pin critical nodes without polling `/inspector/tree`.
- ✅ (December 1, 2025): SSE backpressure + diagnostics landed — `InspectorHttpServer::Options::stream` now exposes `max_pending_events`, `max_events_per_tick`, and `idle_timeout`, each session enforces the budget, and `/inspector/metrics/stream` plus `/inspector/metrics/stream/*` PathSpace nodes publish queue depth, drops, snapshot resends, and disconnect reasons. The SPA’s new “Stream health” panel polls the endpoint so dashboards/operators can spot saturation without tailing logs.
- ✅ (December 1, 2025): Remote mount aggregation shipped — `RemoteMountManager` polls distributed mounts declared via `InspectorHttpServer::Options::remote_mounts`, mirrors their snapshots under `/remote/<alias>`, publishes mount-health placeholders when links go offline, and feeds the combined data back into `/inspector/tree`, `/inspector/node`, and `/inspector/stream` so browser tabs see local + remote deltas without opening extra SSE connections.
- ✅ (December 1, 2025): Multi-root picker + `/inspector/remotes` shipped — the backend now exposes remote mount metadata (alias, path, connection state, `access_hint`, last update) and the SPA consumes it to render a quick-root dropdown, status pills, and hover hints, persisting the selection via query parameters and `localStorage` so operators stick to their preferred subtree between sessions.
- ✅ (December 1, 2025): Playwright/headless automation now exercises the SSE (streaming) and manual-refresh inspector flows via `tests/inspector/playwright/tests/inspector-sse.spec.js` and `inspector-manual.spec.js`. The suite validates search-driven watch insertion, badge transitions (live/missing/truncated/out_of_scope), and watch detail updates, and `scripts/ci_inspector_tests.sh --loop=5` keeps it wired into `scripts/workflow_commit.sh` so every release loop runs the coverage.
- The bundled SPA now ships with the server: `/` serves the tree/detail/search/watchlist UI (root/depth controls, node viewer, paint screenshot card) that talks directly to the JSON endpoints. Custom assets can be mounted with `InspectorHttpServer::Options::ui_root` or `pathspace_inspector_server --ui-root <dir>`, and `--no-ui` disables static serving when embedding the server behind an existing site.
- `pathspace_inspector_server` is the thin CLI host (localhost binding, Ctrl+C shutdown) that seeds demo data when launched with no arguments. Apps should embed the server directly instead of shelling into the CLI, but the executable keeps tests and demos simple.
- Phase 0 scope is complete: backend JSON + manual refresh endpoints exist, the SPA is served directly by `InspectorHttpServer`, configs live in code (`InspectorHttpServer::Options`), and tests cover both the snapshot builder and HTTP surface. Phase 1 onwards track SSE/search/distributed improvements.

### Immediate Follow-ups
1. Wire downstream dashboards to `/inspector/cards/paint-example` and retire the Python proxy once consumers migrate.

## Architecture Overview

```
PathSpace App ──(distributed mount)──> Web Server Inspector API ──SSE/REST──> Browser Inspector UI
```

### Backend (Inspector API)
- Lives alongside the web server adapter (and ships after its initial implementation). Expose:
  - `GET /inspector/tree?root=<path>&depth=N` — initial JSON snapshot.
  - `GET /inspector/node/<encoded-path>` — full detail for a node.
  - `GET /inspector/stream?root=<path>` — SSE channel streaming incremental updates (add/update/remove).
  - `GET /inspector/metrics/stream` — exposes live queue/dropped/resend/disconnect counters (mirrored under `/inspector/metrics/stream/*`).
  - `GET /inspector/metrics/search` / `POST /inspector/metrics/search` — retrieves and records query totals/latencies/truncation plus watchlist state mixes, mirrored under `/inspector/metrics/search/*` for dashboards that poll PathSpace directly.
  - `GET /inspector/remotes` — lists configured remote mounts (alias, `/remote/<alias>` path, `connected`, `message`, `access_hint`, last update epoch). The SPA’s root picker consumes this endpoint to populate dropdown options, remote status pills, and hover hints.
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

## `/inspector/stream` JSON Schema
- **Endpoint:** `GET /inspector/stream` (SSE, `text/event-stream`). Query parameters mirror `/inspector/tree` and now include optional `poll_ms` (minimum 100 ms), `keepalive_ms` (minimum 1000 ms), and `include_values` so callers can tune depth/child sampling per subscription.
- **Snapshot event:** Each connection begins with `event: snapshot` and a payload identical to `/inspector/tree`, wrapped with event metadata and a monotonically increasing `version`.
  ```json
  {
    "event": "snapshot",
    "version": 1,
    "options": {"root": "/widgets", "max_depth": 2, "max_children": 64, "include_values": true},
    "root": {"path": "/widgets", "value_type": "object", "child_count": 4, "children": [ /* ... */ ]},
    "diagnostics": []
  }
  ```
- **Delta event:** Subsequent notifications emit `event: delta` with the same metadata plus a `changes` object:
  - `changes.added` — array of `InspectorNodeSummary` objects for new subtrees (clients replace/insert these whole subtrees).
  - `changes.updated` — `InspectorNodeSummary` objects whose fingerprints changed (value summaries or descendant structure mutated).
  - `changes.removed` — array of path strings pruned from the monitored root.
- **Keep-alives & errors:** When no delta fires before `keepalive_ms`, the server writes `: keep-alive <version>` comments so intermediaries do not time out the stream. Failures emit `event: error` with `{ "error": "inspector_stream_failure", "message": "..." }`. The SPA now surfaces “Streaming”, “Live updates”, and “Stream disconnected” badges based on these events and falls back to manual refresh if EventSource is unavailable.

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
- ✅ (December 1, 2025) `/inspector/stream` powers the SPA’s live search/watchlist panes with manual refresh as a fallback for EventSource-disabled environments.
- Implement SSE rate limiting/backpressure plus per-client diagnostics (queue depth, dropped updates) so long-lived subscriptions stay healthy under load.
- Add Playwright/headless coverage that exercises incremental updates, search filtering, and watchlist badge transitions while the server replays demo deltas.
- Publish backlog items for distributed mounts/ACL gating to tee up the multi-root work once Phase 1 stabilizes.

#### Phase 1 Backlog
- ✅ (December 1, 2025) Implement SSE rate limiting/backpressure: per-connection queues now enforce `InspectorHttpServer::Options::stream.{max_pending_events,max_events_per_tick,idle_timeout}`, overflow triggers snapshot resends + optional disconnects, `/inspector/metrics/stream` mirrors the counters (also published under `/inspector/metrics/stream/*`), and the SPA’s stream-health panel surfaces queue depth, drops, resends, and disconnect reasons.
- ✅ (December 1, 2025) Publish search/watchlist diagnostics: `/inspector/metrics/search` (GET + POST) now mirrors the `/inspector/metrics/search/*` PathSpace nodes, recording query totals/latency/truncation plus watchlist status mixes and timestamps. The SPA posts query + watch summaries after each search/watch evaluation, polls the endpoint for badges, and shows live warning states for truncated results and ACL hits without tailing logs.
- ✅ (December 1, 2025) Land Playwright/headless regression coverage for search/watchlist flows: the SSE + manual suites under `tests/inspector/playwright/tests/` assert badge transitions, streaming/manual deltas, and watch detail updates, and they now run via `scripts/ci_inspector_tests.sh --loop=5` inside the required workflow.

### Phase 2 — Remote Mounts & Multi-App
- Integrate distributed mounts so inspector can target remote app roots.
- UI allows selecting which app/root to inspect (dropdown or query parameter).
- Enforce ACLs per exported subtree; handle unauthorized access gracefully.
- Expand diagnostics view (e.g., error counts, waiters).

#### Phase 2 Backlog
- ✅ (December 1, 2025) Integrate distributed PathSpace mounts: `InspectorHttpServer::Options::remote_mounts` now spins up a `RemoteMountManager` that reuses the distributed mount handshake, polls remote `/inspector/tree` endpoints, prefixes their snapshots under `/remote/<alias>`, injects mount metadata into `/inspector/tree`, and merges remote deltas into `/inspector/stream` without blocking local spaces (per-mount queues + background fetchers prevent starvation).
- ✅ (December 1, 2025) Build multi-root selection UI: the SPA now surfaces a quick-root dropdown backed by `/inspector/remotes`, persists the choice via query parameters + `localStorage`, shows per-alias status pills (online, offline, mixed), and exposes VPN/auth scope hints sourced from `RemoteMountOptions::access_hint` so operators know when a mount requires extra access.
- ✅ (December 1, 2025) Enforce ACL gating with clear UX: `InspectorHttpServer::Options::acl` now accepts per-role rules plus custom role/user headers, the backend normalizes requested roots before evaluating them, and `/inspector/tree`, `/inspector/node`, and `/inspector/stream` return structured 403 payloads (error=`inspector_acl_denied`, message, role, requested path, allowed roots). Violations increment `/diagnostics/web/inspector/acl/{violations/total,last/*}` and append JSON entries under `/diagnostics/web/inspector/acl/violations/events/*`, while the SPA highlights denied roots with red badges, disables EventSource reconnects, and surfaces the server-supplied hint in the root form and tree panel.
- ✅ (December 1, 2025) Expand remote diagnostics: each `RemoteMountManager` worker now records per-alias latency (`last/avg/max`), request counters (success, total errors, consecutive failures), and waiter depth for both HTTP + SSE remote roots. The data is published under `/inspector/metrics/remotes/<alias>/{status,latency,requests,waiters,timestamps}` and mirrored through `/inspector/remotes` (new `latency`, `requests`, `waiters`, and `health` objects) plus snapshot diagnostics so operators and dashboards can pinpoint failing links without tailing logs.

### Phase 3 — Developer Tools
- Watchlist/bookmarks for critical paths.
- Snapshot capture/export to JSON file for bug reports.
- Diff mode: compare two snapshots or revisions.
- Optional limited write actions (e.g., toggling flags) gated behind explicit opt-in and logging (scoped as future optional feature; keep default read-only).

#### Phase 3 Backlog
- ✅ (December 1, 2025) Implement persistent watchlists/bookmarks: `/inspector/watchlists`, `/inspector/watchlists/export`, `/inspector/watchlists/import`, and `/inspector/watchlists?id=<id>` now manage saved sets under `/inspector/user/<id>/watchlists/*` (retiring entries move to `/inspector/user/<id>/watchlists_trash/*`). The SPA exposes save/load/delete/import/export controls backed by these endpoints, enforces per-user limits, and mirrors the saved sets in the watchlist panel.
- ✅ (December 1, 2025) Ship snapshot capture/export/diff: `/inspector/snapshots` now captures labeled snapshots, lists saved sets, deletes stale entries, streams exports directly from `PathSpaceJsonExporter`, and `/inspector/snapshots/diff` reuses the Inspector delta schema so operators can compare revisions without scripting. The SPA’s “Snapshots” panel drives these endpoints (capture form, saved list, download + delete actions, diff viewer) and produces ready-to-attach JSON bundles for bug reports.
- ✅ (December 1, 2025) Prototype gated write toggles: `/inspector/actions/toggles` now lists admin-only actions, POST requests flip/force booleans via preconfigured allowlists, the SPA exposes an opt-in session toggle plus explicit confirmation prompts, and every attempt (success/failure) is logged under `/diagnostics/web/inspector/audit_log` with role/user/client metadata for rollback investigations.

### Phase 4 — Polish & Performance
- Optimize tree rendering (virtualization, lazy loading).
- Improve accessibility (keyboard navigation, ARIA labels).
- Refine logging/metrics for inspector usage (update via Observability section).
- Document workflows in `docs/AI_Debugging_Playbook.md`.

#### Phase 4 Backlog
- ✅ (December 1, 2025) Optimize SPA rendering paths: the inspector tree is now virtualized (windowed list with 28 px rows + overscan), SSE deltas patch the flattened tree in-place instead of rebuilding the entire DOM, and the heavy detail panes (watchlists, snapshots, write toggles, paint card) activate lazily via IntersectionObserver so they only fetch/render once the operator scrolls to them or clicks their refresh buttons. The SPA stays responsive with 100k-node trees on mid-range laptops, and manual refresh buttons still force-load panels when needed.
- ✅ (December 1, 2025) Complete the accessibility pass: the embedded SPA now exposes ARIA roles/labels on the tree/watch panels, ships a keyboard-traversable virtual tree (aria-activedescendant + roving selection), adds high-contrast focus rings for every interactive control (including remote badges), and the Playwright suite runs axe-core WCAG AA audits so regressions fail fast.
- ✅ (December 2, 2025) Expand usage telemetry: the SPA now measures per-panel dwell time via `IntersectionObserver`, posts them to `/inspector/metrics/usage`, and `UsageMetricsRecorder` streams aggregates under `/diagnostics/web/inspector/usage/*` plus `GET /inspector/metrics/usage`; the debugging playbook records how to read the counters and thresholds.

## Security Considerations
- Inspector routes require authenticated sessions; optionally restrict by role (e.g., `inspector` role).
- Support an explicit “root”/admin role that can inspect the full `/` tree; default users remain scoped to their app roots.
- When a session ends (logout, timeout, or connection drop), terminate inspector streams and allow remote applications to shut down before a new session is established.
- All responses read-only by default; future write actions must require extra confirmation or session flag.
- Log inspector actions via the new audit path (`/diagnostics/web/inspector/audit_log/{total,last/*,events/*}`) so each write toggle attempt captures role/user/client, before/after state, and outcome without tailing `app_root/io/log/web` directly.
- Limit per-user watchlists and search frequency to prevent scanning entire PathSpace without authorization.

## Observability & Testing
- Metrics: `/inspector/metrics/stream` (and the mirrored `/inspector/metrics/stream/*` PathSpace nodes) now publish queue depth, max depth seen, dropped deltas, snapshot resends, active/total sessions, and disconnect reasons so dashboards and the SPA’s stream-health panel can watch backpressure alongside the usual request/SSE latency counters.
- Search diagnostics: `/inspector/metrics/search` mirrors `/inspector/metrics/search/*` and records total queries, truncated-query counts, truncated node totals, last/average latency, and watchlist status mixes (`live`, `missing`, `truncated`, `out_of_scope`, `unknown`). The SPA POSTs `{ query, watch }` payloads after each search/watch evaluation, the PathSpace nodes store the counters for dashboards, and the UI badges highlight truncation/ACL pressure without requiring debug logs.
- Remote diagnostics: `/inspector/metrics/remotes/<alias>/{status,latency,requests,waiters,timestamps}` now tracks per-alias connection state, last/average/max latency, success/error totals, consecutive failure streaks, waiter depth/max waiter depth, last-update/error timestamps, and the configured `access_hint`. `/inspector/remotes` exposes the same data (`health`, `latency`, `requests`, `waiters`, `last_error_ms`) so the SPA, dashboards, and automation can alert on slow/offline mounts at a glance.
- ACL diagnostics: `/diagnostics/web/inspector/acl/violations/total` counts denied requests and `/diagnostics/web/inspector/acl/violations/last/*` captures the most recent event (timestamp, user header, role, endpoint, requested path, reason). Each violation also appends a JSON blob under `/diagnostics/web/inspector/acl/violations/events/<timestamp>` so dashboards can replay who attempted to leave their scoped root without scraping HTTP logs.
- Automated tests: backend unit tests (JSON serialization sanity), integration tests (CLI + headless browser), plus the inspector Playwright suite (`tests/inspector/playwright/tests/inspector-sse.spec.js` and `inspector-manual.spec.js`) executed via `scripts/ci_inspector_tests.sh` with the mandated 5× loop.
- ✅ (December 1, 2025) Added an inspector section to `docs/AI_Debugging_Playbook.md` (see §8) covering CLI usage, embedding, and SSE tuning.
- ✅ (December 2, 2025) HistoryBinding telemetry writers now drain queued values before inserting new ones (mirroring `InspectorMetricUtils`), so `/widgets/<id>/metrics/history_binding/*` and the `card` entry always surface the latest toggle/action/error state; `tests/unit/ui/test_HistoryBinding.cpp` now reads back the PathSpace nodes to guard the behavior, and `tests/unit/inspector/test_InspectorHttpServer.cpp` adds coverage for `/inspector/metrics/usage` POST/GET so the new usage recorder plus `/diagnostics/web/inspector/usage/*` nodes stay wired into the SPA + dashboards.
- **Loop failure — December 1, 2025:** The mandated `./scripts/compile.sh --clean --test --loop=5 --release` run for this work stopped in `PathSpaceTests` (`HistoryBinding updates telemetry for actions`). Follow-up plan: run the targeted suite (`ctest --test-dir build -R HistoryBinding --output-on-failure`), inspect `tests/unit/ui/test_HistoryBinding.cpp`, and verify the telemetry nodes that feed the inspector dashboards still publish the expected metrics. Capture repro logs under `test-logs/loop_failures/20251201-075018_PathSpaceTests_loop1` when triaging.
- **Loop failure — December 1, 2025 (08:50 UTC rerun):** `scripts/workflow_commit.sh` still fails in `PathSpaceTests` loop 1 with the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` subcases. Latest log: `build/test-logs/PathSpaceTests_loop1of5_20251201-085028.log`. Keep following the targeted plan above until the HistoryBinding telemetry publishes stable metrics inside the compile loop.
- **Loop failure — December 1, 2025 (PixelNoisePerfHarness):** `PixelNoisePerfHarness` timed out in loop 1 while `scripts/workflow_commit.sh` reran the mandated tests (log: `test-logs/loop_failures/20251201-083302_PixelNoisePerfHarness_loop1`). Follow-up plan: run `build/tests/PixelNoisePerfHarness --timeout 30 --max-frames 512` locally to reproduce, inspect `tests/ui/perf/PixelNoisePerfHarness.cpp` for lingering waits, and ensure the harness honors `PATHSPACE_TEST_TIMEOUT` or an internal budget so it stops before the 20 s loop guard.
- **Loop failure — December 1, 2025 (09:13 UTC rerun):** `scripts/workflow_commit.sh` still hits the `PixelNoisePerfHarness` timeout in loop 1 (`build/test-logs/PixelNoisePerfHarness_loop1of5_20251201-091308.log`, archived to `test-logs/loop_failures/20251201-091330_PixelNoisePerfHarness_loop1`). Keep following the targeted harness plan above (`build/tests/PixelNoisePerfHarness --timeout 30 --max-frames 512`, audit waits, respect `PATHSPACE_TEST_TIMEOUT`) before attempting another full loop.
- **Loop failure — December 1, 2025 (16:45 UTC rerun):** `./scripts/compile.sh --clean --test --loop=5 --release` still fails in `PathSpaceTests` loop 1 on the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` cases (latest log: `build/test-logs/PathSpaceTests_loop1of5_20251201-164534.log`). Continue tracking against the HistoryBinding telemetry plan above before retrying the full loop.
- **Loop failure — December 1, 2025 (17:07 UTC rerun):** `scripts/workflow_commit.sh` again stopped in `PathSpaceTests` loop 1 on the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` cases (log: `build/test-logs/PathSpaceTests_loop1of5_20251201-170744.log`). Keep running the targeted repro (`ctest --test-dir build -R HistoryBinding --output-on-failure`), instrument `tests/unit/ui/test_HistoryBinding.cpp`, and verify the telemetry nodes feeding the inspector dashboards publish stable metrics before the next full loop.
- **Loop failure — December 1, 2025 (21:10 UTC rerun):** `./scripts/compile.sh --clean --test --loop=5 --release` still fails in loop 2 on the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` doctests (`build/test-logs/PathSpaceTests_loop2of5_20251201-210957.log`). Keep following the targeted HistoryBinding telemetry plan above before rerunning the workflow loop.
- **Targeted validation — December 1, 2025 (20:30 UTC):** `ctest --test-dir build -R PathSpaceTests --output-on-failure` passes in ~9.2 s after realigning the inspector search metrics expectations and collapsing duplicate removals in `BuildInspectorStreamDelta`, so the inspector doctests are healthy before the next workflow loop attempt.
- **Loop failure — December 1, 2025 (20:35 UTC and 20:43 UTC reruns):** `./scripts/compile.sh --clean --test --loop=5 --release` and the subsequent `scripts/workflow_commit.sh` attempt both timed out in `PixelNoisePerfHarness` loop 1 (logs: `build/test-logs/PixelNoisePerfHarness_loop1of5_20251201-203448.log`, `build/test-logs/PixelNoisePerfHarness_loop1of5_20251201-204311.log`). Continue the harness repro plan (`build/tests/PixelNoisePerfHarness --timeout 30 --max-frames 512`, audit waits, ensure the test honors `PATHSPACE_TEST_TIMEOUT`) before retrying `scripts/workflow_commit.sh`.
- **Loop failure — December 1, 2025 (22:12 UTC rerun):** `scripts/workflow_commit.sh` still fails in `PathSpaceTests` loop 1 on the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` subcases (`build/test-logs/PathSpaceTests_loop1of5_20251201-221238.log`, archived to `test-logs/loop_failures/20251201-221248_PathSpaceTests_loop1`). Keep following the targeted HistoryBinding telemetry triage above before rerunning the full workflow loop.
- **Loop failure — December 1, 2025 (22:52 UTC rerun):** `./scripts/compile.sh --clean --test --loop=5 --release` still stops in `PathSpaceTests` loop 1 on the `HistoryBinding initializes metrics` / `HistoryBinding updates telemetry for actions` doctests (log: `build/test-logs/PathSpaceTests_loop1of5_20251201-225257.log`). Continue the existing HistoryBinding telemetry investigation plan before retrying the full loop.
- ✅ (December 1, 2025) Inspector write toggles now drain stale bool values before applying a new state, so `/inspector/actions/toggles` actually flips `/app/*` flags and the `Inspector write toggles enforce confirmation and log audits` doctest (plus the workflow loop’s `PathSpaceTests`) stay green again.

## Integration Points
- ✅ (December 2, 2025) Updated `docs/finished/Plan_WebServer_Adapter_Finished.md` with inspector SPA + `/inspector/*` endpoint coverage (SSE budgets, remote mount diagnostics, ACL wiring, and config knobs) so the web adapter ships the same introspection surface without a separate server.
- ✅ (December 2, 2025) Referenced inspector availability in `docs/finished/Plan_Distributed_PathSpace_Finished.md`, wiring the distributed mount story to the inspector plan so client tools know how `/remote/<alias>` snapshots, SSE, and remotes metrics show up in shared deployments.
- ✅ (December 2, 2025) Added a README “Inspector quick start” snippet that shows how to launch `pathspace_inspector_server`, embed `InspectorHttpServer`, and connect a browser to the running app.

## Embedding `InspectorHttpServer`

The server hosts HTTP endpoints inside any process that already owns a `PathSpace`. Typical embedding flow:

1. Include `<inspector/InspectorHttpServer.hpp>` inside the app that already wires up declarative widgets.
2. Configure `InspectorHttpServer::Options` with the desired host/port and snapshot bounds, then construct the server with the same `PathSpace` instance that backs the UI.
3. Call `start()` during startup and stash the bound port via `server.port()` for logs or health checks. The listener runs on its own thread once `start()` succeeds.
4. Call `stop()` and `join()` during shutdown so the listener drains before the `PathSpace` is torn down.

```cpp
#include "inspector/InspectorHttpServer.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace std::chrono_literals;

SP::Inspector::InspectorHttpServer::Options options;
options.host                     = "0.0.0.0";          // bind externally when exposing to browsers
options.port                     = 0;                   // 0 chooses an ephemeral port automatically
options.snapshot.root            = "/app";             // keep inspectors scoped when embedding in apps
options.snapshot.max_depth       = 3;                   // defaults: depth=2, children=32, include_values=true
options.snapshot.max_children    = 64;
options.stream.poll_interval     = 250ms;               // default 350 ms
options.stream.keepalive_interval = 4000ms;             // default 5000 ms
options.stream.max_pending_events = 64;                 // default 64
options.stream.max_events_per_tick = 8;                 // default 8
options.stream.idle_timeout       = 30000ms;            // default 30 s
options.ui_root                  = "/opt/app/inspector-ui"; // optional custom assets

SP::Inspector::InspectorHttpServer server(space, options);
if (auto started = server.start(); !started) {
    std::fprintf(stderr,
                 "Failed to start inspector: %s\n",
                 SP::describeError(started.error()).c_str());
    std::exit(1);
}

std::printf("Inspector listening on %s:%u\n",
            options.host.c_str(),
            static_cast<unsigned>(server.port()));

// Later during shutdown:
server.stop();
server.join();
```

### Options quick reference

- **`host` / `port`:** Defaults to `127.0.0.1:8765`. Set `port = 0` to request an ephemeral port (helpful when embedding multiple inspectors or letting launch scripts pick a free socket).
- **`snapshot`:** Mirrors `InspectorSnapshotOptions` (`root`, `max_depth`, `max_children`, `include_values`). Query parameters on `/inspector/tree`, `/inspector/node`, and `/inspector/stream` override these caps per request, e.g. `?root=/demo&depth=4&include_values=0`.
- **`paint_card`:** Points at `/diagnostics/ui/paint_example/screenshot_baseline/*` by default. Override `diagnostics_root` when your app stores paint metrics under a different subtree.
- **`ui_root` / `enable_ui`:** Leave `ui_root` empty to serve the embedded SPA wholesale. Set it to a filesystem directory when you want to live-reload a locally built UI bundle. Toggle `enable_ui=false` to expose only the JSON/SSE endpoints behind an existing web shell.
- **`stream`:** `poll_interval` and `keepalive_interval` default to 350 ms and 5 s (server clamps `poll_ms < 100` and `keepalive_ms < 1000`). New knobs: `idle_timeout` (30 s by default) closes stalled sessions, `max_pending_events` (64) caps each client queue before we drop + resend a snapshot, and `max_events_per_tick` (8) limits how much we flush per scheduler tick. Clients may override `poll_ms`/`keepalive_ms` via query parameters; the other limits remain server-side to keep diagnostics consistent.

### SSE and asset notes

- `/inspector/stream` emits one `snapshot` event followed by `delta` events (`changes.added/updated/removed`) every poll interval. The SPA reconnects automatically and exposes status badges; manual refresh buttons fall back to `/inspector/tree` in legacy browsers.
- `/inspector/metrics/stream` returns the live queue/dropped/resent/disconnect counters (mirrored under `/inspector/metrics/stream/*` in the inspected `PathSpace`). The SPA’s “Stream health” card polls this endpoint every 5 s so operators can see backpressure without tailing logs; dashboards can do the same or subscribe to the PathSpace nodes directly.
- Enforce scoping by setting `snapshot.root` to an app-specific prefix and documenting whether the process is allowed to expose `/`.
- When hosting assets elsewhere (reverse proxy, embedded web server), keep `enable_ui=false` and point your static host at the bundled SPA assets. Static files can also be hotloaded with `pathspace_inspector_server --ui-root <dir>` for local development.

### CLI quick start

Use the shipping `pathspace_inspector_server` binary to verify options before embedding them:

```bash
./build/pathspace_inspector_server \
  --host 0.0.0.0 \
  --port 0 \
  --root /app \
  --max-depth 3 \
  --max-children 64 \
  --diagnostics-root /diagnostics/ui/paint_example \
  --ui-root out/inspector-ui \
  --no-demo
```

- `--no-demo` skips seeding fake data when you are pointing at a live app `PathSpace`.
- `--no-ui` disables static file serving entirely.
- Pass `--ui-root <dir>` to live-reload assets or leave it unset to exercise the baked-in SPA bundle.
- `--stream-poll-ms`, `--stream-keepalive-ms`, `--stream-idle-timeout-ms`, `--stream-max-pending`, and `--stream-max-events` mirror the new `InspectorHttpServer::Options::stream` knobs so demos/tests can dial in the same budgets before embedding the server.

See the new “Inspector Embedding & Usage” section in `docs/AI_Debugging_Playbook.md` for a task-focused runbook.

## Next Actions
- ✅ (December 1, 2025) Extend the SPA with search/watchlist panes that consume the live SSE feed (manual refresh fallback retained for EventSource-disabled environments).
1. ✅ (December 1, 2025) Add backlog entries per phase (Phase 1: rate limiting/backpressure/search diagnostics; later phases: distributed mounts + ACL gating).
2. ✅ (December 1, 2025) Update Plan Overview with the search/watchlist status and document how to embed `InspectorHttpServer` (see the embedding section above plus `docs/AI_Debugging_Playbook.md`).

## Remaining TODOs (Phase 1 focus)
- ✅ (December 1, 2025) Browser automation coverage (Playwright/headless) now validates search/watchlist flows for both live SSE updates and manual-refresh-only sessions. No other Phase 1 TODOs remain; move forward with the Phase 2 backlog.
- ✅ (December 1, 2025) Inspector search/watch metrics + stream delta regressions fixed: the watch `total` counter now excludes `out_of_scope` everywhere (matching SPA badges) and `BuildInspectorStreamDelta` drops descendant removals so SSE consumers see a single entry per pruned subtree. `ctest --test-dir build -R PathSpaceTests --output-on-failure` passes in ~9.2 s on Release builds, confirming the inspector doctests are green again before rerunning the workflow loop.
- ✅ (December 1, 2025) PixelNoisePerfHarness headless frame counting fixed: `examples/pixel_noise_example.cpp` now treats headless presents as progress, so `python3 scripts/check_pixel_noise_baseline.py --build-dir build` completes in ~3 s (120 frames) and the 5× loop stays under the 20 s per-test guard. Resume the full workflow loop once the rebuilt binary is in place.
