# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for the embedded PathSpace web server.
> **Audience:** PathSpace maintainers implementing HTML delivery for web clients and coordinating dual native/web app flows.
> **Completed:** December 3, 2025 — Phases 0–6 (prototype through Google Sign-In) shipped and the plan is archived for historical reference.

# PathSpace Web Server Adapter Plan

## Goals
- Serve PathSpace applications (paint, widgets, PDF reader, health dashboard, etc.) to web browsers using the existing HTML adapter outputs (`output/v1/html/*`) and canonical path conventions.
- Support native and web delivery from the same PathSpace app definition so producers maintain one code path.
- Provide a minimal, secure HTTP interface that maps URLs to PathSpace paths, manages user authentication, and optionally streams live updates.
- Remain lightweight enough for local development while allowing scale-out in hosted deployment.
- Host the PathSpace inspector SPA plus its JSON/SSE endpoints so operators can introspect the same PathSpace instance without launching a separate sidecar.

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
- **Inspector Bridge** — embeds `SP::Inspector::InspectorHttpServer` (or proxies to the standalone binary) so `/inspector/*` routes share the same PathSpace, reuse `InspectorHttpServer::Options` (snapshot caps, remote mounts, ACLs, diagnostics), and deliver the bundled SPA without another process.

### ServeHtml modular layout (December 2025)
The prototype binary now wires a modular set of components described in
`docs/serve_html/README.md` and `docs/finished/Plan_ServeHtml_Modularization_Finished.md`:

- `ServeHtmlOptions` parses CLI/env overrides and publishes shared identifier helpers.
- `serve_html/auth/*` hosts session stores, bcrypt helpers, demo fixtures, and
  `OAuthGoogle` so authentication evolves without touching routing code.
- `serve_html/routing/*` contains controllers for `/login`, `/logout`, `/session`,
  `/apps`, `/assets`, `/api/ops`, and future routes plus their shared
  `HttpRequestContext` middleware.
- `serve_html/streaming/SseBroadcaster.*` owns the SSE lifecycle and Last-Event-ID
  resume logic.
- `serve_html/Metrics.*` renders Prometheus output and JSON snapshots that both
  `/metrics` and the background publisher reuse.

`ServeHtmlServer.cpp` is now a thin bootstrap that instantiates the modules above,
registers their routes on `httplib::Server`, and handles lifecycle signals.
Contributors adding features should follow the module guide referenced above.

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
   - Reference implementation: `scripts/paint_example_inspector_panel.py` exposes `/api/cards/paint-example` (JSON) and `/api/cards/paint-example/events` (SSE) by shelling into `pathspace_paint_screenshot_card`. This helper is a dev-only convenience; the adapter can choose whether or not to mirror the same payload when web priorities resume.

## URL Mapping
| HTTP Path | Description | PathSpace Source |
| --- | --- | --- |
| `/login` (POST) | Authenticate user, establish session | Reads `/system/auth/users/<user>/password_bcrypt`, compares bcrypt hash, then issues an HTTP-only cookie (`ps_session` by default). |
| `/login/google` (GET) | Initiate Google OAuth (PKCE) | Redirects to the configured Google `auth_endpoint` with `state` + `code_challenge` derived from the hashed verifier. |
| `/login/google/callback` (GET) | Complete Google OAuth, issue session | Exchanges `code` for tokens, verifies the ID token via JWKS, then maps `/system/auth/oauth/google/<sub>` to a username before creating the session. |
| `/apps/<app>/<view>` | Rendered HTML page | `renderers/<rid>/targets/html/<view>/output/v1/html/dom` (and companions) |
| `/apps/<app>/<view>?format=json` | JSON payload (dom/css/js/commands/revision) used by the auto-refresh script | Same renderer target (`output/v1/html/*`) |
| `/assets/<app>/<fingerprint>` | Fingerprinted static asset (fonts, images) | `renderers/<rid>/targets/html/<view>/output/v1/html/assets/<fingerprint>` or app `resources/` |
| `/api/path/<encoded-path>` | (Optional) JSON API to read PathSpace nodes | Direct `read` with permission checks; returns JSON |
| `/apps/<app>/<view>/events` | Live update stream (SSE/WebSocket) | Watch `output/v1/common` for revision changes |
| `/inspector/` | Bundled inspector SPA (static assets) | `InspectorUiAssets` served via `InspectorHttpServer` |
| `/inspector/tree` | JSON snapshot rooted at configured `InspectorSnapshotOptions` | `InspectorSnapshot` backed by `PathSpaceJsonExporter` |
| `/inspector/node` | Focused node detail (value summary + children) | `InspectorSnapshot::VisitNode` reuse |
| `/inspector/stream` | SSE feed emitting `snapshot` + `delta` events | `InspectorStreamSession` watching `InspectorSnapshot` fingerprints |
| `/inspector/metrics/stream` | SSE/JSON queue + drop counters | `/inspector/metrics/stream/*` nodes mirrored to HTTP |
| `/inspector/metrics/search` (GET/POST) | Search/watch diagnostics + POSTed telemetry | `/inspector/metrics/search/*` helpers |
| `/inspector/remotes` | Remote mount picker + alias health | `RemoteMountManager` diagnostics (`/inspector/metrics/remotes/*`) |
| `/inspector/watchlists` (REST) | CRUD saved watchlists/import/export | `/inspector/user/<id>/watchlists/*` |
| `/inspector/snapshots` (+ `/diff`) | Capture/list/download/diff snapshots | Inspector snapshot store + `PathSpaceJsonExporter` |
| `/inspector/actions/toggles` | Gated admin-only bool flips | `InspectorWriteToggle` helpers logging to `/diagnostics/web/inspector/audit_log/*` |

