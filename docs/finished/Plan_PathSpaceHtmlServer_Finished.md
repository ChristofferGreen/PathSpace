# Plan: PathSpace HTML Server Embedding

> **Owner:** PathSpace web adapter maintainers  
> **Drafted:** December 3, 2025  
> **Status:** Complete — Phases 0–5 landed (PathSpaceHtmlServer.hpp + lifecycle/unit coverage, RemoteMountSource health gating, HtmlMirror bootstrap wiring, paint/widgets migrated to the builder with `--html-server`, docs/tooling refreshed, and CI now exercises both the standalone ServeHtml binary and the embedded helper).

## Motivation
ServeHtml now powers `/apps/<app>/<view>` for the paint and widgets examples, but each caller wires it up manually: attach HTML renderer targets, seed demo credentials, then spawn `RunServeHtmlServer` with ad-hoc CLI options and threads. This makes it cumbersome for other PathSpace apps to expose HTML mirrors, and duplicative boilerplate keeps creeping into examples and docs. We want a first-class, embeddable server helper—`PathSpaceHtmlServer<Space>`—that any app can instantiate with a few lines of code to serve HTML surfaces alongside its native UI. Embedding also unlocks richer integrations (shared logging, metrics, shutdown ordering) without juggling separate binaries.

## Goals
1. Expose an embed-friendly HTML server facade (templated on a PathSpace-compatible type) that owns ServeHtml modules, runs on caller-managed threads, and forwards `read/insert/listChildren` to the host space.
2. Provide a high-level helper that registers HTML mirror targets (scene/view mapping, renderer options) without duplicating the readiness plumbing from `declarative_example_shared`.
3. Refactor `paint_example` and `widgets_example` to use the new helper, deleting their bespoke `--serve-html*` command-line handling and thread wrapper.
4. Offer a header-only convenience wrapper (`PathSpaceHtmlServer::Builder`) so third-party apps can embed the server with sensible defaults, while still exposing advanced knobs (auth, session store, SSE, ops API) via the options struct.
5. Update documentation (README, AI_Debugging_Playbook, Plan Overview) and tooling references to point at the new embed workflow instead of the legacy CLI flags.

## Non-Goals
- Replacing the standalone `pathspace_serve_html` binary (it stays for ops and CI coverage).
- Implementing new HTTP features (inspector proxying, CDN sync). Those remain separate follow-ups.
- Changing the declarative renderer contracts; the helper simply wires existing HTML targets.

## Current State Snapshot
- `include/pathspace/web/PathSpaceHtmlServer.hpp` now hosts a header-only wrapper (`PathSpaceHtmlServer<Space>`) plus a nested `Builder`. It validates `ServeHtmlOptions`, starts/stops the server on a caller-managed thread, exposes an injectable launcher so tests and embeddings can avoid the CLI binary path, and auto-assigns a random port when `serve_html.port == 0`.
- `tests/unit/web/test_PathSpaceHtmlServer.cpp` covers option validation, lifecycle (start/stop/restart), remote mounts, forwarding helpers, port auto-selection, and HTML mirror bootstrapping using injected launchers and the ServeHtml stop flag.
- Default launching now routes through `RunServeHtmlServerWithStopFlag`, which wires the modular controllers/session store/SSE broadcaster with a per-instance stop flag, optional log hooks, and readiness/error callbacks so embedders can report bind failures without relying on the global stop flag.
- PathSpaceHtmlServer accepts a `RemoteMountSource` (or legacy `remote_mount_alias`), prefixes ServeHtml roots under `/remote/<alias>`, optionally checks `/inspector/metrics/remotes/<alias>/client/connected` before launching, and can seed demo credentials via `seed_demo_credentials`.
- PathSpaceHtmlServer can attach default HTML mirror targets through `HtmlMirrorBootstrap` + `attach_default_targets`, optionally calls `PresentHtmlMirror` once on startup, and exposes the resolved mirror context and options for embedders.
- Forwarding helpers (`forward_insert`/`forward_read`/`forward_list_children`) reuse the remote mount alias/health gating so embedders and tests can call through the helper without duplicating path rewriting; doctests now cover local and remote forwarding paths.
- `paint_example` and `widgets_example` now expose a single `--html-server` toggle that builds a `PathSpaceHtmlServer` with HtmlMirror bootstrap + demo credential seeding. Ports default to `0` (auto-assign), host to `127.0.0.1`, and the helper mirrors presents without bespoke CLI plumbing.
- The legacy `--serve-html*` flags and inline `StartServeHtmlServer` wrapper were removed from `examples/declarative_example_shared.hpp`; shutdown now flows through `PathSpaceHtmlServer::stop()` before tearing down the declarative runtime.

