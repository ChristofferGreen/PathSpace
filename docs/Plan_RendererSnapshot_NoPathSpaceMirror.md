# Plan: Renderer Snapshot Without PathSpace Trellis Mirror

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
1) Snapshot builder
   - Add a renderer-side snapshot structure (ECS-inspired, cache-friendly, versioned) for declarative widgets: SoA components for transform/layout/style/text/image, plus render commands.
   - Build/update snapshot on publish/commit; store it outside PathSpace. Use chunked/archetype packing to minimize cache misses.
2) Remove PathSpace mirror
   - Delete trellis/runtime writes that create `.../runtime/lifecycle/trellis/...` copies of the scene/widgets.
   - Remove runtime revision/authoring logs from PathSpace.
3) Presenter integration
   - Presenter consumes the renderer snapshot; no PathSpace traversal during render.
   - Ensure snapshots are immutable per frame; versioned to avoid races.
4) Publishing pipeline
   - Ensure authoring changes trigger snapshot rebuilds; add a small diff or full rebuild strategy.
5) Tests
   - Verify renders still succeed without trellis mirror.
   - PathSpace dumps shrink (no runtime subtree).
   - Snapshot correctness: compare rendered output/hash before/after change.
6) Docs
   - Update architecture docs to reflect snapshot-based renderer and removal of PathSpace runtime mirror.
7) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20`.

## Acceptance criteria
- PathSpace no longer contains trellis/runtime mirrors or renderer runtime data.
- Renderer reads from its own snapshot, not from PathSpace.
- Dumps show only authoring state; size drops accordingly.
- Rendering output and tests remain correct; full test loop green.
