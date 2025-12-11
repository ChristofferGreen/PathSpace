# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for distributed PathSpace mounts and networking.
> **Status (December 6, 2025):** Plan completed and archived; this finished document captures the final distributed mount contract (phases 0–2, TLS transport, throttling, diagnostics mirroring) for future reference.

# PathSpace — Distributed Mount Plan

## Goals
- Allow one PathSpace instance ("client") to mount another remote instance ("server") under a local subtree, similar to mounting a network filesystem.
- Preserve PathSpace semantics (atomic inserts, wait/notify, immutable snapshots, diagnostics) across network boundaries with predictable consistency and latency.
- Provide secure, authenticated connections suitable for multi-user environments (per-user roots, ACL enforcement).
- Serve as the foundation for remote HTML presentation, external tooling, and multi-machine coordination.

## Non-Goals (v1)
- Conflict resolution for concurrent writers to the same remote path (first version assumes single writer per mounted subtree).
- WAN-optimized replication or offline operation (focus on LAN / data-center latency).
- Cross-version protocol negotiation beyond a rolling window (assume server and client are upgraded roughly together; document compatibility window).

## Architecture Overview

```
Local PathSpace (client) ── RPC/stream ── Remote PathSpace (server)
        │                                         │
  mount /remote/app/…                      export /users/<user>/system/applications/<app>
```

### Components
- **Mount Manager (client)** — handles mount configuration, maintains connection/session, translates local operations to RPC calls, and merges remote updates into the local trie namespace (JSON serialization support will enable debug views).
- **Mount Manager (client)** — handles mount configuration, maintains connection/session, translates local operations to RPC calls, and merges remote updates into the local trie namespace (JSON serialization support will enable debug views). Each mount now advertises a `take_batch_size` (default 4) so queue drains fetch multiple entries per RPC and feed subsequent local `take` calls from a cache backed by the batched `TakeResponse.values[]` payloads.
- **Export Manager (server)** — authenticates clients, enforces ACLs, and serves requests (read/insert/take, wait, snapshot stream, diagnostics). Manages subscriptions for wait/notify.
- **Protocol Layer** — transports RPCs and streaming updates over standalone Asio-based networking (aligning with the C++ Networking TS trajectory). Layer protobuf or Cap’n Proto schemas on top, including streaming for notifications and snapshot chunks.
- **Security Layer** — mutual TLS with client certificates or token-based auth (e.g., OAuth2 access tokens scoped to exported subtrees).

## Mount Semantics
- **Read/Insert/Take** — operations within mounted subtree forward to server. Results (values, errors) mirror local semantics.
- **Wait/Notify** — register waits on remote server; server streams notifications to client, which wakes local waiters.
- **Snapshots** — remote `output/v1/*` files fetched lazily or via streaming channel. Consider caching to reduce bandwidth.
- **Diagnostics** — mirror `diagnostics/errors/live`, metrics, logs under the mounted subtree.
- **Shutdown** — clean unmount on disconnect; client clears cached nodes and wakes waiters with error status.

- **Single-writer assumption:** One logical writer per mounted subtree; clients coordinate out-of-band to avoid conflicts.
- **Atomic operations:** Each insert/read/take spans a single RPC. For batch inserts, use server-side transaction to maintain atomicity.
- **Latency:** Client operations block until server responds; configure timeouts and expose metrics.
- **Caching:** Optional read-cache for immutable paths (e.g., snapshots). Cache invalidation triggered by server notifications.
- **Failure handling & logout semantics:** If the connection drops or the client logs out, treat it as session termination. The server tears down the mount, notifies remote applications to enter their shutdown flow, and releases resources. Any subsequent access requires a fresh `MountOpen` (no automatic resume). Distinguish between graceful close vs. abrupt drop in logs/metrics.

## Security Model
- **Authentication:** Mutual TLS with client certificates (default). Allow token-based auth for web-facing deployments (JWT/OAuth2 bearer tokens).
- **Authorization:** Server exports explicit subtrees via capability-aware adapters; actual per-path rights are determined by the hosting application's `Capability` object (`can_show`, `can_read`, `can_write`, `can_execute`). Deny access outside exported namespace automatically.
- **Encryption:** TLS 1.3 with modern ciphers. Optional mTLS or token + TLS depending on deployment.
- **Auditing:** Server logs mount attempts, authenticated principals, operations (optional sampling), and data volume. Client logs connection status and mount lifecycle events.

