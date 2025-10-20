# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for the embedded PathSpace web server.
> **Audience:** PathSpace maintainers implementing HTML delivery for web clients and coordinating dual native/web app flows.

# PathSpace Web Server Adapter Plan

## Goals
- Serve PathSpace applications (paint, widgets, PDF reader, health dashboard, etc.) to web browsers using the existing HTML adapter outputs (`output/v1/html/*`) and canonical path conventions.
- Support native and web delivery from the same PathSpace app definition so producers maintain one code path.
- Provide a minimal, secure HTTP interface that maps URLs to PathSpace paths, manages user authentication, and optionally streams live updates.
- Remain lightweight enough for local development while allowing scale-out in hosted deployment.

## Non-Goals (v1)
- Full multi-tenant rate limiting and quota enforcement (capture TODOs for later).
- Cross-origin federation or third-party OAuth (assume single origin, first-party login).
- Serving arbitrary binary blobs outside the renderer/asset namespaces (stick to documented outputs).
- Hot code reload for callables/executions; focus on serving rendered HTML assets.

## Architecture Overview
```
Browser ──HTTP(S)──> Web Server Adapter ──PathSpace Client API──> PathSpace Instance
                                     │                             │
                                     │                             ├─ Renderers/Scenes (native + HTML)
                                     └─ Static Asset Cache ◀───────┘
```

### Components
- **Web Server Adapter** — process exposing HTTP/S, translating requests to PathSpace path operations. Recommended implementation: lightweight HTTP server with no external web frameworks for prototyping, with production targeting a native C++ implementation.
- **PathSpace Client API** — lightweight binding (C++ CLI, IPC, or gRPC-style protocol) used by the server to read from the PathSpace tree, trigger renders, and subscribe to notifications.
- **Static Asset Cache** — folder or in-memory cache that mirrors `output/v1/html/assets/*` and per-app `resources/` entries for fonts/images.
- **Browser Client** — receives HTML/JS/CSS, optionally listens to Server-Sent Events (SSE) or WebSocket notifications for live updates.

### Native/Web Parity Strategy
- Applications author scenes, renderers, and presenters once under app-relative paths.
- Native desktop apps consume software/Metal outputs (`output/v1/common`, framebuffer, textures).
- Web clients consume HTML outputs from the same renderer target.
- Shared logic (state updates, ops inboxes) stays in PathSpace; UI differences live in adapters (native presenter vs. web adapter).

## Request Lifecycle
1. **Authenticate** — Browser posts credentials to `/login`. Server validates and creates a session linked to a PathSpace user root (`/users/<user>/system/applications/<app>`). Set HTTP-only session cookie.
2. **Resolve App** — Browser requests `/apps/<app>/<view>`. Server:
   - Maps `<app>` to PathSpace app root.
  - Resolves `<view>` to a renderer target (e.g., `renderers/html/targets/apps/<view>`).
   - Ensures the renderer has a fresh revision; optionally call `render` execution when `auto_render_on_present` is disabled.
3. **Gather Artifacts**
   - Read `output/v1/html/dom`, `css`, `commands`, `js`, `assets/*`.
   - Read metadata (`output/v1/common/*`) to embed frame indices, revision, present policy, backend info.
   - Collect asset fingerprints (`resources/html/<hash>`).
4. **Respond**
   - Serve `index.html` template injecting DOM, linking CSS/JS, and referencing asset URLs via fingerprint-based paths (`/assets/<hash>`).
   - Set cache headers (ETag = revision hash, `Cache-Control: private, max-age=30` by default).
5. **Live Updates (optional)**
   - SSE endpoint `/apps/<app>/<view>/events` watches `output/v1/common/frameIndex` and pushes new revision IDs.
   - Browser reloads partial DOM via fetch or full page refresh based on adapter mode.

## URL Mapping
| HTTP Path | Description | PathSpace Source |
| --- | --- | --- |
| `/login` (POST) | Authenticate user, establish session | Backed by application-specific auth (external service or PathSpace paths) |
| `/apps/<app>/<view>` | Rendered HTML page | `renderers/<rid>/targets/html/<view>/output/v1/html/dom` (and companions) |
| `/assets/<app>/<fingerprint>` | Fingerprinted static asset (fonts, images) | `renderers/<rid>/targets/html/<view>/output/v1/html/assets/<fingerprint>` or app `resources/` |
| `/api/path/<encoded-path>` | (Optional) JSON API to read PathSpace nodes | Direct `read` with permission checks; returns JSON |
| `/apps/<app>/<view>/events` | Live update stream (SSE/WebSocket) | Watch `output/v1/common` for revision changes |

