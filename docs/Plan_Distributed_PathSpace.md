# Handoff Notice

> **Drafted:** October 20, 2025 — initial plan for distributed PathSpace mounts and networking.

# PathSpace — Distributed Mount Plan

## Goals
- Allow one PathSpace instance (“client”) to mount another remote instance (“server”) under a local subtree, similar to mounting a network filesystem.
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
- Capability/permission system (PathSpace “Capability” object) must expose `can_show`, `can_read`, `can_write`, `can_execute` helpers so the server can evaluate path access without duplicating ACL logic.
- Logging enhancements to differentiate local vs remote operations.

## Observability
- Expose metrics: RPC latency, error rates, reconnect counts, bytes transferred, pending notification queue size.
- Publish structured logs for mount lifecycle events (`MountOpen`, `MountClose`, disconnects, auth failures).
- Future enhancement: once PathSpace adds symlink support, mirror remote diagnostics under convenient local paths via symlinks so tooling/inspectors can read them without custom APIs.

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
- `docs/Plan_SceneGraph_Renderer.md` / `docs/Plan_WebServer_Adapter.md` — reference distributed mounts where relevant.
- `docs/AI_Debugging_Playbook.md` — troubleshooting steps for network mounts (connectivity, certificate issues).

## Open Questions
- **Conflict resolution:** Do we ever allow multiple writers? If so, what mechanism (last-writer-wins, versioning)?
- **Compression:** Default to gzip-compressed JSON frames (already part of the transport design); revisit if we need alternative codecs for large snapshot payloads.
- **Offline behavior:** How should clients behave if the mount drops mid-operation (retry vs fail-fast)?
- **Mount discovery:** Do we broadcast available exports or rely on explicit configuration?
- **Future multi-writer support:** When needed, evaluate CRDT/operation-transform approaches so multiple clients can edit shared subtrees and converge without central locking. Document design implications (including Capability interactions/permissions) before relaxing the single-writer assumption.

## Next Actions
1. Review and refine requirements with web/renderer stakeholders.
2. Finalize protocol schema and security requirements.
3. Add backlog items in `docs/AI_Todo.task` for Phase 0 implementation.
4. Update dependent plans (web server, HTML adapter) to reference distributed mounts.
