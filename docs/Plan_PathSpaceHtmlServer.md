# Plan: PathSpace HTML Server Embedding

> **Owner:** PathSpace web adapter maintainers  
> **Drafted:** December 3, 2025  
> **Status:** Draft — pending execution after ServeHtml modularization groundwork.

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
- `StartServeHtmlServer`/`StopServeHtmlServer` in `examples/declarative_example_shared.hpp` spawn a std::thread that calls `RunServeHtmlServer` with a bespoke options struct. Consumers must:
  - Configure renderer targets manually (via Html mirror helpers).
  - Seed credentials via `EnsureUserPassword`.
  - Pass CLI-derived host/port/flag values.
- Examples duplicate CLI flags (`--serve-html-host`, `--serve-html-view`, etc.) and imperative error handling.
- Shutdown ordering is manual; server threads may outlive the PathSpace they reference if callers forget to join them before `ShutdownDeclarativeRuntime`.

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

### Phase 1 – Minimal Embeddable Server (1.5 days)
- Implement `PathSpaceHtmlServer<Space>` using modular ServeHtml components (Router, Controllers, SessionStore).
- Support start/stop, error reporting, and optional `std::function` hooks for logging.
- Add unit tests (with a fake `Space`) to validate lifecycle and request forwarding stubs.

### Phase 2 – Mirror Helper Integration (1 day)
- Move Html mirror wiring from `declarative_example_shared.hpp` into reusable helpers (exposed via new header).
- Ensure helper validates renderer/view existence and raises structured errors.

### Phase 3 – Refactor Examples (1.5 days)
- Remove `--serve-html*` CLI flags from `paint_example` and `widgets_example`.
- Replace manual thread management with `PathSpaceHtmlServer` start/stop (tied to headless flag or new `--html-server` toggle if needed).
- Update documentation/examples to describe the new entry points.

### Phase 4 – Docs & Tooling (0.5 day)
- README: replace “run `--serve-html`” instructions with embedding snippet.
- `docs/AI_Debugging_Playbook.md`: update the ServeHtml troubleshooting section to mention the embeddable server and how to enable it in apps.
- `docs/Plan_Overview.md` + `docs/Memory.md`: record plan + status.

### Phase 5 – Cleanup (0.5 day)
- Delete legacy helpers (`StartServeHtmlServer`/`StopServeHtmlServer`) and related CLI flags.
- Ensure CI tests still drive the standalone binary and add a smoke test that embeds the server inside a minimal app (maybe reuse existing Python harness with a new fixture).

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

## Exit Criteria
- Examples and future apps call the embeddable helper instead of manual threading/CLI plumbing.
- Legacy helper functions + CLI flags removed.
- Documentation reflects the new workflow, and tests cover both standalone and embedded server modes.
