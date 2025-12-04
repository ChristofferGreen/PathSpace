# ServeHtml Module Guide

> **Updated:** December 4, 2025 — documents the modular layout that replaced the
> old 3,900+ LOC `ServeHtmlServer.cpp` monolith so contributors know where each
> responsibility now lives.

## Overview
`pathspace_serve_html` is now a set of composable components under
`include/pathspace/web/` and `src/pathspace/web/serve_html/`. This guide explains
what each module owns, how to extend it, and which tests cover the surface. Use
it alongside `docs/finished/Plan_ServeHtml_Modularization_Finished.md` whenever
adding features or debugging the server.

## Directory Layout
- **Config & identifiers** — `ServeHtmlOptions.{hpp,cpp}` and
  `ServeHtmlIdentifier.hpp` parse CLI/env overrides, normalize identifiers, and
  publish option defaults. Doctests live in
  `tests/unit/web/test_ServeHtmlOptions.cpp`.
- **Auth/session** — `serve_html/auth/{SessionStore,Credentials,OAuthGoogle}.*`
  own session store interfaces, the in-memory + PathSpace backends, bcrypt
  helpers, Google Sign-In PKCE/JWKS verification, and demo credential seeding.
  Unit coverage: `tests/unit/web/test_ServeHtmlSessionStore.cpp`.
- **Routing & controllers** — `serve_html/routing/{HttpHelpers,AuthController,
  HtmlController,OpsController}.hpp` plus the matching `.cpp` (where needed)
  register HTTP handlers, enforce middleware (rate limits, sessions, logging),
  and build responses with helpers from `serve_html/{HtmlPayload,Routes}.hpp` and
  `serve_html/AssetPath.{hpp,cpp}`.
- **Streaming** — `serve_html/streaming/SseBroadcaster.*` implements event
  sessions, keep-alives, and Last-Event-ID resume logic. Tested by
  `tests/unit/web/test_ServeHtmlSseBroadcaster.cpp`.
- **Metrics & time helpers** — `serve_html/Metrics.*` and
  `serve_html/TimeUtils.*` expose Prometheus rendering, JSON snapshots, and time
  conversion helpers. `/metrics` and the background publisher both use these.
- **Demo scaffolding** — `serve_html/DemoSeed.*` and `ServeHtmlServer.cpp`
  provide optional demo app seeding plus the thin bootstrap that wires every
  module together.

## Where to Add Features
- **New CLI/config knobs** — extend `ServeHtmlOptions` (parsing + validation) and
  document the environment override in `docs/web_server.toml`.
- **Session or auth changes** — add methods to `SessionStore` if both backends
  need the behavior, or implement a new backend in `serve_html/auth`. Keep
  bcrypt/account helpers inside `Credentials.*`; controllers should consume the
  interfaces, not raw bcrypt utilities.
- **Routes & controllers** — introduce a controller under
  `serve_html/routing/` (header + optional `.cpp`). Register it from
  `ServeHtmlServer.cpp` and reuse `HttpRequestContext`. Avoid embedding PathSpace
  headers directly in controllers—inject the abstractions via `PathSpaceUtils`.
- **Streaming or future transports** — extend `SseBroadcaster` or create a new
  broadcaster under `serve_html/streaming/`, then have the server wire both to a
  shared producer interface so tests can exercise them independently.
- **Metrics or diagnostics** — add counters to `MetricsCollector` and surface the
  JSON + Prometheus strings via the existing helpers so `/metrics` and the
  background publisher stay in sync.

## Testing Checklist
- **Unit tests** — keep adding doctests alongside each module
  (`tests/unit/web/test_*.cpp`). Prefer deterministic tests that exercise
  controllers/helpers without binding sockets; `HttpHelpers` exposes hooks for
  this.
- **Integration tests** — `tests/tools/test_pathspace_serve_html_{http,sse,
  google}.py` drive the compiled binary. Run them via `ctest -R
  PathSpaceServeHtml(Http|Sse|Google)` before shipping routing/auth changes.
- **Golden transcripts** — guardrails recorded in `tests/data/serve_html/` must
  remain stable. Update them intentionally if response formats change and note
  the reason in `docs/Memory.md`.

## Remaining TODOs
These follow-ups depend on the modular split and now live in
`docs/AI_Todo.task`:
- **Multi-instance serving + distributed session stores** — wire the
  `SessionStore` interface into a durable backing service and document the
  clustering story for `/apps/*/events` and `/metrics`.
- **Inspector proxy & embedded tools** — expose the inspector SPA/controllers
  behind optional routes so operators can debug PathSpace without launching a
  sidecar.
- **Request logging/auditing** — add structured request logs under
  `/diagnostics/web/security/*` using the centralized metrics helpers.

Refer to `docs/finished/Plan_ServeHtml_Modularization_Finished.md` for the full
migration history and rationale for each module boundary.

