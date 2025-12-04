# Plan: ServeHtml Modularization

> **Owner:** PathSpace web adapter maintainers  
> **Drafted:** December 3, 2025  
> **Status:** Finished — December 4, 2025. All phases shipped and the plan is
> archived under `docs/finished/Plan_ServeHtml_Modularization_Finished.md`.

## Motivation
`src/pathspace/web/ServeHtmlServer.cpp` ballooned past 3,900 LOC while implementing auth, Google Sign-In, rate limiting, SSE streaming, ops enqueueing, metrics, and CLI plumbing. The single TU now owns configuration parsing, bcrypt/session stores, routing, HTML/JSON rendering, SSE broadcasting, diagnostics snapshots, and tests rely on the entire binary. The monolith is already painful to reason about and will block forthcoming work (distributed config, proxying inspector routes, PathSpace-backed session storage hardening, request logging, etc.). We need a modular layout before features like multi-instance clustering, external IdPs, or embedded inspector bridges pile on.

## Goals
1. Define composable C++ components (headers + source files) for ServeHtml so auth/session logic, routing, streaming, metrics, and adapters evolve independently.
2. Preserve existing behavior and test coverage (Python end-to-end + doctests) while extracting modules.
3. Establish public/internal headers in `include/pathspace/web/` so PathSpace apps can embed specific pieces (e.g., `ServeHtmlSessionStore`) without dragging the whole server.
4. Introduce dependency injection seams (interfaces or `std::function` hooks) so token buckets, session stores, and controllers can be unit-tested without bringing up the entire ServeHtml process—even if spinning up a bare `PathSpace` is cheap, the current monolith forces every test through httplib + threads.
5. Document the new structure in `docs/finished/Plan_WebServer_Adapter_Finished.md` and onboarding material to keep contributors oriented.

## Non-Goals
- Rewriting httplib-based HTTP stack or switching frameworks.
- Changing the CLI surface/flags beyond the refactor necessary to move parsing into its own module.
- Shipping additional runtime features (inspector proxying, CDN sync, WebSocket transport). Capture them as follow-ups only if the split unblocks them.

## Current State Snapshot (December 3, 2025)
- `ServeHtmlServer.cpp` mixes:
  - CLI/environment parsing & help strings.
  - PathSpace session persistence + in-memory store.
  - Bcrypt hashing (vendored lib).
  - Rate limiting buckets.
  - Request routing (login/logout/session/apps/assets/events/api/ops/metrics/healthz).
  - SSE implementation & Last-Event-ID handling.
  - Google OAuth PKCE + JWKS verification.
  - Demo seeding.
  - Metrics snapshot writing.
  - Process lifecycle (signals, stop flag, threads).
- Tests (`tests/tools/test_pathspace_serve_html_{http,sse,google}.py`) only cover the combined binary.
- No focused unit tests exist for sessions, rate limiting, or SSE logic.

## Target Architecture
```
serve_html/
├── Config.hpp / Config.cpp           # CLI + toml parsing, validation
├── Auth/
│   ├── Credentials.hpp               # bcrypt + user lookup
│   ├── SessionStore.hpp/.cpp         # interfaces for memory/pathspace stores
│   └── OAuthGoogle.hpp/.cpp          # PKCE + JWKS verification
├── RateLimit/
│   └── TokenBucket.hpp/.cpp
├── Routing/
│   ├── Router.hpp/.cpp               # table of handlers, middleware chain
│   ├── HtmlController.hpp/.cpp       # /apps, /assets, /events
│   ├── AuthController.hpp/.cpp       # /login, /logout, /session
│   ├── OpsController.hpp/.cpp        # /api/ops
│   └── MetricsController.hpp/.cpp    # /metrics, JSON snapshot
├── Streaming/
│   └── SseBroadcaster.hpp/.cpp       # watcher threads, keep-alives
├── DemoSeed.hpp/.cpp                 # optional fixture injection
├── ServerMain.cpp                    # wiring + httplib setup
```
- Interfaces expose PathSpace dependencies via thin abstractions (e.g., `PathSpaceFacade` with `read/insert/listChildren`).
- Controllers use dependency injection to receive `SessionValidator`, `HtmlRenderer`, etc.
- Shared utilities (logging helpers, error serialization, JSON builders) live under `web/detail/`.