## Protocol Design
- **Transport:** Asio-based TLS connections with a lightweight length-prefixed framing. Each frame carries a gzip-compressed JSON payload (UTF-8 before compression) so messages stay debuggable while remaining compact. Evaluate layering HTTP/2/gRPC later if needed.
- **Message format:** JSON documents with a `type` field (`ReadRequest`, `Notification`, etc.) plus payload data; compressed with gzip before framing. Include `protocolVersion`, `sessionId`, and optional `requestId` fields for tracking.
- **RPCs:** 
  - `MountOpen`, `MountClose`
  - `Read`, `Insert`, `Take`
  - `List` (optional for introspection)
  - `SubscribeNotifications` (server streaming)
  - `FetchSnapshotChunk` (streaming)
- **Batch pops:** `TakeRequest` includes `max_items` (capped at 64) and `TakeResponse` returns `values[]` so clients can prefetch queue entries. The legacy `value` field remains for backward compatibility, but new transports should rely on the array form so `RemoteMountManager` can cache extras per path.

### Protocol Schema (Phase 0 contract)
- **Envelope:** All frames share `{ "type": "<FrameKind>", "sent_at_ms": <uint64>, "payload": { … } }`. `FrameKind` values are `MountOpenRequest`, `MountOpenResponse`, `ReadRequest`, `ReadResponse`, `WaitSubscribeRequest`, `WaitSubscribeAck`, `Notification`, `Heartbeat`, and `Error`.  
  The canonical encoder/decoder lives in `src/pathspace/distributed/RemoteMountProtocol.{hpp,cpp}` and is covered by `tests/unit/distributed/test_RemoteMountProtocol.cpp`.
- **MountOpenRequest payload:** `{ version:{major,minor}, request_id, client_id, mount_alias, export_root, capabilities:[{name,parameters[]}], auth:{kind:"mtls"|"bearer", subject, audience?, proof, fingerprint?, issued_at_ms, expires_at_ms} }`.  Absolute paths are required for `export_root`; aliases are `[A-Za-z0-9_-]+`.
- **MountOpenResponse payload:** `{ version, request_id, accepted, session_id, lease_expires_ms, heartbeat_interval_ms, granted_capabilities[], error? }`. `error` carries `{code,message,retryable,retry_after_ms?}`.
- **ReadRequest payload:** `{ request_id, session_id, path, include_value, include_children, include_diagnostics, consistency?: { mode:"latest"|"at_least_version", version? } }`.
- **ReadResponse payload:** `{ request_id, path, version, success, value?: {encoding,type_name,schema_hint?,data_base64}, error? }` with `encoding="typed/slidingbuffer"` as the default so readers decode via the registered `InputMetadata` instead of peeking at raw `NodeData`.
- **WaitSubscribeRequest payload:** `{ request_id, session_id, subscription_id, path, include_value, include_children, after_version? }`.
- **WaitSubscribeAck payload:** `{ subscription_id, accepted, error? }` so clients learn if the server rejected or throttled a waiter.
- **Notification payload:** `{ subscription_id, path, version, deleted, value? }` replicates the write that satisfied a waiter.
- **Heartbeat payload:** `{ session_id, sequence }` keeps connections alive; servers drop sessions after two missed intervals.
- **Error payload:** `{ code, message, retryable, retry_after_ms? }` doubles as the standalone `Error` frame or nested errors inside other messages.

### Security Requirements (Phase 0)
- **Transport:** TLS 1.3 is mandatory. Server certificates must pin to the deployment CA; clients present certificates when `auth.kind == "mtls"`.
- **AuthContext:** `subject` carries the authenticated principal (e.g., CSR `CN`). `proof` stores either the presented certificate fingerprint (`mtls`) or a signed bearer token hash. `audience` scopes the token; `fingerprint` mirrors the DER digest used for audit logging.
- **Token mode:** When `auth.kind == "bearer"`, `proof` must contain a JWT or opaque token and `fingerprint` records the issuing authority. The server verifies expiry (`expires_at_ms`), `audience`, and capability scope before issuing `MountOpenResponse`.
- **Session lease:** Servers cap sessions via `lease_expires_ms` and expect periodic `Heartbeat` frames; clients must re-`MountOpen` before the lease lapses and should honor `retry_after_ms` guidance when throttled.
- **Diagnostics:** Log every handshake (`/diagnostics/web/inspector/acl/*`) and publish per-remote metrics under `/inspector/metrics/remotes/<alias>/{status,latency,requests,waiters}` so operators can audit mounts without scraping stdout.
- **Failure semantics:** Disconnects or auth failures emit `Error` frames with explicit codes (`invalid_credentials`, `permission_denied`, `lease_expired`, `too_many_waiters`) so mount managers differentiate between retryable and fatal errors.
  - `GetDiagnostics`
