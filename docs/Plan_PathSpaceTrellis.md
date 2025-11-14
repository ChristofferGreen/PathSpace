# PathSpace Trellis — Redesign Notes

_Last updated: November 14, 2025 (skeleton draft)_

## Context
- The legacy trellis layer and its diagnostics were removed on November 14, 2025.
- Historical design choices, stress coverage, and serialization notes live in `docs/finished/Plan_PathSpace_FanIn_Abandoned.md` for archival reference.
- This document tracks the forward-looking redesign; expand sections as decisions land.

## Objectives (initial pass)
- Reintroduce a fan-in surface that multiplexes multiple concrete sources behind a single public path.
- Preserve PathSpace semantics (wait/notify, insert/read/take) without reintroducing mutex-heavy bookkeeping.
- Keep observability first-class: plan how descriptors, traces, and metrics will surface in the new model.

## Open Questions
1. **State ownership:** Should buffered state live inside the backing `PathSpace` (reusing context primitives) or move to a purpose-built component?
2. **Persistence contract:** Which trellis fields must survive restarts, and in what schema? Do we still need per-source counters?
3. **Runtime instrumentation:** What is the minimal telemetry we must ship in vNext to unblock tooling?
4. **Compatibility shims:** Do we require transitional adapters for existing consumers, or can the old APIs be retired immediately?

Document answers inline as they become clear; add subsections for approved solutions.

## Next Steps (proposed)
1. Draft a minimal architecture sketch covering state layout, notification flow, and insertion semantics.
2. Prototype a queue-only trellis to validate the execution path, then extend to latest-mode if still required.
3. Outline a migration plan for tests/tooling (doctest coverage, compile hooks, dashboards) so they align with the new descriptors.

## Status Tracking
- _2025-11-14_: Legacy trellis removed; redesign planning started here.
- Capture future milestones chronologically with date stamps and concise summaries.