## Workstreams & Milestones
1. **Phase 0 – Guardrails (1 day)**
   - Freeze behavior with golden data: capture example HTTP/SSE transcripts (attach to repo under `tests/data/serve_html/`).
   - Extend Python tests to cover `/logout`, `/session`, and failure cases (invalid cookie, rate-limit JSON) so refactors detect regressions.
   - **Status (December 4, 2025):** Guardrail transcripts live in `tests/data/serve_html/{http,sse}_transcript.json` and the HTTP/SSE integration tests now assert `/logout`, `/session`, invalid-cookie rejection, and `429` JSON payloads directly against the recorded expectations. Next up is Phase 1’s config/options extraction so the monolithic TU stops owning CLI parsing.
2. **Phase 1 – Config & Options Extraction (1–2 days)**
   - Move `ServeHtmlOptions`, CLI parsing, help text, and env seeding into `web/ServeHtmlOptions.{hpp,cpp}`.
   - Add validation helpers (port range, renderer existence) with focused unit tests.
   - **Status (December 4, 2025):** `ServeHtmlOptions` now lives in `include/pathspace/web/ServeHtmlOptions.hpp` with implementations in `src/pathspace/web/ServeHtmlOptions.cpp`, exposing environment overrides (`PATHSPACE_SERVE_HTML_*`), shared identifier helpers, and doctest coverage (`tests/unit/web/test_ServeHtmlOptions.cpp`). `ServeHtmlServer.cpp` shrank accordingly, and Phase 2 can begin splitting the auth/session stores.
   - Env overrides follow the CLI flag names (e.g., `PATHSPACE_SERVE_HTML_HOST`, `PATHSPACE_SERVE_HTML_PORT`, `PATHSPACE_SERVE_HTML_SESSION_TIMEOUT`, `PATHSPACE_SERVE_HTML_GOOGLE_CLIENT_ID`); see `docs/web_server.toml` for the full mapping captured alongside the config reference.
3. **Phase 2 – Auth/Session Modules (2–3 days)**
   - Create `SessionStore` interface + `InMemorySessionStore` and `PathSpaceSessionStore` files.
   - Extract bcrypt helpers + demo seeding utilities.
   - Add unit tests exercising token rotation, expiry, and PathSpace persistence with temp spaces.
   - **Status (December 4, 2025):** `include/pathspace/web/serve_html/auth/SessionStore.hpp` and
     `src/pathspace/web/serve_html/auth/SessionStore.cpp` now host the shared `SessionStore`
     abstraction plus concrete in-memory and PathSpace backends, while the bcrypt helpers and demo
     seeding moved into `serve_html/auth/Credentials.*` and `serve_html/DemoSeed.*`. `ServeHtmlServer.cpp`
     shrank to wiring code, and the new doctest `tests/unit/web/test_ServeHtmlSessionStore.cpp`
     exercises both backends (create/validate/revoke plus persisted JSON snapshots). Next up is
     Phase 3’s router/controller extraction.
4. **Phase 3 – Routing & Controllers (3–4 days)**
   - Introduce a router abstraction (method/path dispatch) to replace massive if/else ladder.
   - Implement controllers per route family; centralize middleware (auth, rate limiting, logging, JSON errors).
   - Reuse shared response builders (HTML template, JSON payload, SSE frames).
   - **Status (December 4, 2025):** `AuthController` now lives under `src/pathspace/web/serve_html/routing/`
     with shared HTTP helpers/metrics so `/login`, `/logout`, `/session`, and Google OAuth flows register
     via the controller. `/api/ops` has been extracted into `OpsController` with the same
     `HttpRequestContext` wiring, so rate limiting, session enforcement, metrics, and queue inserts now sit
     behind `src/pathspace/web/serve_html/routing/OpsController.{hpp,cpp}` and `ServeHtmlServer.cpp` just
     wires the controller. The HTML + asset routes landed in `HtmlController` (`src/pathspace/web/serve_html/routing/HtmlController.{hpp,cpp}`)
     with a dedicated `HtmlPayload` builder and `IsAssetPath` helper, so `/apps/<app>/<view>` and `/assets/<app>/<asset>` reuse the
     shared middleware and the monolith drops another ~400 LOC. The new doctest `tests/unit/web/test_HtmlHelpers.cpp` covers the
     HTML response escaping + asset path validator, and `ServeHtmlServer.cpp` now simply wires `Auth`, `Ops`, and `Html` controllers
     before delegating to the remaining SSE route (which Phase 4 will move into the streaming module).