- **Error mapping:** Map PathSpace errors to gRPC status codes with additional detail (retryable vs not).
- **Versioning:** Embed protocol version; server advertises supported range. Define compatibility policy (e.g., N-1).

## Configuration & Deployment
- **Server config (`distributed_server.toml`):** exported subtrees, auth mode, TLS certs/keys, listen address, rate limits.
- **Client config (`mounts.toml`):** remote address, mount path (local), credential reference, retry/backoff policy.
- **Monitoring:** metrics for RPC latency, error rates, bytes transferred, active mounts, wait subscriber counts.
- **Scaling:** support multiple simultaneous mounts; ensure server can handle concurrent clients with resource limits.

## Dependencies
- TLS certificate management (self-signed for dev, ACME/Let’s Encrypt or enterprise CA in prod).
- Capability/permission system (PathSpace "Capability" object) must expose `can_show`, `can_read`, `can_write`, `can_execute` helpers so the server can evaluate path access without duplicating ACL logic.
- Logging enhancements to differentiate local vs remote operations.

## Observability
- Expose metrics: RPC latency, error rates, reconnect counts, bytes transferred, pending notification queue size.
- Publish structured logs for mount lifecycle events (`MountOpen`, `MountClose`, disconnects, auth failures).
- Future enhancement: once PathSpace adds symlink support, mirror remote diagnostics under convenient local paths via symlinks so tooling/inspectors can read them without custom APIs.

## Stakeholder Requirements (Web + Renderer)
### ServeHtml / Web adapter
- **Live update feeds:** `docs/finished/Plan_WebServer_Adapter_Finished.md` ("Live Update Strategy") subscribes to `renderers/<rid>/targets/html/<view>/output/v1/common/frameIndex`, `diagnostics/errors/live`, and `renderers/<rid>/targets/html/<view>/events/renderRequested/queue` before streaming SSE events. The distributed transport must expose those paths verbatim and deliver new revisions within the adapter’s `retry: 2000` / 5 s keep-alive envelope so EventSource clients do not churn.
- **Clustered deployments:** The same plan’s scaling section requires every remote frontend to mount `/system/applications`, `/system/web/sessions`, and the renderer subtree before serving traffic. Export policies in this plan need explicit read scopes for those paths (plus future write scopes for session cleanup) so `pathspace_serve_html` instances can stay stateless even when the PathSpace producer lives elsewhere.
- **Input/ops routing:** Web clients post JSON ops (<= 1 MiB payloads) to `/system/applications/<app>/ops/<op>/inbox/queue` via `POST /api/ops/<op>` (Native/Web Coexistence section). Phase 1 of this plan must guarantee authenticated `insert`/`take` support on those inbox paths, reuse existing `Capability` checks, and surface structured throttling errors recognized by the adapter.
- **Inspector surfacing:** `remote_mounts[]` entries from `web_server.toml` forward directly into `InspectorHttpServer::Options::remote_mounts`. Alias health, ACL denials, and `/inspector/remotes` metrics produced by the mount need to mirror the signals described in `docs/finished/Plan_PathSpace_Inspector_Finished.md` so adapter-hosted inspector dashboards stay accurate. The declarative migration tracker already flags distributed mounts as the blocker for ServeHtml’s "pending" row, so this requirement unblocks that consumer.

