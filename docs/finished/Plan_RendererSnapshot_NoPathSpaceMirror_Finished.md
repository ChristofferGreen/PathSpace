# Plan: Renderer Snapshot Without PathSpace Trellis Mirror

> **Status (December 29, 2025):** Plan completed and archived under `docs/finished/`.

## Goal
Remove the renderer/trellis mirror from PathSpace and instead build renderer-owned, cache-friendly snapshots outside PathSpace. PathSpace should hold only authoring/shared state; renderer snapshots live in renderer memory and are not written back into PathSpace.

## Current issue
- Declarative trellis copies the widget tree into a runtime subtree under the scene (`.../runtime/lifecycle/trellis/...`) so the presenter can walk a stable view. This bloats PathSpace, shows up in dumps, and mixes runtime-only data with shared state.

## Target design
- Authoring/logic tree stays in PathSpace.
- Publish/commit step builds a renderer snapshot (SoA/AoS arrays, versioned) owned by the renderer, not stored in PathSpace.
- Presenter reads only the snapshot; no runtime/trellis nodes are written into PathSpace.
- PathSpace dumps show only authoring state (scene/window/widgets/theme), not renderer runtime data.
- Snapshot structure should follow an ECS-style layout: components in SoA, chunked/archetyped where helpful (e.g., transforms/layout/style/text), with versioning for coherence.

## ECS design notes
- Components: transform, layout metrics, style/material, text glyph runs, image refs, interaction flags.
- Archetypes/chunks: group common component sets (e.g., text widgets, solid-rect widgets) for tight SoA packing.
- Versioning/double-buffer: build snapshot per publish; presenter reads immutable snapshot for the frame; swap on next publish.
- ID mapping: stable entity IDs derived from PathSpace widget IDs; store a lookup table from PathSpace path → snapshot entity index.
- Coherency: no locks during render; snapshot is immutable for the frame.

## Work items
1) Snapshot builder — **DONE (Dec 27, 2025)**
   - RendererSnapshotStore now holds renderer-side snapshots in-process; SceneSnapshotBuilder no longer writes builds/* bucket binaries or manifests into PathSpace (only revision metadata/current_revision remain).
   - Follow-up: validate chunk/archetype packing and incremental rebuilds once the cache is profiled in the renderer loop.
2) Remove PathSpace mirror — **DONE (Dec 28, 2025)**
   - Trellis/runtime queues stay in PathSpace, but the lifecycle worker now consumes them via an in-memory PathSpaceTrellis instead of mounting under `runtime/lifecycle/trellis`.
   - Renderer buckets are cached in-memory only; no `/structure/widgets/.../render/bucket` mirrors remain in PathSpace.
3) Presenter integration — **DONE (Dec 28, 2025)**
   - Presenter reads from RendererSnapshotStore snapshots; renderers no longer traverse PathSpace runtime mirrors.
   - Snapshots stay immutable per frame; versioned via SceneSnapshotBuilder/RendererSnapshotStore.
4) Publishing pipeline — **DONE (Dec 29, 2025)**
   - Declarative lifecycle now watches `scenes/<sid>/diagnostics/dirty/state` and translates authoring dirty markers into renderer rebuilds: visual/layout/text edits trigger a cached widget diff pass, while structural dirty marks drop missing cached buckets and force a full rebuild of the scene snapshot.
   - Dirty handling no longer consumes queue entries, so external observers can still block on `diagnostics/dirty/queue` while the lifecycle performs its own rebuilds and republish.
5) Tests — **DONE (Dec 28, 2025)**
   - Full `./scripts/compile.sh --loop=5 --per-test-timeout 60` green.
   - PathSpace dumps shrink (no runtime subtree); renderer snapshots validated through UITests.
6) Docs — **DONE (Dec 28, 2025)**
   - AI_PATHS/Widget_Schema_Reference updated to describe renderer-owned snapshots and the absence of trellis/runtime mirrors in PathSpace.
7) Validation — **DONE (Dec 28, 2025)**
   - Loop logs: `build/test-logs/PathSpaceUITests_All_loop{1..5}_20251228-0020*.log`, manifest `build/test-logs/loop_manifest.tsv`.

## Acceptance criteria
- PathSpace no longer contains trellis/runtime mirrors or renderer runtime data.
- Renderer reads from its own snapshot, not from PathSpace.
- Dumps show only authoring state; size drops accordingly.
- Rendering output and tests remain correct; full test loop green.