## Authentication & Security
- **Credential source:** PathSpace-backed credentials live under `/system/auth/users/<user>` storing salted/bcrypt hashes. Optional Google Sign-In augments this: the adapter redirects callers to Google (PKCE), exchanges codes via the configured endpoints, verifies ID token signatures against JWKS, and maps each Google `sub` to a canonical username stored at `/system/auth/oauth/google/<sub>`. Missing mappings return 401 so deployments opt in explicitly. All knobs ship in `[auth.google]` inside `docs/web_server.toml` and mirror the CLI flags added to `pathspace_serve_html`.
- **Session payload:** `{ session_id, user_id, app_root, csrf_token, issued_at, expires_at }` persisted in-memory for dev or via PathSpace-backed JSON blobs under `/system/web/sessions/<id>`.
- **Session store backend:** `pathspace_serve_html` accepts `--session-store {memory|pathspace}` and `--session-store-root <path>`; the default keeps sessions in-process while the PathSpace backend mirrors them into `/system/web/sessions`. The knobs match `[auth.session_store]` in `docs/web_server.toml`.
- **Expiry & rotation:** 30-minute inactivity timeout, 8-hour absolute max; rotate session IDs after login and privileged operations. Logout drops the session and emits `sessions/<id>/invalidated`.
- **Per-request checks:** middleware validates cookie signature, checks expiry, and ensures `requested_path` stays under `app_root`. On failure return 401 with a generic payload and log the event.
- **Google diagnostics:** `/login/google/callback` logs token exchange/signature failures, the JWKS cache publishes metrics under `/io/metrics/web_server/serve_html/live`, and unmapped `sub` values increment auth-failure counters so dashboards surface misconfigurations quickly.
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
- `/apps/<app>/<view>/events` now streams SSE directly from `pathspace_serve_html`: it watches the target glob (`<target>/**`), pushes `frame`, `diagnostic`, `reload`, and `error` events, tags each payload with `id=<revision>` so `Last-Event-ID` works, emits `retry: 2000`, and sends keep-alive comments every ~5 s with `X-Accel-Buffering: no` to keep proxies from buffering.
- The demo flow includes `--demo-refresh-interval-ms` so local runs/tests can exercise the stream without wiring a real renderer; when omitted the SSE route simply waits for the first PathSpace update.
- Browser JS decides whether to:
  - Fetch updated DOM/CSS (for `AlwaysLatestComplete`).
  - Diff `commands` / re-run canvas replay.
  - Display error banners if diagnostics report issues.
- `pathspace_serve_html` now injects a live-update script into every HTML response. It opens an `EventSource` on `/apps/<app>/<view>/events`, fetches `/apps/<app>/<view>?format=json` when revisions advance, hot-swaps DOM/CSS/commands, reruns inline JS hooks, and surfaces diagnostic banners so operators no longer need to refresh manually. When `EventSource` is unavailable the script degrades to a warning badge + manual-refresh guidance.

## Inspector Integration
- Run the inspector routes (`/inspector/*`) inside the same adapter so operators can pivot from the rendered app to the live PathSpace tree without launching `pathspace_inspector_server` separately. The adapter either embeds `SP::Inspector::InspectorHttpServer` directly or proxies to the shipped binary; in both cases it reuses the same `PathSpace` pointer and authentication middleware so ACLs and session cookies line up.
- Expose an `[inspector]` block in `web_server.toml` mirroring `InspectorHttpServer::Options`:
  - `root`, `max_depth`, `max_children`, and `include_values` map to `InspectorSnapshotOptions`.
  - `diagnostics_root` points at `/diagnostics/ui/paint_example/screenshot_baseline` by default so the paint screenshot card lights up without extra wiring.
  - `stream` budgets (`poll_interval_ms`, `keepalive_ms`, `idle_timeout_ms`, `max_pending_events`, `max_events_per_tick`) must match the server defaults documented in `docs/finished/Plan_PathSpace_Inspector_Finished.md` so `/inspector/metrics/stream/*` and the SPA’s stream-health card stay meaningful.
  - `remote_mounts[]` forward directly into `InspectorHttpServer::Options::remote_mounts`, enabling the multi-root dropdown plus `/inspector/remotes` diagnostics when the web server also brokers distributed mounts.
  - `acl` configuration (per-role root lists, header overrides) plugs into `InspectorAcl` so `/inspector/tree`, `/inspector/node`, `/inspector/stream`, and write toggles all enforce the same scopes as the rest of the adapter.
- Serve the bundled SPA by default under `/inspector/` using the assets linked in `InspectorUiAssets`. When developing custom front-ends, honor `ui_root` so the adapter can hotload a locally built bundle while keeping JSON/SSE endpoints identical.
- Surface the existing diagnostics endpoints untouched:
  - `/inspector/metrics/stream` mirrors queue depth, drops, resends, active/total sessions, and disconnect reasons from the PathSpace nodes.
  - `/inspector/metrics/search` accepts POSTed telemetry from the SPA (query, watchlist stats) and exposes aggregated counters for dashboards.
  - `/inspector/metrics/remotes/<alias>` and `/inspector/remotes` keep publishing alias health (latency, failure streaks, waiter depth) so operators can see remote mounts go degraded directly from the browser.
  - `/inspector/user/<id>/watchlists/*`, `/inspector/watchlists`, `/inspector/snapshots`, and `/inspector/actions/toggles` retain their storage locations, ensuring saved watchlists, snapshots/diffs, and gated admin toggles survive whether the inspector runs standalone or inside the adapter.