### Renderer / SceneGraph stack
- **HTML target surfaces:** `docs/finished/Plan_SceneGraph_Renderer_Finished.md` defines the HTML adapter outputs under `renderers/<rid>/targets/html/<view>/output/v1/html/{dom,css,commands,assets/*}` plus `output/v1/common/frameIndex`. Distributed mounts must replicate these trees exactly and preserve revision ordering to honor the renderer’s AlwaysLatestComplete policy that web presenters rely on.
- **Diagnostics + wait paths:** Renderer diagnostics (`diagnostics/errors/live`, damage/progressive metrics) and the optional `renderers/<rid>/targets/html/<view>/events/renderRequested/queue` feed both ServeHtml SSE and the renderer stress tests. The mount transport should target single-digit hundreds of milliseconds latency so HTML previews refresh before the SSE `retry: 2000` budget, and it must propagate deletes so browser fallbacks do not replay stale DOM/CSS bundles.
- **Target metadata parity:** SceneGraph plans require paired `renderers/<rid>/targets/{surfaces,html}/<view>` entries with shared `RenderSettings`, dirty hints, and history. Remote mounts therefore need to treat the `renderers/*` namespace as single-writer, forward metadata updates atomically, and reject conflicting writers so remote HTML mirrors never diverge from native windows.

### Validation signals
- Measure end-to-end delay between server-side writes to `renderers/<rid>/targets/html/<view>/output/v1/common/frameIndex` and the adapter’s SSE pushes; keep p95 below the 2 s reconnection budget implied by `retry: 2000`.
- Reuse `scripts/ci_inspector_tests.sh --loop=5` (referenced by the web adapter plan) once the distributed mount client exists so Playwright exercises the HTML + inspector stack against a mounted PathSpace.
- Track `/inspector/metrics/remotes/<alias>` latency/error counters for every mount alias so operators can correlate ServeHtml or renderer regressions with transport delays.

## Inspector Integration
- Every exported PathSpace (server side) must host the inspector HTTP surface—embed `InspectorHttpServer` beside the declarative runtime or run the `pathspace_inspector_server` sidecar with matching ACL/session settings—so operators can introspect mounted subtrees without bespoke tooling.
- Remote mounts reuse `InspectorHttpServer::Options::remote_mounts`: the client-side `RemoteMountManager` polls each remote `/inspector/tree`/`/inspector/stream` endpoint, prefixes it under `/remote/<alias>`, and merges the deltas into the local inspector SPA dropdown (`/inspector/remotes`). Document the alias list in mount configs so teams know which remote roots should appear in the UI.
- Inspector diagnostics need to travel with the mount: publish `/inspector/metrics/remotes/<alias>/{status,latency,requests,waiters}` plus `/diagnostics/web/inspector/acl/*` so remote health, ACL denials, and queue depth are visible from a single dashboard. Reference `docs/finished/Plan_PathSpace_Inspector_Finished.md` for the full endpoint/metric catalog and keep the distributed plan in sync whenever new inspector knobs ship.
- When exporting mounts intended for remote inspection, spell out the browser entrypoint (host/port) and the permitted `snapshot.root`/`acl` scopes so clients can safely expose the inspector to trusted roles only. Distributed deployments should document whether the SPA is served directly from the mounted process or proxied through an existing web adapter.

## Plan & Phases
1. **Phase 0 — Prototype Mount (single client/server)**
   - Define protobuf/gRPC schemas.
   - Implement server export of `/users/<user>/system/applications/<app>` (read-only).
   - Implement client mount manager supporting `read` and `wait`.
   - Manual tests over localhost with self-signed TLS.
2. **Phase 1 — Full Operation Support**
   - Add `insert`, `take`, batch operations.
   - Implement wait/notify streaming with proper backpressure.
   - Add diagnostics mirroring and snapshot fetching.
3. **Phase 2 — Security Hardening**
   - mTLS support, configurable ACLs, rate limiting.
   - Introduce a throttling mount wrapper (PathSpace adapter) that proxies requests to the real tree while tracking usage per session and injecting delays once thresholds are exceeded.
   - Audit logging and metrics instrumentation.
   - Robust error handling (retries, exponential backoff, disconnect detection).
4. **Phase 3 — Integration & Tooling**
   - Update web server plan to rely on distributed mounts when PathSpace is remote.
   - Provide CLI (`pathspace mount …`) for manual mount management.
   - Add automated tests (unit, integration with mock server).
