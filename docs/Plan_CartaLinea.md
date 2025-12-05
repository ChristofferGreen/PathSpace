# Handoff Notice

> **Handoff note (October 19, 2025):** Carta Linea planning is paused during the assistant transition. Update this plan only after confirming scope with the new maintainer; see `docs/AI_Onboarding_Next.md` for current priorities.

# PathSpace — Carta Linea Plan

> **Context update (October 15, 2025):** Planning assumptions now align with the assistant context launched for this release; map any legacy terminology to the updated vocabulary.

Status: Active concept (MVP-focused)
Scope: Cross-app model for cards, timeline, and filesystem projections backed by paths
Audience: Product/UX, system architects, UI/rendering engineers

## Overview

Carta Linea unifies three views of the same object space and introduces a deck model where an application is a deck of its active cards (e.g., Photoshop as a deck of open documents) and users can also define explicit decks:
- Card View (task-oriented): what’s active, pinned, or snoozed now.
- Lifestream View (time-oriented): past, present, and scheduled future across all objects.
- Filesystem View (structure-oriented): the canonical folders-and-files hierarchy.

Every object has a canonical filesystem path for its content and a small amount of metadata (timestamps, lifecycle, tags, and optional relations). The three views are projections over the same objects; there is no duplication of content. See Foundations for how PathSpace (storage) and SceneGraph (UI) underpin the model.

## Goals

- One object, many views: cards and timeline derive from the same canonical path and metadata.
- Filesystem-first: content lives where it is; all features are additive and non-disruptive.
- Path-based integration: index and control surfaces exposed as paths (readable, scriptable).
- Atomicity: publish changes via atomic writes/renames; readers observe consistent snapshots.
- Incremental adoption: useful with zero app integration; apps can opt into richer hints over time.


Non-goals (MVP):
- Multi-device sync and conflict resolution.
- Encrypted metadata and access controls.
- Full-text content indexing.

## Core model (MVP)

Object (conceptual record):
- id: stable UUID (per object).
- kind: one of { file, directory, task } (extendable later).
- path: canonical path (for file/directory kinds); tasks may reside under a virtual root.
- timestamps:
  - created_at, updated_at (derived from filesystem or sidecar)
  - active_from (when the object became active)
  - due_at (optional)
  - completed_at (optional)
- lifecycle: one of { inactive, active, pinned, snoozed, completed, dismissed }
- tags: optional list of labels (hierarchical tags allowed, e.g., project/alpha).
- app_id: optional application affinity (e.g., com.vendor.app or internal id) used to build application decks
- stack_id: optional grouping key to render related cards as a stack.

Notes:
- The canonical path is authoritative for content. For virtual objects (e.g., tasks), a synthetic path under a virtual root is used.
- The metadata surface is minimal by design to stay compatible with existing workflows.

## Foundations: PathSpace storage and SceneGraph UI

PathSpace provides the storage substrate and path-based API used by Carta Linea. Storing indices and operations as paths makes the model scriptable, testable, and safe for multithreaded producers/consumers via write-then-rename semantics used across the project. Because storage is path-addressable, replication across processes or machines can be provided by underlying PathSpace mechanisms without changing Carta Linea’s data model or APIs.

Cards, Lifestream, and Filesystem are presented using the SceneGraph and Renderer. A card is a SceneGraph application/scene bound to an object id (or a stack) and reading from `index/builds/<revision>/*` selected via `index/current_revision`; the exact equivalence of “task” and “application” is intentionally flexible and may vary per app.

See docs/AI_Architecture.md and docs/finished/Plan_SceneGraph_Renderer_Finished.md for details. Scenes operate on app-relative paths inside the Carta app root and consume derived assets published under `index/builds/<revision>/...` via `index/current_revision`; they should not traverse arbitrary OS paths during rendering.

## Card forms: typed vs freeform (SceneGraph subtrees)

- A card’s visual and interactive structure is a sub-SceneGraph (“form”) bound to an object id.
- Forms come in two modes:
  - Freeform: a generic card usable for any object; shows title/path, preview (if available), and basic actions.
  - Typed: specific shapes for well-known kinds or content types (e.g., email, tweet, image/jpeg).