## Session & Security
- **Session Store** — In-memory for dev; pluggable (Redis) for production. Store `user_id`, `app_root`, `permissions`.
- **Authorization** — Before touching PathSpace, verify `requested_path` is under `app_root`. Reject cross-app or system-level paths.
- **CSRF** — Standard token for `POST`/`PUT` routes; GET routes serve content only.
- **Transport** — Native server must support HTTPS (TLS termination) using configured certificates; allow HTTP only for local development/testing.
- **Command Execution** — Limit or sandbox PathSpace `insert`/`take` operations triggered via HTTP to avoid long-running tasks or arbitrary code. Consider a whitelist of allowed executions (e.g., `render`, `mark_dirty`).

## Asset Pipeline
- Extend renderer pipeline to publish:
  - `output/v1/html/assets/<fingerprint>` binaries.
  - `output/v1/html/manifest.json` (adapter config, dependencies, revision hash).
  - `resources/<app>/<fingerprint>` for shared assets (fonts/images) outside a specific view.
- Server caches assets in memory/disk using fingerprint as key; send `Cache-Control: public, max-age=31536000, immutable`.
- Provide a background job to prefetch assets when a new revision lands (avoid latency spikes on first HTTP request).

## Live Update Strategy
- Subscribe to PathSpace notifications on:
  - `renderers/<rid>/targets/html/<view>/output/v1/common/frameIndex`
  - `diagnostics/errors/live`
  - `renderers/<rid>/targets/html/<view>/events/renderRequested/queue` (optional)
- On new frame, SSE push `{ revision, frameIndex, timestamp }`.
- Browser JS decides whether to:
  - Fetch updated DOM/CSS (for `AlwaysLatestComplete`).
  - Diff `commands` / re-run canvas replay.
  - Display error banners if diagnostics report issues.

## Native/Web Coexistence
- **Shared State Paths** — Keep application state under `apps/<app>/state/*`. Native and web presenters both react to PathSpace changes.
- **Renderer Targets** — For each view, define both:
  - `renderers/<rid>/targets/surfaces/<view>` (native framebuffer).
  - `renderers/<rid>/targets/html/<view>` (HTML adapter).
- **Present Policies** — Native windows use configured policy; HTML always uses `AlwaysLatestComplete`. Document in `docs/Plan_SceneGraph_Renderer.md`.
- **Input/Interaction** — Web clients post events to `ops/` inbox paths via HTTP APIs; native clients use local PathSpace insertions. Ensure server exposes minimal REST endpoints to translate browser events into PathSpace operations (e.g., `/api/path/<path>` POST to insert an event).

## Authentication & Sessions
- **Credential source:** default to PathSpace-backed credentials under `/system/auth/users/<user>` storing salted/bcrypt hashes; allow configuration for external IdPs later.
- **Session payload:** `{ session_id, user_id, app_root, csrf_token, issued_at, expires_at }` persisted in-memory for dev or via Redis/SQLite adapters.
- **Expiry & rotation:** 30-minute inactivity timeout, 8-hour absolute max; rotate session ID after login and privileged operations. Logout drops session and emits `sessions/<id>/invalidated`.
- **Per-request checks:** middleware validates cookie signature, checks expiry, and ensures requested path stays under `app_root`. On failure return 401 with generic message and log event.
- **Google Sign-In:** Optional OAuth 2.0 (Authorization Code + PKCE) adapter. Configure `client_id`, `client_secret`, `redirect_uri` via `[auth.google]` in `web_server.toml`. Callback verifies ID token signature/audience, maps Google `sub` to PathSpace user entry (`/system/auth/oauth/google/<sub>`), and falls back if user unrecognized. Store minimal token data (ID token only) and reuse existing session issuance on success.
- **Logout effects:** When a session ends (explicit logout, timeout, or connection drop), close any associated distributed mounts so the underlying PathSpace applications enter their shutdown flow. Require a fresh login/mount before resuming work.

## Deployment Models
- **Embedded:** Web server linked inside the PathSpace process for low-latency local use.
- **Sidecar:** Separate process communicating over Unix domain socket or gRPC; PathSpace exposes a lightweight RPC for `read/insert/render`.
- **Cluster:** Multiple stateless web frontends behind reverse proxy (NGINX/Envoy) with TLS termination, PathSpace workers behind service mesh. HTML bundles shared via object store (e.g., S3) or replicated disk.
- Provide `web_server.toml` describing mode, listen address, TLS certificates/keys (for native HTTPS), session store adapter, and logging level.