## Proposed Architecture
```
include/pathspace/web/PathSpaceHtmlServer.hpp
└── template<class Space>
    class PathSpaceHtmlServer {
        public:
            PathSpaceHtmlServer(Space& space, HtmlServerOptions options);
            void start();
            void stop();
            bool isRunning() const;
            Space& space();
        private:
            std::unique_ptr<ServeHtml::Router> router_;
            std::thread server_thread_;
            // Injected dependencies: session store, SSE broadcaster, etc.
    };
```
- `HtmlServerOptions` mirrors current CLI flags but adds programmatic defaults (host `127.0.0.1`, random port when 0, `auth_mode=Demo` for examples).
- Provide helper functions:
  - `CreateHtmlMirrorTargets(space, HtmlMirrorConfig)` returns handles for `window/view` combos and ensures renderer readiness.
  - `SeedDemoCredentials(space, DemoSeedOptions)` used by examples/tests.
- Modularization prep ensures controllers and stores accept an abstract `PathSpaceFacade`, so the template simply satisfies that interface.

## Workstreams
### Phase 0 – API Design (0.5 day)
- Draft header (`PathSpaceHtmlServer.hpp`) with builder pattern and document lifecycle.
- Define `HtmlServerOptions` and `HtmlMirrorConfig` structs.
- Capture examples of embedding in docs to validate ergonomics.
- **Status (December 11, 2025):** PathSpaceHtmlServer.hpp landed with a template wrapper, start/stop lifecycle, injectable launcher, auto port selection (`port=0` → random), and HTML mirror bootstrap options. Modular ServeHtml assembly remains next.

### Phase 1 – Minimal Embeddable Server (1.5 days)
- Implement `PathSpaceHtmlServer<Space>` using modular ServeHtml components (Router, Controllers, SessionStore).
- Support start/stop, error reporting, and optional `std::function` hooks for logging.
- Add unit tests (with a fake `Space`) to validate lifecycle and request forwarding stubs.
- Introduce an optional `RemoteMountSource` config so the helper can resolve `/remote/<alias>/renderers/...` through `RemoteMountManager`, surface heartbeat/errors, and fall back cleanly when the distributed mount disconnects. The Phase 0 manager now lives in `src/pathspace/distributed/RemoteMountManager.{hpp,cpp}` so the helper can pull client metrics from `/inspector/metrics/remotes/<alias>/client/*` before deciding whether to retry or fail fast.
- **Status (December 11, 2025):** Remote mount normalization/health gating, demo credential seeding, forwarding helpers, random port selection, attach_default_targets wiring (HtmlMirrorBootstrap + present-on-start), and the stop-flag/log-hook aware `RunServeHtmlServerWithStopFlag` path with listen failure surfacing shipped in PathSpaceHtmlServer with unit coverage. Example refactors remain.

### Phase 2 – Mirror Helper Integration (1 day)
- Move Html mirror wiring from `declarative_example_shared.hpp` into reusable helpers (exposed via new header).
- Ensure helper validates renderer/view existence and raises structured errors.

- **Status (December 11, 2025):** `include/pathspace/web/HtmlMirror.hpp` now exposes `CreateHtmlMirrorTargets`/`PresentHtmlMirror` plus a `HtmlMirrorContext` that carries renderer/target handles; doctest coverage lives in `tests/unit/web/test_HtmlMirror.cpp`. PathSpaceHtmlServer now consumes these helpers for default wiring; paint/widgets examples still call through the shared helper and need refactors.

### Phase 3 – Refactor Examples (1.5 days)
- Remove `--serve-html*` CLI flags from `paint_example` and `widgets_example`.
- Replace manual thread management with `PathSpaceHtmlServer` start/stop (tied to headless flag or new `--html-server` toggle if needed).
- Update documentation/examples to describe the new entry points.
- **Status (December 11, 2025):** `paint_example`/`widgets_example` now use `--html-server` to build `PathSpaceHtmlServer` with HtmlMirror bootstrap + demo credentials; bespoke CLI flags and the shared Start/Stop helpers have been removed. Docs/tooling updates remain.