Selection (MVP — highest priority first):
1) Object override:
   - `/users/<user>/system/applications/carta/index/objects/<id>/form` contains an app-relative scene path (e.g., `scenes/card_forms/image`). If present, use it.
2) Content type:
   - If a MIME-type hint exists (sidecar or sniff), resolve via `index/forms/selectors/by_mime/<mime>` → `<form-id>`.
3) Kind default:
   - Resolve via `index/forms/selectors/by_kind/<kind>` → `<form-id>`.
4) Fallback:
   - Use `freeform`.

Path layout (under Carta app root):
- `scenes/card_forms/<form-id>/`
  - `src/...`, `builds/<revision>/...`, `current_revision` (see docs/AI_Paths.md)
- `index/forms/selectors/`
  - `by_mime/<mime>` — text file containing app-relative path, e.g. `scenes/card_forms/image`
  - `by_kind/<kind>` — text file containing app-relative path, e.g. `scenes/card_forms/task`
- `index/objects/<id>/form` — optional per-object override (app-relative scene path)

SceneGraph contract (v1):
- Inputs: object id; card scene reads `index/builds/<revision>/objects/<id>/*` and uses derived assets under `index/builds/<revision>/objects/<id>/derived/*`. Scenes must read only app-relative paths under the Carta app root and must not open arbitrary filesystem paths at render time.
- Standard slots: header, body, footer. Typed forms fill these consistently; freeform fills minimally.
- Output: drawable subtree authored under the form’s scene; rendering/publishing follows the snapshot rules in docs/finished/Plan_SceneGraph_Renderer_Finished.md.

Examples (non-normative):
- email: header(from, subject, time), body preview, actions (reply/archive).
- tweet: author, handle, text, media thumbnails, action badges.
- image/jpeg: image fit-to-card, metadata strip, open action.

## Decks (applications and explicit)

Concept:
- A deck is an ordered container of cards.
- Application deck: a runtime/application-defined deck (e.g., “Photoshop” deck contains all open document cards).
- Explicit deck: a user-defined deck (e.g., “Today”, “Reading”, “Sprint 42”).

Path layout (under Carta app root):
- `index/decks/<deck-id>/`
  - `meta/type` — `application` or `explicit`
  - `meta/title` — human-readable title
- `index/builds/<revision>/decks/<deck-id>/`
  - `cards/<order>-<object-id>` — zero-padded order entries (explicit decks only)
- Membership hints:
  - `index/objects/<id>/deck/<deck-id>` — presence indicates membership (for explicit decks)
- Application decks (derived):
  - `index/builds/<revision>/decks/app/<app-id>/`
    - `cards/<rev_epoch_ms>-<object-id>` — derived recency order (newer first; apps may later supply explicit order)

Operations (MVP):
- Add to explicit deck:
  - Drop file to `ops/deck_add/inbox/` with body: `<deck-id> <object-id>\n`
- Remove from explicit deck:
  - Drop file to `ops/deck_remove/inbox/` with body: `<deck-id> <object-id>\n`
- Reorder within an explicit deck:
  - Drop file to `ops/deck_move/inbox/` with body: `<deck-id> <object-id> <new_zero_padded_order>\n`

SceneGraph contract:
- `scenes/deck` renders a deck by reading `index/builds/<revision>/decks/<deck-id>/cards/*` selected via `index/current_revision` and composes child card forms (by id) using the selection rules above.
- An application’s primary view can be a deck scene; e.g., a Twitter deck rendering tweet cards, or a Photoshop deck rendering document cards.

## Path layout

System-owned and user-owned application roots (see docs/AI_Paths.md):
- `/system/applications/<app>`
- `/users/<user>/system/applications/<app>`

Carta Linea uses a user-owned application root:
- `/users/<user>/system/applications/carta/`