## Reverse Proxy / NGINX Integration
- Production deployments are expected to sit behind NGINX (or a similar reverse proxy) for TLS, load balancing, and buffering control.
- Recommended settings:
  - Preserve client information: `proxy_set_header Host $host; proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for; proxy_set_header X-Forwarded-Proto $scheme;`.
  - Disable buffering for SSE: `proxy_buffering off; proxy_cache off; proxy_set_header Connection '';` so events stream without delay.
  - Extend timeouts for long-lived streams: `proxy_read_timeout 120s; proxy_send_timeout 120s;` (tune per deployment).
  - Prep for WebSocket upgrades (future): `proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade";`.
  - Enforce request size limits consistent with server policy (`client_max_body_size 4m;` configurable).
  - Enable gzip/deflate for HTML/CSS/JS, but exclude SSE endpoints (`gzip_types text/html text/css application/javascript; gzip_proxied any;`).
- Document health check usage (`/api/healthz`) and example NGINX snippets so developers can drop the server behind a proxy with minimal friction.

## API Surface
- Note: once JSON serialization support lands (see plan & backlog), `/api/path` and SSE updates will use the shared JSON encoder for consistent output across web/debug tooling.
- **Static routes:** `/apps/<app>/<view>`, `/assets/<app>/<fingerprint>`, `/apps/<app>/<view>/events`.
- **REST:** 
  - `GET /api/path/<encoded>` → JSON snapshot (value, children) with permission filtering.
  - `POST /api/path/<encoded>` → Insert/update; payload validated against allowlist under `app_root/config/web_api_allowlist`.
  - `POST /api/ops/<op>` → Enqueue operation into `ops/<op>/inbox` (JSON body with schema version).
  - `GET /api/healthz` (liveness) / `GET /api/readyz` (PathSpace connectivity).
- **Error contract:** JSON `{ "error": { "code": "...", "message": "..." } }`, mapping PathSpace errors to HTTP status codes.

## Security & Hardening
- Force HTTPS (redirect HTTP) outside dev. Provide helper script for dev certificates.
- Use HTTP-only, Secure cookies with SameSite=Lax (configurable). Store CSRF token per session and require header `X-CSRF-Token` on mutating requests.
- Content-Security-Policy baseline: `default-src 'self'; img-src 'self' data:; script-src 'self'; style-src 'self' 'unsafe-inline'`.
- Enforce payload size limits (1 MB default) and token-bucket rate limiting per IP + per session. Log suspicious activity to `app_root/io/log/security`.
- Optionally mount the user-facing subtree through a throttling PathSpace adapter that forwards calls to the real app while tracking request rates; when thresholds are exceeded it introduces delays or temporary denials.
- Maintain allowlist of permitted PathSpace executions; reject and audit any disallowed invocation attempts.

## Asset & Cache Strategy
- Fingerprint assets with SHA-256 and serve from `/assets/<app>/<hash>` using `Cache-Control: public, max-age=31536000, immutable`.
- DOM/CSS/JS responses include ETag (revision hash) and `Last-Modified`; honor conditional requests to reduce bandwidth.
- Prewarm bundles by watching `output/v1/html/common/frameIndex` and copying outputs into a staging directory, then atomically swapping symlinks the server serves from.
- Optional CDN: allow rewriting asset URLs via config `cdn_base_url`; fallback to local serving when unset.

## Live Update Protocol
- Use Server-Sent Events for v1. Message payload:
  ```json
  { "type": "frame", "revision": 42, "frameIndex": 128, "timestamp": "2025-10-20T15:04:05Z" }
  ```
  additional types: `"diagnostic"` (last error), `"reload"` (schema change).
- Clients debounce fetches (≥100 ms) and re-fetch DOM on reconnect. Reconnect strategy: exponential backoff capped at 5 s with jitter.
- Track missed revisions by storing last served revision in session; if gap detected, send `"reload"` message forcing full refresh.
- Document WebSocket upgrade path for future bi-directional features (input streaming, logs).

## Testing & Tooling
- Integration harness (`scripts/test_web_server.sh`) spins up PathSpace fixtures, runs the server, executes headless browser tests (Playwright/Puppeteer) to validate renders, SSE refresh, auth, asset caching.
- Provide mock PathSpace client for unit testing handlers without full engine.
- Developer workflow: `npm run serve-html` (or `./scripts/run_dev_web_server.sh`) plus automatic bundle export (`./scripts/export_html_example.py --watch`). Document in README.
- Update `docs/AI_Debugging_Playbook.md` with log paths (`app_root/io/log/web`, `.../security`) and troubleshooting checklist.