- Document that SSE endpoints (`/inspector/stream`, `/inspector/metrics/stream`) require the same reverse-proxy buffering overrides as `/apps/<app>/<view>/events`. Keep the proxy snippets in sync so operators do not have to special-case inspector routes.
- Testing expectation: `scripts/ci_inspector_tests.sh --loop=5` must run against the adapter-hosted endpoints so Playwright verifies both the HTML delivery paths and the inspector SPA in one workflow.

## Native/Web Coexistence
- **Shared State Paths** — Keep application state under `apps/<app>/state/*`. Native and web presenters both react to PathSpace changes.
- **Renderer Targets** — For each view, define both:
  - `renderers/<rid>/targets/surfaces/<view>` (native framebuffer).
  - `renderers/<rid>/targets/html/<view>` (HTML adapter).
- **Present Policies** — Native windows use configured policy; HTML always uses `AlwaysLatestComplete`. Document in `docs/Plan_SceneGraph_Renderer.md`.
- **Input/Interaction** — Web clients post events to `ops/` inbox paths via HTTP APIs; native clients use local PathSpace insertions. The adapter now exposes `POST /api/ops/<op>` (JSON `{"app":"<app>","schema":"<id>","payload":{...}}`, ≤ 1 MiB, auth required) which writes directly to `/system/applications/<app>/ops/<op>/inbox/queue` so browser actions feed the same reducers/handlers that native presenters observe. Future `/api/path/<path>` writes still route through the same allowlist/CSRF policy once implemented.

## Deployment Models
- **Embedded:** Web server linked inside the PathSpace process for low-latency local use.
- **Sidecar:** Separate process communicating over Unix domain socket or gRPC; PathSpace exposes a lightweight RPC for `read/insert/render`.
- **Cluster:** Multiple stateless web frontends behind reverse proxy (NGINX/Envoy) with TLS termination, PathSpace workers behind service mesh. HTML bundles shared via object store (e.g., S3) or replicated disk.
- Provide `web_server.toml` describing mode, listen address, TLS certificates/keys (for native HTTPS), session store adapter, and logging level.

## Scaling & Multi-Server Deployments (Phase 4)
Phase 4 requires the adapter to stay stateless so multiple `pathspace_serve_html` instances can sit behind a proxy and read the same PathSpace tree. The only persistent inputs are PathSpace itself and the session store, so frontends scale horizontally once they share those roots.

### Supported topologies
| Pattern | When to use | Notes |
| --- | --- | --- |
| **Embedded fan-out** | Multiple server threads/processes inside the same binary as the PathSpace workload. | Each worker shares an in-process `ServeHtmlSpace` so reads/writes go through the host PathSpace context. Use this while load stays on a single host. |
| **Sidecar per host** | Dedicated PathSpace process plus one `pathspace_serve_html` sidecar on the same machine. | Communicate over shared memory/IPC; mount the same `/system/applications` tree so renders and ops stay in sync. Frontends remain stateless and rely on a shared session path. |
| **Remote/clustered** | Multiple web frontends on separate hosts behind a load balancer. | Requires the distributed PathSpace mount (Plan_Distributed_PathSpace.md) so every frontend can read `/system/applications` and `/system/web/sessions`. Assets move through shared storage or CDN. |

### Shared state requirements
1. **Session store:** Run the server with `--session-store pathspace --session-store-root /system/web/sessions/<cluster>` (mirrors `[auth.session_store]` in `docs/web_server.toml`). All instances then read/write the same JSON session blobs so cookies issued by node A remain valid on node B.
2. **Consistent PathSpace roots:** Point `--apps-root`, `--users-root`, and `--renderer` at the same namespace for every instance. When a server runs remotely, mount the target PathSpace via the distributed transport before launching the adapter.
3. **HTML bundle replication:** Watch `renderers/<renderer>/targets/html/<view>/output/v1/html/*` and push DOM/CSS/JS/commands/`assets_manifest.txt` into a shared directory or object store. Frontends either read directly from PathSpace (single host/sidecar) or from a staged mirror that is atomically swapped when new revisions land.

### Load balancers, affinity, and health checks
- Route HTTP traffic through NGINX/Envoy with sticky sessions bound to the HTTP-only cookie (default `ps_session`). SSE clients must hit the same backend for the lifetime of the stream; configure the proxy for cookie or IP affinity and disable proxy buffering on `/apps/*/events`.
- Configure health checks against `/healthz` plus a PathSpace-specific probe (e.g., `GET /apps/<app>/<view>?format=json`) before putting a node in rotation. Failed checks should eventually clean the node’s `/system/web/sessions/<cluster>/<id>` entries to avoid dangling cookies.
- When using CDN-backed assets, pin `/assets/<app>/<relative>` to cache tier while keeping `/apps/<app>/<view>` on the origin so DOM refreshes always reflect the newest revision.

### Rollout checklist for a new node
1. Mount or join the target PathSpace instance and verify reads under `/system/applications` succeed.
2. Seed credentials (or rely on the shared `/system/auth/users/*` entries) and confirm `POST /login` persists sessions under the shared path.
3. Pre-warm HTML bundles by hitting `/apps/<app>/<view>` once per app so the asset index is ready before the load balancer opens the route.
4. Validate SSE stickiness by connecting two browsers, killing one backend, and ensuring clients reconnect with `Last-Event-ID` without losing revisions.
5. Document the node inside `docs/web_server.toml`’s new `[scaling.multi_instance]` block so operators know which cluster path and asset staging strategy it follows.