Within this root (MVP):
- `index/` — snapshot builds of read models for the projections
  - `builds/<revision>/`
    - `objects/<id>/path` — canonical path to content (text)
    - `objects/<id>/kind` — kind (text)
    - `objects/<id>/lifecycle` — lifecycle (text)
    - `objects/<id>/app_id` — application affinity (text), used to populate application decks
    - `objects/<id>/timestamps/{created_at,updated_at,active_from,due_at,completed_at}` (text ISO-8601; files optional if unset)
    - `objects/<id>/derived/thumbnail` — renderer-friendly preview asset (immutable snapshot)
    - `objects/<id>/derived/text_preview` — small text extract when applicable
    - `objects/<id>/derived/meta` — normalized metadata blob for forms
    - `cards/active/<rev_epoch_ms>-<id>` — presence indicates active; recency key is (9999999999999 - epoch_ms) to sort newest first lexicographically
    - `cards/pinned/<order>-<id>` — zero-padded order index; lower sorts earlier (explicit pin order)
    - `cards/snoozed/<wake>-<id>` — wake time key (e.g., `2025-09-30T09:00:00Z`)
    - `stream/<YYYY>/<MM>/<DD>/<time>-<id>` — timeline key files for that day (time-ordered)
    - `tags/<tag-path>/<id>` — membership in a tag (e.g., `tags/project/alpha/<id>`)
  - `current_revision` — single-value pointer to latest published index build
- Per-object derived snapshots (optional producer contract):
  - `index/objects/<id>/derived/builds/<revision>/...`
  - `index/objects/<id>/derived/current_revision`
- `ops/` — execution queues (append via file-drop into inbox directories)
  - `activate/inbox/` — create a file with body: `<id>\n`
  - `complete/inbox/` — create a file with body: `<id>\n`
  - `dismiss/inbox/` — create a file with body: `<id>\n`
  - `pin/inbox/` / `unpin/inbox/` — create a file with body: `<id>\n`
  - `snooze/inbox/` — create a file with body: `<id> <iso8601>\n`
  - `stack/inbox/` — create a file with body: `<id> <stack_id>\n`
  - `unstack/inbox/` — create a file with body: `<id>\n`
  - `tag_add/inbox/` / `tag_remove/inbox/` — create a file with body: `<id> <tag>\n`
- `log/` — newline-delimited operational log (append-only for audit/troubleshooting)
  - `events` — human-readable summaries of operations applied
  - `errors` — errors/warnings emitted by the indexer

Sidecars at the content location (optional but recommended):
- `.carta/id` — object UUID (text)
- `.carta/meta.json` — optional author hints (display title, preferred icon, stable tags)

Virtual root for task-like objects (if used):
- `/users/<user>/system/applications/carta/virtual/tasks/<id>/` — placeholder path for tasks

## Indexer responsibilities (MVP)

A single user-space indexer process maintains the `index/` subtree:
- Discover objects:
  - For files/directories: within user-selected roots (config TBD), mint or read `.carta/id`.
  - For tasks: under the virtual root, the id is the directory name.
- Maintain object records:
  - Track canonical `path`, `kind`, `timestamps` (fs stat + sidecar overrides).
  - Update `lifecycle` per operations or inferred activity (e.g., activate on open if bound).
- Maintain projections (materialized files):
  - Cards: update `cards/active`, `cards/pinned`, `cards/snoozed` using atomic file creates/removes and ordered names.
  - Lifestream: create timeline entries under `stream/YYYY/MM/DD/...` keyed by a primary time (see Timeline policy).
  - Tags: maintain presence under `tags/<tag-path>/<id>`.
- Atomicity:
  - Write to a temporary file and rename into place.
  - Update fan-out structures in a consistent order; on crash, re-scan to reconcile.
- Observability:
  - Append a concise line to `log/events` per applied operation.
  - Write errors to `log/errors`.

## Timeline policy (MVP)

Primary time key for ordering an object in the Lifestream:
1) `completed_at` if set
2) else `active_from` if set
3) else `updated_at`
4) else `created_at`

The key selection is stable per object update and recorded under the current index build (e.g., `index/builds/<revision>/objects/<id>/timeline_key`, selected via `index/current_revision`).

## Operations (user-visible semantics)

All operations are path-driven via `ops/<op>/inbox/` queues. Each file dropped into an inbox is applied once by the indexer; invalid payloads are logged to `log/errors`.

- Activate: drop a file into `ops/activate/inbox/` with body `<id>\n`
  - Sets `lifecycle=active`, `active_from=now`, updates cards and stream.
- Complete: drop a file into `ops/complete/inbox/` with body `<id>\n`
  - Sets `lifecycle=completed`, `completed_at=now`, updates stream, removes from active.
- Dismiss: drop a file into `ops/dismiss/inbox/` with body `<id>\n`
  - Sets `lifecycle=dismissed`, removes from active; remains visible in stream.
