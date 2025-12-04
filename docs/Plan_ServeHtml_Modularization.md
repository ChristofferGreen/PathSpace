# Plan: ServeHtml Modularization

> **Owner:** PathSpace web adapter maintainers  
> **Drafted:** December 3, 2025  
> **Status:** Draft — pending kickoff once ServeHtml prototype stabilizes (post Phase 6 of the finished Web Server Adapter plan).

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
2. **Phase 1 – Config & Options Extraction (1–2 days)**
   - Move `ServeHtmlOptions`, CLI parsing, help text, and env seeding into `web/ServeHtmlOptions.{hpp,cpp}`.
   - Add validation helpers (port range, renderer existence) with focused unit tests.
3. **Phase 2 – Auth/Session Modules (2–3 days)**
   - Create `SessionStore` interface + `InMemorySessionStore` and `PathSpaceSessionStore` files.
   - Extract bcrypt helpers + demo seeding utilities.
   - Add unit tests exercising token rotation, expiry, and PathSpace persistence with temp spaces.
4. **Phase 3 – Routing & Controllers (3–4 days)**
   - Introduce a router abstraction (method/path dispatch) to replace massive if/else ladder.
   - Implement controllers per route family; centralize middleware (auth, rate limiting, logging, JSON errors).
   - Reuse shared response builders (HTML template, JSON payload, SSE frames).
5. **Phase 4 – Streaming & Background Workers (2 days)**
   - Extract SSE broadcaster (watchers, keep-alive, Last-Event-ID) into its own class with targeted tests (simulate PathSpace updates).
   - Provide `ServeHtmlBroadcaster` API so future transports (WebSocket) can share logic.
6. **Phase 5 – External Providers & Observability (2 days)**
   - Move Google OAuth/ JWKS cache into `Auth::OAuthGoogle` module; expose structured errors for better logs.
   - Isolate metrics snapshot writing so controllers just emit counters.
   - Ensure `/metrics` + JSON snapshots share one DTO.
7. **Phase 6 – Documentation & Adoption (1 day)**
   - Update `docs/finished/Plan_WebServer_Adapter_Finished.md`, `docs/AI_Debugging_Playbook.md`, README, and memory with the new file layout.
   - Provide contributor guide snippet (“Where to add ServeHtml features”).

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

## Timeline & Dependencies
- Estimated ~10 engineering days spread over two iterations.
- Requires existing ServeHtml integration tests to remain green; no other blocking dependencies.
- Coordinate with distributed PathSpace plan owners once modularization unlocks remote session stores.

## Exit Criteria
- `ServeHtmlServer.cpp` shrinks to a thin bootstrap (`main` + wiring, <200 LOC).
- Each major subsystem has its own TU + header with targeted tests.
- Documentation reflects the modular structure and contributors know where to add/modify logic.
- No regression in `PathSpaceServeHtml{Http,Sse,Google}` tests nor in overall compile loop.