Use this section as the canonical reference when someone asks “how do we run three web servers against the same PathSpace?” and link to it from deployment notes, runbooks, and incident retrospectives.

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
- HTTPS termination is handled entirely at the proxy today. A reference `nginx.conf` stanza:

  ```
  server {
      listen 443 ssl;
      server_name demo.pathspace.local;

      ssl_certificate /etc/letsencrypt/live/demo.pathspace.local/fullchain.pem;
      ssl_certificate_key /etc/letsencrypt/live/demo.pathspace.local/privkey.pem;
      include /etc/letsencrypt/options-ssl-nginx.conf;
      ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

      location / {
          proxy_pass http://127.0.0.1:8080;
          proxy_set_header Host $host;
          proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
          proxy_set_header X-Forwarded-Proto $scheme;
          proxy_set_header Connection '';
          proxy_buffering off;
      }
  }

  server {
      listen 80;
      server_name demo.pathspace.local;
      return 301 https://$host$request_uri;
  }
  ```

  Use `certbot --nginx` (or caddy's automatic TLS) to provision/renew certificates, feed `/api/healthz` into your load balancer checks, and set `STRICT-TRANSPORT-SECURITY` headers at the proxy so browsers stay on HTTPS even though the adapter itself still speaks HTTP locally.

## API Surface
- Note: once JSON serialization support lands (see plan & backlog), `/api/path` and SSE updates will use the shared JSON encoder for consistent output across web/debug tooling.
- **Static routes:** `/apps/<app>/<view>`, `/assets/<app>/<fingerprint>`, `/apps/<app>/<view>/events`.
- **REST:** 
  - `GET /api/path/<encoded>` → JSON snapshot (value, children) with permission filtering.
  - `POST /api/path/<encoded>` → Insert/update; payload validated against allowlist under `app_root/config/web_api_allowlist`.
  - `POST /api/ops/<op>` → Enqueue operation into `/system/applications/<app>/ops/<op>/inbox/queue`. Accepts JSON `{"app":"<app>","schema":"<schema>","payload":{...}}` (≤ 1 MiB, content-type JSON, auth required) and echoes `{status,app,op,queue,bytes}` so callers can correlate with reducer logs.
  - `GET /api/healthz` (liveness) / `GET /api/readyz` (PathSpace connectivity).
- **Error contract:** JSON `{ "error": { "code": "...", "message": "..." } }`, mapping PathSpace errors to HTTP status codes.

## Security & Hardening
- Force HTTPS (redirect HTTP) outside dev. Provide helper script for dev certificates.
- Use HTTP-only, Secure cookies with SameSite=Lax (configurable). Store CSRF token per session and require header `X-CSRF-Token` on mutating requests.
- Content-Security-Policy baseline: `default-src 'self'; img-src 'self' data:; script-src 'self'; style-src 'self' 'unsafe-inline'`.
- Enforce payload size limits (1 MB default) and token-bucket rate limiting per IP + per session. `pathspace_serve_html` now exposes `--rate-limit-{ip,session}-{per-minute,burst}` so operators can tune the token buckets, returns `429` + `{ "error": "rate_limited" }`, and appends structured `{ts,scope,route,remote_addr,session}` entries to `<app_root>/io/log/security/request_rejections` (global events fall back to `/system/applications/io/log/security/...`).
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
- The implementation tags every event with `id=<revision>`, emits `retry: 2000`, and compares the current revision against the last acknowledged revision: gaps trigger a `reload` event, otherwise clients receive `frame` and `diagnostic` payloads in lockstep with `output/v1/common` and `diagnostics/errors/live`.
- Clients debounce fetches (≥100 ms) and re-fetch DOM on reconnect. Reconnect strategy: exponential backoff capped at 5 s with jitter.
- Track missed revisions by storing last served revision in session; if gap detected, send `"reload"` message forcing full refresh.
- Document WebSocket upgrade path for future bi-directional features (input streaming, logs).

## Testing & Tooling
- Integration harness (`scripts/test_web_server.sh`) spins up PathSpace fixtures, runs the server, executes headless browser tests (Playwright/Puppeteer) to validate renders, SSE refresh, auth, asset caching.
- `tests/tools/test_pathspace_serve_html_sse.py` launches `pathspace_serve_html --seed-demo --demo-refresh-interval-ms` and asserts that `/apps/<app>/<view>/events` produces advancing `frame` revisions plus `diagnostic` events; wire it into CTest so the compile loop keeps the SSE contract honest.
 - `tests/tools/test_pathspace_serve_html_sse.py` launches `pathspace_serve_html --seed-demo --demo-refresh-interval-ms`, asserts that `/apps/<app>/<view>/events` produces advancing `frame` and `diagnostic` payloads, and now reconnects with `Last-Event-ID` to prove resume logic skips already acknowledged revisions.
- `tests/tools/test_pathspace_serve_html_http.py` launches the server with auth required, logs in via `POST /login`, fetches `/apps/<app>/<view>` for the HTML+JSON responses, downloads `/assets/<app>/<relative>` to confirm immutable cache headers and 304 hits, scrapes `/metrics`, and sleeps past the short session timeout to verify auth expiry and cookie revocation.
- `tests/tools/test_pathspace_serve_html_google.py` stands up a stub OAuth/JWKS server, drives `/login/google` through `/login/google/callback`, asserts PKCE parameters, and proves that ID tokens are verified + mapped before the demo HTML route responds.
- Provide mock PathSpace client for unit testing handlers without full engine.
- Developer workflow: `npm run serve-html` (or `./scripts/run_dev_web_server.sh`) while a watcher runs `./build/paint_example --export-html out/html/paint` or `./build/widgets_example --export-html out/html/widgets` to refresh bundles; README.md now documents the flags.
- `docs/AI_Debugging_Playbook.md` §9 (“Web Server Adapter”) now captures log paths (`app_root/io/log/web`, `.../security`), curl smoke commands, SSE debugging, and auth troubleshooting so contributors have a single runbook when the adapter misbehaves.

## Observability
- Metrics: request latency histogram, render enqueue latency, SSE event/connection counters, asset cache hit/miss totals, auth failures, and rate-limit rejections are now recorded by the in-process collector. Scrape them via the authenticated `/metrics` endpoint (Prometheus text format) or read the JSON snapshot mirrored under `<apps_root>/io/metrics/web_server/serve_html/live` for dashboards.
- Structured metrics snapshot: the same JSON node includes timestamps, per-route totals/errors, SSE stats, and rate-limit tallies so log aggregators can ingest a structured feed without tailing stdout. Per-request JSON logging remains a follow-up once we decide where those logs should live; today the security queue plus metrics snapshot cover the oncall dashboards.
- Diagnostics bridge: SSE diagnostics already stream `diagnostics/errors/live`; the metrics snapshot now references the same revision IDs so tooling can tie frame progress to renderer state.
- Alerting guidance: start with 5xx > 1% for five minutes, SSE connection spikes (gauge > expected), or rate-limit counters climbing unexpectedly. Extend thresholds once `/metrics` is wired into Grafana/Prometheus for the hosted deployments.

## Prototype Server (`pathspace_serve_html`)
- `tools/serve_html.cpp` builds the `pathspace_serve_html` binary (no external frameworks, just `httplib`). Use it for Phase 0 manual validation before the full adapter lands.
- CLI options (defaults in parentheses):
  - `--host <addr>` (`127.0.0.1`), `--port <n>` (`8080`).
  - `--apps-root <path>` (`/system/applications`) and `--renderer <name>` (`html`) to map `/apps/<app>/<view>` onto `renderers/<renderer>/targets/html/<view>/output/v1/html/*`.
  - `--seed-demo` injects a demo app (`/apps/demo_web/gallery`) with DOM/CSS/commands so contributors can hit `http://127.0.0.1:8080/apps/demo_web/gallery` immediately.
    It also seeds `/system/auth/oauth/google/google-user-123` with the `demo` username so the Google Sign-In test harness has a default mapping.
  - `--demo-refresh-interval-ms <n>` (dev-only) keeps the demo app's frameIndex/diagnostics ticking so SSE clients and tests see live updates without wiring a renderer.
  - `--session-store {memory|pathspace}` controls where authenticated sessions live; the `pathspace` backend persists them under `/system/web/sessions`. Pair it with `--session-store-root <path>` to select a custom PathSpace subtree for session data.
  - `--rate-limit-ip-per-minute`, `--rate-limit-ip-burst`, `--rate-limit-session-per-minute`, and `--rate-limit-session-burst` cap bursts per client IP and per authenticated session (defaults: 600/120 per IP, 300/60 per session). Setting a value to zero disables that bucket.
  - `--google-client-id/--google-client-secret/--google-redirect-uri` (plus optional `--google-{auth,token,jwks}-endpoint`, `--google-users-root`, `--google-scope`) enable the Google Sign-In flow. They mirror `[auth.google]` in `docs/web_server.toml` and emit `/login/google` → `/login/google/callback` redirects when populated.
- `/apps/<app>/<view>/events` streams SSE with `frame`, `diagnostic`, `reload`, and `error` payloads, sets `id=<revision>` for Last-Event-ID resume, emits `retry: 2000`, and disables proxy buffering via `X-Accel-Buffering: no`.
- `/assets/<app>/<relative>` now serves the renderer’s binary payloads by watching `output/v1/html/assets/{data,meta}`. Requests are authenticated, emit `Cache-Control: public, max-age=31536000, immutable`, honor `If-None-Match` via `ETag = "r<revision>:<relative>"`, and expose `X-PathSpace-App/View/Asset` so logs/CDNs can be inspected quickly. The server seeds `images/demo-badge.txt` for the demo app so smoke tests can verify the flow immediately.
- The prototype currently reads `dom`, `css`, `js`, `commands`, and `revision` nodes, emits an inline HTML template (or returns the stored DOM verbatim), and adds `ETag` + `Cache-Control: no-store`. It logs bind failures and exposes `/healthz` for smoke tests.
- Limitations to track in later phases:
  - Asset serving is best-effort per request today; CDN pushes, background prefetch, and TOML-driven renderer/asset mappings still need to land before production.
  - A single renderer ID is shared across all apps for now; once config plumbing lands, map views to renderer/target pairs per app.
  - Auth/SSE/diagnostics exist now, but the server still runs in-process with the demo PathSpace instance and lacks remote mount plumbing or CDN-facing asset streaming.
- The server reads from the in-process PathSpace instance. Until we wire distributed mounts or shared contexts, run it inside the same binary that populates `output/v1/html/*` or rely on the demo seed.
- Open TODOs captured in later phases: CDN-backed asset replication, config-driven renderer mapping, and broader integration tests (see Next Actions below).

## Dual Delivery Demo
- `examples/paint_example` and `examples/widgets_example` now expose `--serve-html` (plus `--serve-html-{host,port,view,target,user,password}` and `--serve-html-allow-unauthenticated`) to run the native window and browser in parallel.
- The flag mounts an HTML renderer target (`--serve-html-target`) next to the native view, mirrors every present via `Window::Present(..., view="--serve-html-view")`, and spawns an embedded `pathspace_serve_html` thread bound to the current `PathSpace` context.
- Default credentials (`demo` / `demo`) are seeded under `/system/auth/users/demo/password_bcrypt`; users can pass custom bcrypt pairs and the helpers re-hash them before starting the server.
- Usage:
  1. `./build/paint_example --serve-html` (or widgets) launches the native window.
  2. Visit `http://127.0.0.1:8080/apps/<app>/<view>` (e.g., `/apps/paint_example/web`) and log in with the CLI-supplied credentials.
  3. Drawing in the native window updates the browser DOM in lock-step via the HTML mirror + SSE stream.
- The embedded server shares the same CLI config surface as `pathspace_serve_html` (host/port/cookie/session timeouts) so future config files can drive both standalone and embedded modes identically.
- Automated dual-delivery tests (browser ↔ native state propagation) remain TODO and are tracked in `docs/AI_TODO.task`.

## Session/Auth Baseline — December 2, 2025
- **Login required by default.** `pathspace_serve_html` now authenticates every `/apps/<app>/<view>` request via `POST /login` before returning HTML. Only the `--allow-unauthenticated` flag keeps the Phase 0 behaviour for local iteration.
- **Credential storage.** User secrets live under `/system/auth/users/<user>/password_bcrypt`. The helper seeds a demo account (`demo` / `demo`) whenever `--seed-demo` is passed so developers can test the flow without hand-authoring hashes.
- **Bcrypt enforcement.** Password verification uses the vendored OpenWall bcrypt implementation (CC0). All hashes are compared with `bcrypt_checkpw`, keeping the existing “bcrypt everywhere” requirement from the plan. Hash creation for demos uses the same path.
- **Session plumbing.** Successful logins return `{"status":"ok","username":"<user>"}` and set an HTTP-only `ps_session` cookie (name is configurable). `POST /logout` revokes the token and clears the cookie, while `GET /session` reports the current authentication state so browsers can bootstrap UI.
- **Config surface.** New CLI switches map directly onto the `web_server.toml` schema captured in `docs/web_server.toml`: `--users-root`, `--session-cookie`, `--session-timeout`, `--session-max-age`, and `--allow-unauthenticated`. The TOML doc explains how these will be persisted once the adapter reads config from disk.
- **Timeouts.** Idle timeout defaults to 30 minutes and absolute lifetime to 8 hours. Cookies inherit the absolute timeout when set; zero values fall back to session-only cookies so operators can tune policies.
- **Docs + instrumentation.** `/session` responses and the CLI help text now document the login flow so inspector tooling can reuse the same middleware when it mounts inside the adapter.

## HTML Export Flags — December 2, 2025
- `examples/paint_example.cpp` and `examples/widgets_example.cpp` now accept `--export-html <dir>`. The flag launches the declarative app headlessly, renders the bound scene through the HTML adapter, and writes a disk bundle matching `renderers/<renderer>/targets/html/<view>/output/v1/html/*`.
- Bundle layout: `dom.html`, `styles.css`, `commands.json`, `metadata.txt`, `assets_manifest.txt`, and `assets/<logical>` blobs (PNG, WOFF2, etc.). Each run replaces the target directory, making it trivial for a file watcher to rsync into whatever staging folder `pathspace_serve_html` or a CDN mount expects.
- The export flag implies headless mode and is intentionally incompatible with screenshot/GPU smoke options so we do not have to juggle surface rebinds mid-run. When the renderer emits a canvas fallback the metadata records `mode=canvas` and `usedCanvasFallback=true`, mirroring the nodes under `output/v1/html`.
- This satisfies Next Action 3 by letting the prototype server serve real paint/widgets output instead of the hard-coded demo seed; future automation can wrap the binaries (e.g., `./scripts/run_dev_web_server.sh` + watcher) without needing new C++ entry points.

## Extensibility & Configuration
- `web_server.toml` sections: `[server]`, `[paths]`, `[assets]`, `[security]`, `[apps.<app>]` (per-app overrides: renderer target IDs, allowed REST paths, SSE options).
- Allow middleware plugins (analytics, custom auth) registered via config. Provide stable hook interface (request/response interceptors).
- Document onboarding for new apps: configure renderer target, export HTML outputs, add entry to config with optional route aliases.

## Implementation Phases
1. **Phase 0 — Prototype (local only)**
   - ✅ `pathspace_serve_html` prototype exists (December 2, 2025). It exposes `/apps/<app>/<view>`, reads DOM/CSS/commands/JS/revision, and can seed a demo gallery via `--seed-demo`.
   - No auth; assume single developer session.
   - Serve paint/widgets examples by reading from `output/v1/html` once those examples gain the `--export-html` flag (Next Action 3).
   - Documented above; keep usage notes current as we add asset streaming and SSE hooks.
2. **Phase 1 — Session & Static Assets**
   - ✅ (2025-12-02) Add simple username/password auth stored in PathSpace, enforce bcrypt verification, and issue HTTP-only cookies (`ps_session` by default).
   - ✅ (2025-12-03) Serve assets via `/assets/<app>/<fingerprint>` with immutable cache headers. `pathspace_serve_html` now maintains a per-app asset index whenever `/apps/<app>/<view>` is requested, streams `/assets/data/*` + `/assets/meta/*`, honors `If-None-Match`, and tags responses with `X-PathSpace-*` diagnostics so CDN/front-ends can see which view supplied the bytes.
   - ✅ (2025-12-03) Write integration tests (e.g., curl scripts or a C++ test harness) to assert correct responses. `tests/tools/test_pathspace_serve_html_http.py` now logs in, fetches HTML/JSON, then requests `/assets/demo_web/images/demo-badge.txt` to verify the 200 response, MIME type, cache headers, and demo bytes.
3. **Phase 2 — Live Updates & Error Handling**
   - ✅ (2025-12-03) `/apps/<app>/<view>/events` serves SSE (frame, diagnostic, reload, error) with Last-Event-ID resume, keepalive comments, and proxy-buffering guards; demo mode can auto-refresh for tests.
   - ✅ (2025-12-03) Browser responses now inline an EventSource-driven script that listens to `/events`, fetches `/apps/<app>/<view>?format=json` for DOM/CSS/JS/commands diffs, and renders diagnostic banners + reload cues automatically—manual refresh is no longer required.
4. **Phase 3 — Dual Native/Web Support**
   - ✅ (2025-12-03) Standardized `POST /api/ops/<op>` so authenticated browsers submit JSON (≤ 1 MiB with `{app,schema,payload}`) that the server writes to `/system/applications/<app>/ops/<op>/inbox/queue`. The seeded demo button now posts `demo_refresh` ops and `tests/tools/test_pathspace_serve_html_http.py` asserts the endpoint.
   - ✅ (2025-12-03) `paint_example` and `widgets_example` now accept `--serve-html` (plus host/port/user/password overrides) which attach an HTML mirror view and launch an embedded `pathspace_serve_html` instance sharing the active `PathSpace`. Developers can paint in the native window while loading `http://<host>:<port>/apps/<app>/<view>` in a browser to watch edits land live.
5. **Phase 4 — Hardened Deployment**
   - ✅ (2025-12-03) Added a pluggable session store (`--session-store {memory|pathspace}` + `--session-store-root`) so production hosts can persist session state under `/system/web/sessions`, and captured HTTPS termination guidance (NGINX/Caddy snippets, certificate workflow, proxy health checks) in this plan + `docs/web_server.toml`.
   - ✅ (2025-12-03) Documented scaling options for multi-instance deployments (see "Scaling & Multi-Server Deployments" above) so multiple frontends can safely share sessions, assets, and PathSpace roots.
   - ✅ (2025-12-03) Added per-IP and per-session token-bucket middleware (configurable via `--rate-limit-*` flags) plus structured audit entries under `<app_root>/io/log/security/request_rejections`; the HTTP integration test now drives a low-burst configuration to assert 429 responses and JSON `{ "error": "rate_limited" }` payloads.
6. **Phase 5 — Observability & Tooling**
   - ✅ (2025-12-03) Added the ServeHtml metrics collector (`/metrics` Prometheus endpoint plus `<apps_root>/io/metrics/web_server/serve_html/live` JSON snapshot) capturing per-route latency histograms, render enqueue latency, SSE connections/events, asset cache hits, auth failures, and rate-limit rejections.
   - ✅ (2025-12-03) Extended `tests/tools/test_pathspace_serve_html_http.py` (metrics scrape, asset 304, auth expiry) and `tests/tools/test_pathspace_serve_html_sse.py` (Last-Event-ID reconnect) so observability regressions fail in CI.
   - ✅ (2025-12-03) Updated `docs/AI_Debugging_Playbook.md` §9 with `/metrics` curl examples, PathSpace metrics paths, and the auth expiry/SSE reconnect triage notes.
7. **Phase 6 — External IdP Integration**
   - ✅ (2025-12-03) Implemented Google Sign-In (Authorization Code + PKCE). `/login/google` now redirects to the configured OAuth endpoint, stores state/code-verifier pairs, and `/login/google/callback` exchanges codes, verifies ID token signatures/audience against JWKS, and maps Google `sub` values to usernames stored under `/system/auth/oauth/google/<sub>` before issuing the standard session cookie. CLI + `[auth.google]` knobs cover client id/secret, redirect URI, endpoints, scopes, and mapping roots.
   - ✅ (2025-12-03) Added automated regression `tests/tools/test_pathspace_serve_html_google.py` that spins up a stub OAuth/JWKS server, drives `/login/google` end-to-end, asserts PKCE parameters, and confirms the token exchange writes to the correct PathSpace queue.
   - ✅ (2025-12-03) Documented the Google setup flow (config file references, node layout, debugging steps) inside this plan, `docs/AI_Debugging_Playbook.md` (§9.6), and `docs/web_server.toml` so hosts know how to register OAuth clients and seed mappings safely.

Backlog tracking: Phases 3–5 are now represented in `docs/AI_TODO.task` as “Web Server Dual Delivery Parity,” “Web Server Hardened Deployment,” and “Web Server Observability & Tooling” so their acceptance criteria stay visible even while the plan focuses on Phase 2 polish.

## Required Updates Elsewhere
- `docs/Plan_SceneGraph_Renderer.md` — Reference this plan; document the HTML server’s role.
- `docs/AI_Architecture.md` — Add note under HTML adapter about server delivery.
- `docs/AI_Debugging_Playbook.md` — Include troubleshooting steps for web server logs/SSE.
- `docs/AI_Todo.task` — Log implementation steps with priorities.
- `docs/web_server.toml` — New `[auth.google]` stanza documents client id/secret, redirect URI, endpoints, and mapping roots for the Google Sign-In flow.
- Example README(s) — Add instructions for launching the server and visiting the browser demo.

> **Dependency:** Remote deployments assume distributed mounts per `docs/Plan_Distributed_PathSpace.md` so the web server can reach user app roots securely.

## Open Questions
- **API Transport** — Evaluate when we will need HTTP/2 or gRPC layering on top of the native C++ HTTP server (e.g., for sidecar/cluster deployments).
- **Incremental HTML Updates** — Finalize the JSON diff/event model and fallback strategy for streaming DOM updates.
- **Session Tooling** — Decide how developers manage auth sessions (CLI helpers, inspection endpoints) without introducing duplicative configuration.

## Next Actions
1. ✅ (2025-12-02) Created `tools/serve_html` prototype (`pathspace_serve_html`). Serves `/apps/<app>/<view>` straight from `renderers/<renderer>/targets/html/<view>/output/v1/html/*`, seeds demo data, and documents the usage in this plan. Follow-ups: support asset streaming + SSE before exposing it outside dev loops.
2. ✅ (2025-12-02) Defined session/auth middleware (bcrypt validation, HTTP-only cookies, `/login`/`/logout`/`/session` routes) and captured the config surface in `docs/web_server.toml` so hosts can wire the same values outside CLI flags.
3. ✅ (2025-12-02) Added `--export-html <dir>` to paint/widgets examples so they stream DOM/CSS/commands/assets to disk for the server/dev loop; README now documents the workflow and bundles mirror `output/v1/html/*`.
4. ✅ (2025-12-03) Implemented `/apps/<app>/<view>/events` SSE endpoint with Last-Event-ID resume logic, reload notices, and diagnostic streaming tied to `output/v1/common/frameIndex` + `diagnostics/errors/live`; added the dev-only `--demo-refresh-interval-ms` flag plus `tests/tools/test_pathspace_serve_html_sse.py` to verify frame + diagnostic events.
5. ✅ (2025-12-03) Added integration tests executing server → PathSpace → browser flow (headless fetch) verifying 200 responses, auth gate rejection, and caching headers via `tests/tools/test_pathspace_serve_html_http.py` (CTest target `PathSpaceServeHtmlHttp`).
6. ✅ (2025-12-03) Updated documentation references (`docs/Plan_Overview.md`, `docs/AI_Debugging_Playbook.md`) and added backlog entries for Phases 3–5 in `docs/AI_TODO.task` so downstream teams know where to find the troubleshooting guide and the remaining work.
7. ✅ (2025-12-03) `PathSpaceUITests` Loop 4 regression (log: `build/test-logs/PathSpaceUITests_loop4of5_20251203-095627.log`) is resolved. `SceneLifecycle::InvalidateThemes` now immediately marks all app + window widgets dirty in addition to waking the lifecycle worker, so `ThemeConfig::SetActive` triggers deterministic dirty events. `tests/ui/test_DeclarativeSceneLifecycle.cpp` passes locally and the 5× `./scripts/compile.sh --loop=5 --loop-label PathSpaceUITests --per-test-timeout 20` run completes without flakes.
8. ✅ (2025-12-03) Landed Phase 1 asset delivery: `/assets/<app>/<relative>` streams `output/v1/html/assets/{data,meta}` with `Cache-Control: public, max-age=31536000, immutable`, `ETag`/`If-None-Match` support, and demo seed data (`images/demo-badge.txt`). The HTTP integration test now exercises the route to guard the cache headers and payload bytes.
9. ✅ (2025-12-03) Phase 3 kickoff: `/api/ops/<op>` accepts JSON (`{"app","schema","payload"}`) and writes to `/system/applications/<app>/ops/<op>/inbox/queue`, returning `{status,queue}` metadata. The demo button now fires `demo_refresh` ops and the HTTP integration test posts to the endpoint to guard auth + payload size limits.
10. ✅ (2025-12-03) The declarative paint and widgets examples gained `--serve-html` + credential flags so contributors can launch the native window and browser view at the same time. The flag attaches an HTML mirror target, seeds bcrypt credentials (`demo`/`demo` by default), and spawns an embedded `pathspace_serve_html` thread pointing at `/apps/<app>/<view>`.
11. ✅ (2025-12-03) Phase 5 observability landed: `pathspace_serve_html` now exposes `/metrics`, mirrors structured counters under `<apps_root>/io/metrics/web_server/serve_html/live`, tracks SSE/asset/auth/rate-limit stats, and the integration suites cover `/metrics`, Last-Event-ID reconnect, asset 304 behavior, and forced session expiry.
12. ✅ (2025-12-03) Phase 6 external IdP integration shipped: `/login/google` + `/login/google/callback` wrap Google OAuth (PKCE), verify ID tokens via JWKS, and look up `/system/auth/oauth/google/<sub>` → username mappings before issuing sessions. `docs/web_server.toml` now carries `[auth.google]`, `docs/AI_Debugging_Playbook.md` gained the troubleshooting flow, and `tests/tools/test_pathspace_serve_html_google.py` guards the happy path with a stub OAuth server.