- Pin/Unpin: drop a file into `ops/pin/inbox/` or `ops/unpin/inbox/` with body `<id>\n`
  - Adds/removes from `cards/pinned` with a rank; rank policy: new pins get topmost rank.
- Snooze: drop a file into `ops/snooze/inbox/` with body `<id> <iso8601>\n`
  - Sets `lifecycle=snoozed`, hides from active until wake; presence under `cards/snoozed/<wake>-<id>`.
- Stack/Unstack: drop a file into `ops/stack/inbox/` with body `<id> <stack_id>\n` or into `ops/unstack/inbox/` with body `<id>\n`
  - Sets/clears `stack_id`; cards render as grouped.
- Tag add/remove: drop a file into `ops/tag_add/inbox/` or `ops/tag_remove/inbox/` with body `<id> <tag>\n`
  - Adds/removes membership under `tags/<tag>/<id>`.

## Integration

Zero-integration:
- Files and folders are indexed from the filesystem alone.
- Users can activate, pin, snooze, tag, and complete without app changes.

Optional sidecar hints:
- Apps can write `.carta/meta.json` with stable tags or display hints.
- If desired, apps can manage `.carta/id` on copy/duplicate workflows.

UI surfaces (built within the existing rendering system; see docs/finished/Plan_SceneGraph_Renderer_Finished.md):
- `scenes/cards` — renders `index/builds/<revision>/cards/*` selected via `index/current_revision` with stacking and pin order.
- `scenes/deck` — renders `index/builds/<revision>/decks/<deck-id>/cards/*` selected via `index/current_revision` as a deck (application or explicit) and composes child card forms.
- `scenes/lifestream` — renders `index/builds/<revision>/stream/*` selected via `index/current_revision` with date buckets.
- `scenes/files` — renders the canonical filesystem view with Carta badges.
Example wiring (abridged):
- `renderers/2d/targets/surfaces/cards/scene = "scenes/cards"`
- `surfaces/cards/renderer = "renderers/2d"`
- `surfaces/cards/scene = "scenes/cards"`
- `windows/MainWindow/views/cards/surface = "surfaces/cards"`

## Atomicity and concurrency

- All `index/` updates use write-then-rename; readers see consistent states.
- `ops/*` files are treated as append-only queues; the indexer consumes lines idempotently.
- On startup, the indexer reconciles `index/` against discovered content and sidecars.

## Failure modes

- Missing `.carta/id`: mint a new id and write sidecar; log the event.
- Duplicate ids (e.g., copied folder): detect when two paths claim the same id; mint a new id for the copy and fix the sidecar; log remediation.
- Moved/renamed content: track by id; update path in the next index build so readers see it under `index/builds/<revision>/objects/<id>/path`.
- Stale index entries: periodic reconciliation removes entries for missing content.

## MVP

- Kinds: file, directory, task (virtual).
- Lifecycle: inactive, active, pinned, snoozed, completed, dismissed.
- Minimal metadata: id, kind, path, timestamps, lifecycle, tags, stack_id.
- Paths:
  - User app root: `/users/<user>/system/applications/carta/`
  - `index/builds/<revision>/...` with `index/current_revision` (readers)
  - `ops/*`, `log/*`
  - Sidecars: `.carta/id`, optional `.carta/meta.json`
- Projections:
  - Cards: pinned, then active by recency; snoozed hidden until wake.
  - Lifestream: Today, Yesterday, This Week, Earlier, and Upcoming.
  - Filesystem: unmodified structure with Carta badges.
- Performance targets:
  - Operation to visible update: < 250 ms end-to-end.
  - UI readers perform read-only path traversal; no locks required.

## Open questions

- What scope of the filesystem is indexed by default (per-user roots, explicit opt-in, or both)?
- Should tasks always be virtual under Carta’s root, or optionally materialized as files?
- Rank policy for pinned items (explicit numeric vs. implicit recency) — pick one for MVP and keep files stable.
- Minimum set of tags supported in UI (free-form vs. curated namespaces).

## Cross-references

- See docs/AI_Paths.md for canonical path conventions and app roots.
- See docs/finished/Plan_SceneGraph_Renderer_Finished.md for scene/surface organization; Carta views should adopt the same atomic publication and snapshot patterns.