### Phase 4 – Docs & Tooling (0.5 day)
- README: replace “run `--serve-html`” instructions with embedding snippet.
- `docs/AI_Debugging_Playbook.md`: update the ServeHtml troubleshooting section to mention the embeddable server and how to enable it in apps.
- `docs/Plan_Overview.md` + `docs/Memory.md`: record plan + status.
- **Status (December 11, 2025):** README now shows the embed-first helper snippet, `docs/AI_Debugging_Playbook.md` documents the embedded workflow, and Plan_Overview/Memory capture Phase 4 completion; Phase 5 cleanup/CI remains.

### Phase 5 – Cleanup (0.5 day)
- Delete legacy helpers (`StartServeHtmlServer`/`StopServeHtmlServer`) and related CLI flags.
- Ensure CI tests still drive the standalone binary and add a smoke test that embeds the server inside a minimal app (maybe reuse existing Python harness with a new fixture).
- **Status (December 11, 2025):** Legacy helpers were removed in earlier phases; CTest now wires the Python integration suites (`PathSpaceServeHtmlHttp`, `PathSpaceServeHtmlSse`, `PathSpaceServeHtmlGoogle`) plus a new embedded smoke run (`PathSpaceHtmlServerEmbed`) that launches `pathspace_html_server_embed` with the demo seed. CI coverage aligns with the helper.

## Risks & Mitigations
| Risk | Mitigation |
| --- | --- |
| Template bloat / ODR issues | Keep template header thin; implement logic in non-templated helpers that accept an abstract interface. |
| Examples lose configurability | Provide optional builder overrides (port, auth mode) so behavior stays tunable without CLI flags. |
| Lifecycle bugs (server referencing destroyed PathSpace) | Document start/stop order, add RAII wrapper, and ensure `stop()` joins the thread before `PathSpace` teardown. |
| Test coverage gaps | Add a dedicated integration test that embeds the server in-process and hits `/apps/...` via Python harness. |

## Deliverables
- `include/pathspace/web/PathSpaceHtmlServer.hpp` + implementation files.
- Refactored paint/widgets examples using the new helper.
- Updated docs + plan references; removal of legacy helper APIs.
- Unit/integration tests covering the embedded workflow.

## Timeline
Approximately 4–5 engineering days after the ServeHtml modularization base lands (can overlap Phase 1 config extraction with modularization if desired).

## Dependencies
- ServeHtml modularization (interfaces + router extraction) should be in progress or complete to avoid duplicating logic.
- Existing integration tests (`test_pathspace_serve_html_*`) remain the regression suite; new embedded test adds coverage once helper ships.
- Distributed PathSpace client/server work (`docs/finished/Plan_Distributed_PathSpace_Finished.md`) supplies the `RemoteMountManager` aliasing, TLS/auth handshake, and wait/notify bridge required when the HTML server embeds a remote space; keep options and diagnostics in sync with that plan.

## Distributed Mount Alignment
- `PathSpaceHtmlServer` must accept either a direct `Space&` or a remote alias resolved by the distributed mount client. Startup fails fast when `options.remote_mount` is set but the alias cannot be mounted or authenticated.
- When running against `/remote/<alias>/…`, the helper only issues read/wait/take calls and lets `RemoteMountManager` enforce the single-writer rule. Notifications stay routed through the distributed client so local waiters wake the same way as native mounts.
- Surface mount health (session state, TLS peer, latency/timeout metrics) alongside ServeHtml logs/metrics so operators can attribute outages to either the HTML server or the remote PathSpace export.
- Document the exact HTML namespace expectations (`/remote/<alias>/renderers/<rid>/targets/html/<view>/output/v1/*`) and link back to `docs/finished/Plan_Distributed_PathSpace_Finished.md` so future updates keep both plans aligned.

## Exit Criteria
- Examples and future apps call the embeddable helper instead of manual threading/CLI plumbing.
- Legacy helper functions + CLI flags removed.
- Documentation reflects the new workflow, and tests cover both standalone and embedded server modes.
- **Met (December 11, 2025):** Paint/widgets run through the helper, legacy flags/helpers are gone, docs reference the embed-first flow, and CI runs ServeHtml + embedded smoke tests.
