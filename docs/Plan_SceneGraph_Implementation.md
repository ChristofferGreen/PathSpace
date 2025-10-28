# Scene Graph Implementation Plan

> Completed milestones are archived in `docs/Plan_SceneGraph_Implementation_Finished.md` (snapshot as of October 28, 2025).

## Context and Objectives
- Groundwork for implementing the renderer stack described in `docs/Plan_SceneGraph_Renderer.md` and the broader architecture contract in `docs/AI_Architecture.md`.
- Deliver an incremental path from today's codebase to the MVP (software renderer, CAMetalLayer-based presenters, surfaces, snapshot builder), while keeping room for GPU rendering backends (Metal/Vulkan) and HTML adapters outlined in the master plan.
- Maintain app-root atomic semantics, immutable snapshot guarantees, and observability expectations throughout the rollout.

## Success Criteria
- A new `src/pathspace/ui/` subsystem shipping the MVP feature set behind build flags.
- Repeatable end-to-end tests (including 15× loop runs) covering snapshot publish/adopt, render, and present flows.
- Documentation, metrics, and diagnostics that let maintainers debug renderer issues without spelunking through the code.

## Active Focus Areas
- Extend diagnostics and tooling: expand error/metrics coverage, normalize `PathSpaceError` reporting, add UI log capture scripts, and refresh the debugging playbook updates before the next hardening pass.
- Draft the Linux/Wayland bring-up checklist covering toolchain flags, presenter shims, input adapters, and CI coverage so non-macOS contributors understand UI stack gaps.
- Add deterministic golden outputs (framebuffers plus `DrawableBucket` metadata) with tolerance-aware comparisons to guard renderer and presenter regressions.
- Broaden stress coverage for concurrent edits versus renders, present policy edge cases, progressive tile churn, input-routing races, and error-path validation.
- Tighten CI gating to require the looped CTest run and lint/format checks before merges.

## Task Backlog
- Wrap every widget preview in `examples/widgets_example.cpp` with horizontal/vertical stack containers so the gallery reflows as the window resizes; reuse the layout helpers landed in the stack container milestone (see archive doc for context).
- Fix widget focus repaint regressions: ensure focus styling clears when focus leaves a widget so the highlight doesn’t linger (reproduce via gallery Tab cycling; current focus ring sticks half-on after blur).
- Close the font/resource plan from `docs/Plan_SceneGraph_Renderer.md` §"Plan: Resource system (images, fonts, shaders)" and surface the resulting font manager in the widget gallery (theme selection plus label typography should draw from the new resource-backed fonts).
- Enhance `examples/paint_example.cpp` with palette buttons (red, green, blue, yellow, purple, orange, etc.) and a brush-size slider routed through the widget bindings so live demos can tweak stroke color and width without code changes.
- Add widget action callbacks: allow attaching callable/lambda payloads to widget paths so pressing a button can immediately invoke application logic (hook into reducers/ops schema without bespoke polling).
- Extend `examples/widgets_example.cpp` with a demo button wired to a lambda that prints “Hello from PathSpace!” (or similar) using the new callback plumbing to validate the API.

## Open Questions
- Finalize the `DrawableBucket` binary schema (padding, endianness, checksum) before snapshot and renderer work diverges further.
- Decide on the minimum color management scope for the MVP (sRGB8 only versus optional linear FP targets).
- Clarify the resource manager's involvement for fonts and images in the MVP versus deferred phases.
- Validate CAMetalLayer drawable lifetime, IOSurface reuse, and runloop coordination during resize and fullscreen transitions for the Metal presenter.
- Nail down the metrics format (`output/v1/common` versus `diagnostics/metrics`) to keep profiler expectations stable.
- Define sequencing for path-traced lighting and tetrahedral acceleration work (post-MVP versus incremental alongside GPU backends).

## Documentation and Rollout Checklist
- Update `README.md` build instructions once UI flags ship.
- Keep `docs/Plan_SceneGraph_Renderer.md` cross-references aligned; link to this implementation plan from that doc and vice versa.
- Add developer onboarding snippets (how to run tests, inspect outputs) to `docs/`.
- Track milestone completion in `AI_Todo.task` or the equivalent planning artifact.

## References
- Completed implementation details: `docs/Plan_SceneGraph_Implementation_Finished.md`.
- Specification: `docs/Plan_SceneGraph_Renderer.md`.
- Core architecture overview: `docs/AI_Architecture.md`.

## Maintenance Considerations
- Ensure feature flags allow partial builds (e.g., disable the UI pipeline when unsupported).
- Monitor binary artifact sizes for snapshots; consider tooling to inspect revisions.
- Plan for future GPU backend work without blocking the MVP—keep interfaces abstract and avoid hard-coding software assumptions.
- Establish guardrails for progressive mode defaults to avoid regressing latency-sensitive apps.
- Respect residency policies and track resource lifecycle metrics so caches stay healthy.