5. **Phase 4 — Scalability & Features (optional)**
   - Support multiple exported subtrees, partial mounts.
   - Evaluate caching strategies and diff streaming for high-volume snapshots.
   - Consider optional compression for large value transfers.

## Required Documentation Updates
- `docs/AI_Architecture.md` — add distributed architecture section.
- `docs/AI_Paths.md` — describe mount path conventions and reserved namespaces (e.g., `/remote/<host>/<mount>`).
- `docs/finished/Plan_SceneGraph_Renderer_Finished.md` / `docs/finished/Plan_WebServer_Adapter_Finished.md` — reference distributed mounts where relevant.
- `docs/AI_Debugging_Playbook.md` — troubleshooting steps for network mounts (connectivity, certificate issues).

## Open Questions
- **Conflict resolution:** Do we ever allow multiple writers? If so, what mechanism (last-writer-wins, versioning)?
- **Compression:** Default to gzip-compressed JSON frames (already part of the transport design); revisit if we need alternative codecs for large snapshot payloads.
- **Offline behavior:** How should clients behave if the mount drops mid-operation (retry vs fail-fast)?
- **Mount discovery:** Do we broadcast available exports or rely on explicit configuration?
- **Future multi-writer support:** When needed, evaluate CRDT/operation-transform approaches so multiple clients can edit shared subtrees and converge without central locking. Document design implications (including Capability interactions/permissions) before relaxing the single-writer assumption.

## Next Actions
1. ✅ (December 5, 2025) Review and refine requirements with web/renderer stakeholders — captured in the new "Stakeholder Requirements (Web + Renderer)" section plus updated validation signals.
2. ✅ (December 5, 2025) Finalize protocol schema and security requirements — see the new "Protocol Schema (Phase 0 contract)" and "Security Requirements (Phase 0)" sections plus `RemoteMountProtocol` implementation/tests.
3. ✅ (December 5, 2025) Add backlog items in `docs/AI_Todo.task` for Phase 0 implementation (now expanded with server, client, and diagnostics tasks).
4. ✅ (December 5, 2025) RemoteMountServer Phase 0 export landed in `src/pathspace/distributed/RemoteMountServer.{hpp,cpp}` with doctests (`tests/unit/distributed/test_RemoteMountServer.cpp`). The server enforces alias/root validation, negotiates sessions/heartbeats, emits `typed/slidingbuffer` ValuePayloads (with canonical `type_name` hints) instead of exposing raw `NodeData`, mirrors wait notifications via the shared `NotificationSink`, publishes metrics under `/inspector/metrics/remotes/<alias>/server/*`, and logs auth failures plus lifecycle events under `/diagnostics/web/inspector/acl/<alias>/events/*`.
5. ✅ (December 5, 2025) RemoteMountManager Phase 0 client shipped in `src/pathspace/distributed/RemoteMountManager.{hpp,cpp}` with coverage in `tests/unit/distributed/test_RemoteMountManager.cpp`. The manager mounts exported roots under `/remote/<alias>`, dials the protocol through pluggable session factories, bridges blocking reads onto wait subscriptions, and publishes client metrics under `/inspector/metrics/remotes/<alias>/client/*`.
6. ✅ (December 5, 2025) Update dependent plans (web server, HTML adapter) to reference distributed mounts — `docs/finished/Plan_PathSpaceHtmlServer_Finished.md` now documents the `/remote/<alias>/…` workflow and `RemoteMountSource` option, and `docs/HTML_Adapter_Quickstart.md` teaches operators how to mount remote spaces before running the adapter harnesses.
7. ✅ (December 6, 2025) Land `TypeMetadataRegistry` (`src/pathspace/type/TypeMetadataRegistry.{hpp,cpp}`) plus RemoteMountManager auto-registration so distributed clients keep canonical `type_info::name()` records for every insert/read/take/wait. The registry exposes lookup APIs + a helper macro, giving the typed payload rollout a concrete metadata cache ahead of the protocol changes.