5. **Phase 4 – Streaming & Background Workers (2 days)**
   - Extract SSE broadcaster (watchers, keep-alive, Last-Event-ID) into its own class with targeted tests (simulate PathSpace updates).
   - Provide `ServeHtmlBroadcaster` API so future transports (WebSocket) can share logic.
   - **Status (December 4, 2025):** `include/pathspace/web/serve_html/streaming/SseBroadcaster.hpp` and
     `src/pathspace/web/serve_html/streaming/SseBroadcaster.cpp` now own the broadcaster +
     `HtmlEventStreamSession`, `ServeHtmlServer.cpp` simply wires `SseBroadcaster` after the Auth/Ops/Html
     controllers, and `tests/unit/web/test_ServeHtmlSseBroadcaster.cpp` drives a real `ServeHtmlSpace` to cover
     initial snapshots plus revision-gap reload events.
6. **Phase 5 – External Providers & Observability (2 days)**
   - Move Google OAuth/ JWKS cache into `Auth::OAuthGoogle` module; expose structured errors for better logs.
   - Isolate metrics snapshot writing so controllers just emit counters.
   - Ensure `/metrics` + JSON snapshots share one DTO.
   - **Status (December 4, 2025):** `OAuthGoogle` now lives under `include/src pathspace/web/serve_html/auth/` with
     dedicated PKCE state store, JWKS cache, HTTP helpers, and structured `Error` codes. `AuthController`
     delegates Google Sign-In to the module (logging the `describe_error` output) so future providers can plug
     in without touching routing code. Metrics gained a shared `MetricsSnapshot` DTO and `capture_snapshot()`
     helper; both `/metrics` and the PathSpace publisher reuse the snapshot so publishing logic is isolated
     from the controllers.
7. **Phase 6 – Documentation & Adoption (1 day)**
   - Update `docs/finished/Plan_WebServer_Adapter_Finished.md`, `docs/AI_Debugging_Playbook.md`, README, and memory with the new file layout.
   - Provide contributor guide snippet (“Where to add ServeHtml features”).
   - **Status (December 4, 2025):** Added `docs/serve_html/README.md` (module guide + contributor
     checklist), refreshed README + `docs/finished/Plan_WebServer_Adapter_Finished.md` +
     `docs/AI_Debugging_Playbook.md` + `docs/Plan_Overview.md` to reference the split, logged the
     completion in `docs/Memory.md`, and recorded follow-up backlog items in
     `docs/AI_Todo.task`.

## Testing Strategy
- Keep existing integration tests as the highest-confidence guard.
- Add C++ unit tests where seams emerge (session store, token bucket, SSE broadcaster, router matching).
- Ensure `./scripts/compile.sh --loop=5 --timeout=20` stays the gating step once refactor lands.

## Risks & Mitigations
| Risk | Mitigation |
| --- | --- |
| Regression while moving logic across files | Expand integration tests before Phase 1; land refactor in small, reviewable chunks (per workstream). |
| Linker bloat or duplicate symbols after splitting | Enforce namespaces + anonymous namespaces carefully; add `web/CMakeLists.txt` to group sources. |
| PathSpace dependencies leak into headers | Use forward declarations + minimal includes; keep heavy PathSpace headers inside `.cpp` files. |
| Contributor confusion during migration | Document the new map (diagram + README snippet); add `docs/web/README.md` if necessary. |

## Deliverables
- Modular `serve_html` target with subdirectories under `src/pathspace/web/` and `include/pathspace/web/`.
- New/updated unit tests for auth, rate limiting, and streaming.
- Updated docs + memory + plan overview entries.
- Follow-up TODOs captured in `docs/AI_TODO.task` for remaining enhancements after modularization.

## Post-Plan Follow-Ups (logged in `docs/AI_Todo.task`)
- **Multi-instance ServeHtml + distributed session stores** — implement a durable
  `SessionStore` backend (shared PathSpace or external service), document
  load-balancer affinity, and expand `/diagnostics/web/*` so clusters stay
  observable.
- **Inspector proxy & tooling routes** — expose the inspector SPA + JSON/SSE
  bridges as optional controllers so operators can debug without a sidecar.
- **Request logging/audit hardening** — add structured request/response logs and
  security events under `/diagnostics/web/security/*`, reusing the centralized
  metrics helpers.

## Timeline & Dependencies
- Estimated ~10 engineering days spread over two iterations.
- Requires existing ServeHtml integration tests to remain green; no other blocking dependencies.
- Coordinate with distributed PathSpace plan owners once modularization unlocks remote session stores.

## Exit Criteria
- `ServeHtmlServer.cpp` shrinks to a thin bootstrap (`main` + wiring, <200 LOC).
- Each major subsystem has its own TU + header with targeted tests.
- Documentation reflects the modular structure and contributors know where to add/modify logic.
- No regression in `PathSpaceServeHtml{Http,Sse,Google}` tests nor in overall compile loop.