## Observability
- Metrics: request latency histogram, SSE connection count, render trigger latency, cache hit rate, auth failures, rate-limit rejections. Expose optional `/metrics` endpoint (Prometheus).
- Structured logs (JSON): `{ ts, level, route, status, latency_ms, session_id?, user_id?, message }`.
- Tie into PathSpace diagnostics by relaying `diagnostics/errors/live` to logs/SSE clients.
- Establish alert thresholds (e.g., 5xx > 1% for 5 min, SSE disconnect spikes).

## Extensibility & Configuration
- `web_server.toml` sections: `[server]`, `[paths]`, `[assets]`, `[security]`, `[apps.<app>]` (per-app overrides: renderer target IDs, allowed REST paths, SSE options).
- Allow middleware plugins (analytics, custom auth) registered via config. Provide stable hook interface (request/response interceptors).
- Document onboarding for new apps: configure renderer target, export HTML outputs, add entry to config with optional route aliases.

## Implementation Phases
1. **Phase 0 — Prototype (local only)**
   - Implement prototype HTTP server (`tools/serve_html` — a small standalone C++ binary) with `/apps/<app>/<view>` route using only the standard library / PathSpace utilities to stand up the HTML adapter quickly.
   - No auth; assume single developer session.
   - Serve paint/widgets examples by reading from `output/v1/html`.
   - Document setup in this plan.
2. **Phase 1 — Session & Static Assets**
   - Add simple username/password auth stored in PathSpace or config.
   - Serve assets via `/assets/<fingerprint>` with cache headers.
   - Write integration tests (e.g., curl scripts or a C++ test harness) to assert correct responses.
3. **Phase 2 — Live Updates & Error Handling**
   - Add SSE endpoint for revision changes.
   - Browser client updates canvas/DOM without full reload.
   - Surface diagnostics (`diagnostics/errors/live`) in UI.
4. **Phase 3 — Dual Native/Web Support**
   - Ensure state mutations triggered from web reflect in native views (and vice versa) by standardizing ops endpoints.
   - Extend examples to demonstrate native window + browser running simultaneously.
5. **Phase 4 — Hardened Deployment**
   - Add pluggable session store, HTTPS termination guidance.
   - Document scaling options (multiple web servers reading from shared PathSpace).
   - Add rate limiting hooks (middleware) and audit logging (PathSpace or external log).
  6. **Phase 5 — Observability & Tooling**
   - Instrument metrics/logging, expose `/metrics`, and integrate with existing dashboards.
   - Land end-to-end tests for auth expiry, SSE reconnect, asset caching headers.
   - Update `docs/AI_Debugging_Playbook.md` with web server troubleshooting.
7. **Phase 6 — External IdP Integration**
   - Implement Google Sign-In adapter (PKCE flow) using configured client credentials.
   - Add automated tests mocking Google's OAuth endpoints and token responses.
   - Document setup steps (Google Cloud Console registration, redirect URIs) and failure handling in this plan and the debugging playbook.

## Required Updates Elsewhere
- `docs/Plan_SceneGraph_Renderer.md` — Reference this plan; document the HTML server’s role.
- `docs/AI_Architecture.md` — Add note under HTML adapter about server delivery.
- `docs/AI_Debugging_Playbook.md` — Include troubleshooting steps for web server logs/SSE.
- `docs/AI_Todo.task` — Log implementation steps with priorities.
- Example README(s) — Add instructions for launching the server and visiting the browser demo.

> **Dependency:** Remote deployments assume distributed mounts per `docs/Plan_Distributed_PathSpace.md` so the web server can reach user app roots securely.

## Open Questions
- **API Transport** — Evaluate when we will need HTTP/2 or gRPC layering on top of the native C++ HTTP server (e.g., for sidecar/cluster deployments).
- **Incremental HTML Updates** — Finalize the JSON diff/event model and fallback strategy for streaming DOM updates.
- **Session Tooling** — Decide how developers manage auth sessions (CLI helpers, inspection endpoints) without introducing duplicative configuration.

## Next Actions
1. Create `tools/serve_html` prototype (small C++ HTTP server with no external frameworks) to serve HTML outputs during development.
2. Define session/auth middleware (bcrypt hashing, cookie handling) and document configuration in `web_server.toml`.
3. Extend paint/widgets examples with `--export-html` flag producing HTML bundles (dom/css/js/assets).
4. Implement SSE endpoint with reconnection handling; add headless tests verifying refresh + diagnostics events.
5. Add integration tests executing server → PathSpace → browser flow (headless fetch) verifying 200 responses, auth gate, and caching headers.
6. Update documentation references, add TODO entries for the phases above.