### Phase 0 Backlog Entries (December 5, 2025)
- ✅ `RemoteMountServer Phase 0 Export [FEATURE]` — implemented by `RemoteMountServer` (see details below).
- ✅ `RemoteMountManager Phase 0 Client [FEATURE]` — implemented by `RemoteMountManager`, which inserts `RemoteMountSpace` nodes under `/remote/<alias>`, dials RemoteMountProtocol servers via session factories, mirrors read + wait semantics locally, and exports client metrics/lease signals under `/inspector/metrics/remotes/<alias>/client/*`.
- ✅ (December 5, 2025) `RemoteMount Phase 0 Diagnostics & Manual Tests [TASK]` — `docs/AI_Debugging_Playbook.md` now walks through self-signed TLS generation, MountOpen troubleshooting, and the inspector metrics to watch, and `examples/remote_mount_manual.cpp` (built via `remote_mount_manual`) exercises a loopback RemoteMountServer/Manager pair so engineers can validate read + wait flows before the transport lands.

## Phase 0 Export Implementation Snapshot (December 5, 2025)
- **Server entry point:** `src/pathspace/distributed/RemoteMountServer.{hpp,cpp}` surfaces a shared-export manager that validates `MountOpenRequest` payloads, grants interpreter-only capabilities (`read`, `wait`), and issues session leases/heartbeat deadlines. `tests/unit/distributed/test_RemoteMountServer.cpp` covers the happy paths for read + wait notifications.
- **Serialization:** Reads return typed payloads encoded as base64 `SlidingBuffer` snapshots (`value.encoding="typed/slidingbuffer"`, `value.type_name` mirrors the runtime type, and `schema_hint` is reserved for future identifiers). Child listings are returned when `include_children=true`, enabling future tree walks without full snapshots.
- **Wait/notify bridge:** The server wraps each export’s `PathSpace` context sink to observe `notify()` calls. `WaitSubscribeRequest` entries enqueue notifications only when versions advance (per-path counters); watch queues keep pending `Notification` frames until the transport drains them.
- **Metrics + diagnostics:** `/inspector/metrics/remotes/<alias>/server/{sessions,waiters,status}` tracks active sessions, total leases, last subjects, and waiter depth. `/diagnostics/web/inspector/acl/<alias>/events/<ts>` captures auth failures and accepted mounts (subject, audience, code) so tooling can flag repeated denials.

See `docs/AI_Todo.task` for the acceptance criteria and step breakdowns tied to these backlog entries.

## RemoteMountManager Implementation Snapshot (December 5, 2025)
- **Client entry point:** `src/pathspace/distributed/RemoteMountManager.{hpp,cpp}` introduces the Phase 0 client plus the pluggable `RemoteMountSession`/`RemoteMountSessionFactory` interfaces. Each configured mount builds a dedicated `RemoteMountSpace` that is inserted under `/remote/<alias>` so the rest of PathSpace can treat the remote export like a nested trie.
- **Mirror paths:** RemoteMountManager now supports alias-aware mirror definitions inside `RemoteMountClientOptions`. Diagnostics tails replicate into `/diagnostics/errors/live/remotes/<alias>/…`, `/inspector/metrics/remotes/<alias>/server/*` is copied into the local metrics tree, and additional snapshot roots (e.g., `/renderers/<rid>/targets/html`) can be mirrored with depth/child caps so ServeHtml and inspector consumers see remote health and HTML output without screen-scraping.
- **Read parity:** Non-blocking reads issue `ReadRequest` frames, decode the typed `SlidingBuffer` payloads via the registered `InputMetadata`, and deserialize them into the caller-provided type so `/remote/<alias>/…` mirrors `/apps/...` contents byte-for-byte. Blocking reads automatically register `WaitSubscribeRequest` frames; `RemoteMountSpace` wakes local waiters once a `Notification` arrives and propagates the returned value back through the standard `Out & Block{}` flow.
- **Lease + heartbeat management:** The manager spins a heartbeat thread per mount, tracks lease expirations, increments heartbeat sequences, and drops sessions when the transport fails. Client metrics (`client/connected`, latency aggregates, waiter depth, request counters) publish under `/inspector/metrics/remotes/<alias>/client/*` so dashboards and the web adapter can diagnose connectivity from the consumer side.
- **Tests:** `tests/unit/distributed/test_RemoteMountManager.cpp` wires the manager to a loopback `RemoteMountServer`, verifying that simple reads return remote data and that `Block{}` reads unblock as soon as remote inserts arrive. The test factory exercises the same `RemoteMountSession` abstraction the production transports will use.
- **Tests:** `tests/unit/distributed/test_RemoteMountManager.cpp` wires the manager to a loopback `RemoteMountServer`, verifying that simple reads return remote data and that `Block{}` reads unblock as soon as remote inserts arrive. The test factory exercises the same `RemoteMountSession` abstraction the production transports will use, and the December 6 refresh asserts that typed payloads surface the expected `type_name` metadata across reads, takes, and notifications.

### Phase 1 TODO Snapshot (December 5, 2025)
- ✅ **Insert/Take and batch parity:** RemoteMountServer/Manager now stream batched queue drains via `TakeRequest.max_items` + `TakeResponse.values[]`, mounts expose a `take_batch_size` knob so `/remote/<alias>/ops/*` drains amortize network latency, and execution payloads that resolve to `std::string`, booleans, numeric scalars, any registered type, or true `void` are materialized locally and forwarded to the remote export. Void callables now surface as `void/sentinel` inserts so queues that intentionally emit no payload can drain without tripping the prior `InvalidType` guard.
- ✅ (December 6, 2025) **Diagnostics mirroring + snapshot streaming:** RemoteMountManager now includes configurable mirror paths that copy `/diagnostics/errors/live` into `/diagnostics/errors/live/remotes/<alias>`, replicate `/inspector/metrics/remotes/<alias>/server/*` into the local metrics space, and keep configured snapshot roots (e.g., `/renderers/*/targets/html`) updated so inspector views and ServeHtml consumers no longer need to scrape remote logs. Mirrors are append-only for diagnostics and breadth-first snapshots for metrics/surfaces, with alias-aware defaults that can be customized per mount.
- ✅ (December 6, 2025) **Streaming/backpressure polish:** RemoteMountServer now maintains per-session notification streams with queue depth metrics, throttling thresholds, and `notify_backpressure` retry-after hints; RemoteMountManager consumes the stream via a dispatcher thread, surfaces pending/dropped counts under `/inspector/metrics/remotes/<alias>/client/notifications/*`, and routes waiter wake-ups without per-wait polling so heartbeats stay free even when hundreds of waits are registered.
- ✅ (December 6, 2025) **Config + TLS transport:** RemoteMountTlsServer now exposes RemoteMountServer exports over TLS 1.3 with mutual-auth certificates, and RemoteMountTlsSessionFactory dials mounts via length-prefixed RemoteMountProtocol frames. TLS handshakes automatically populate `AuthContext.subject/fingerprint`, diagnostics log structured JSON `{code,message,subject,audience,fingerprint}`, and metrics add `status/last_fingerprint`. Operators point the server at `server.crt/server.key/ca.crt` while clients pass `RemoteMountTlsClientConfig` (CA, cert, key, SNI) via `RemoteMountClientOptions::tls` to keep fingerprints in sync with actual certificates.

## TLS Transport Snapshot (December 6, 2025)
- **Server listener:** `RemoteMountTlsServer` binds to a TLS 1.3 endpoint, wraps `RemoteMountServer`, and enforces mutual-auth certificates via `RemoteMountTlsServerConfig` (bind address, port, server cert/key, CA bundle, client-cert requirement, concurrency ceiling). Each accepted socket exchanges a single length-prefixed RemoteMountProtocol frame (MountOpen/Read/Insert/Take/Wait/NotificationStream/Heartbeat) and returns standard Error frames on failure.
- **Client factory:** `RemoteMountTlsSessionFactory` converts `RemoteMountClientOptions` entries into TLS-backed `RemoteMountSession`s. `RemoteMountTlsClientConfig` carries the CA path, client cert/key, SNI host, and connect timeout so mounts can dial remote exports securely. Sessions automatically derive `AuthContext.subject/fingerprint/proof` from the presented certificate before issuing `MountOpenRequest`s, keeping diagnostics and metrics aligned with the actual TLS identity.
- **Diagnostics + metrics:** `/diagnostics/web/inspector/acl/<alias>/events/*` now stores JSON `{code,message,subject,audience,fingerprint,proof}` entries instead of ad‑hoc strings, and `/inspector/metrics/remotes/<alias>/status/last_fingerprint` surfaces the most recent fingerprint so dashboards can confirm which certificate authenticated the mount.

> **Update (December 5, 2025):** RemoteMountProtocol now carries `InsertRequest`/`InsertResponse` and `TakeRequest`/`TakeResponse`, and the server/manager forward `std::string` inserts and takes (covering the ops queue + ServeHtml bridge). Inserts now accept serialized NodeData snapshots so arbitrary value types mirror local semantics; takes are still limited to UTF-8 strings while we design the type-agnostic pop serializer, and task forwarding remains TODO before closing the backlog item.

> **Update (December 5, 2025 — later):** `PathSpace::takeSerializedNodeData` plus the RemoteMountServer/Manager plumbing now stream NodeData snapshots for typed takes, so `/remote/<alias>/counter` can pop native integers/floats with the same queue semantics as local paths. Batched drains and task forwarding are still pending before the backlog item closes, but ServeHtml + ops queues can now rely on non-string payloads end-to-end.
> **Update (December 6, 2025 — typed payload removal):** `ValuePayload` now requires `type_name` and defaults to `encoding="typed/slidingbuffer"`, so distributed reads/takes insert typed buffers (produced via `InputMetadata::serialize`) and no longer leak NodeData layouts outside of PathSpace. Legacy `string/base64` frames remain behind the `PATHSPACE_REMOTE_TYPED_PAYLOADS=0` escape hatch for staggered deploys, while `nodedata/base64` is no longer accepted anywhere in the transport.

> **Update (December 5, 2025 — night):** `TakeRequest` gained `max_items` (capped at 64) and `TakeResponse` emits `values[]`, enabling RemoteMountManager to prefetch queue entries with a per-mount `take_batch_size` (default 4). Batched drains for `/remote/<alias>/ops/*` are now handled locally from the cache; the remaining gap in this item is forwarding execution/task payloads instead of rejecting them at insert time.

> **Update (December 5, 2025 — late night):** RemoteMountManager now executes Execution inserts whose return type is `std::string` locally and forwards the resulting value via the existing string payload path. This unblocks ServeHtml/op queues that relied on string-returning helpers, while non-string execution payloads still return `InvalidType` until the remote executor design lands.

> **Update (December 6, 2025):** RemoteMountManager now materializes Execution inserts with boolean and numeric (32/64-bit signed/unsigned, float/double) return types, converting the callable result into serialized NodeData before forwarding it to the remote export. Remaining executor work is limited to void executions and bespoke payloads that cannot be represented as scalars.

> **Update (December 6, 2025 — afternoon):** Custom execution result types can now register encoders via `distributed/RemoteExecutionRegistry.hpp`. RemoteMountManager consults the registry before rejecting an execution insert, so structured payloads (e.g., vectors, app-specific structs) flow over the existing NodeData path without changing the transport. The only executor gap left in this item is bridging true `void` callables.

> **Update (December 6, 2025 — evening):** RemoteMountManager executes `void` callables locally and forwards a `void/sentinel` payload while RemoteMountServer treats those inserts as acknowledgements (no stored value, `tasks_inserted = 1`). `/remote/<alias>` queues that emit no payload now drain successfully and the Insert/Take parity backlog item is officially closed.

> **Update (December 6, 2025 — night):** Per-session notification streaming is live. RemoteMountServer batches notifications into session queues, publishes `/server/notifications/{pending,throttled,retry_after_ms}` metrics, and rejects new waits with a `notify_backpressure` retry hint whenever backlog exceeds the throttle watermarks. RemoteMountManager now owns a dispatcher thread that drains the stream in batches, delivers wake-ups to local waiters without polling, publishes `/client/notifications/{pending,dropped}` metrics, and propagates server retry hints so ServeHtml/inspector workloads stop starving heartbeats when large wait sets accumulate.

> **Update (December 6, 2025 — late night):** The Phase 2 throttling mount wrapper is live. `RemoteMountExportOptions::throttle` now accepts `RemoteMountThrottleOptions` (per-export token bucket + waiter caps), RemoteMountServer spaces calls through the adapter, and metrics land under `/inspector/metrics/remotes/<alias>/server/throttle/*` (`hits_total`, `last_sleep_ms`, `waiters_rejected`, `retry_after_ms`). When a session exceeds its waiter cap the server replies with `error.code == "too_many_waiters"` and the configured `retry_after_ms`, giving RemoteMountManager and ServeHtml deterministic backoff signals without tearing down the session.
