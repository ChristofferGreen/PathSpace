# Scene Graph Implementation Plan

> Completed milestones are archived in `docs/Plan_SceneGraph_Implementation_Finished.md` (snapshot as of October 29, 2025).
> Focus update (October 29, 2025): Widget focus dirty hints now inflate via `Widgets::Input::FocusHighlightPadding()`, and UITests assert highlight edge coverage so lingering rings surface immediately. Before moving to the font/resource manager rollout, add coverage for the slider → listbox focus regression so the bug is reproducible in CI.

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
- ✅ (October 28, 2025) Wrapped every widget preview in `examples/widgets_example.cpp` with horizontal/vertical stack containers so the gallery reflows as the window resizes; reused the layout helpers landed in the stack container milestone (see archive doc for context).
- ✅ (October 29, 2025) Hardened slider focus handoffs by storing default slider footprints at creation time and extending `PathSpaceUITests` with `Widget focus slider-to-list transition marks previous footprint without slider binding`. Slider → list transitions now queue dirty hints for both widgets even before bindings are attached, keeping the newly added highlight coverage test and the existing framebuffer diff case green.
- ✅ (October 29, 2025) Built `examples/widgets_example_minimal.cpp`, a pared-down demo with slider/list/tree widgets plus keyboard focus navigation that skips diagnostics, trace capture, and screenshot plumbing to spotlight the ergonomic GUI builder surface.
- Long-term follow-up: evaluate API ergonomics and refactor the minimal example toward a ≤400 LOC target once higher-level layout/focus helpers arrive.
- Close the font/resource plan from `docs/Plan_SceneGraph_Renderer.md` §"Plan: Resource system (images, fonts, shaders)" and surface the resulting font manager in the widget gallery (theme selection plus label typography should draw from the new resource-backed fonts).
- Enhance `examples/paint_example.cpp` with palette buttons (red, green, blue, yellow, purple, orange, etc.) and a brush-size slider routed through the widget bindings so live demos can tweak stroke color and width without code changes.
- Add widget action callbacks: allow attaching callable/lambda payloads to widget paths so pressing a button can immediately invoke application logic (hook into reducers/ops schema without bespoke polling).
- Extend `examples/widgets_example.cpp` with a demo button wired to a lambda that prints “Hello from PathSpace!” (or similar) using the new callback plumbing to validate the API.

### Detailed Plan — Font & Resource Manager Integration

**Goal**  
Ship the resource-backed font pipeline described in `docs/Plan_SceneGraph_Renderer.md` so TypographyStyle consumers (widgets, labels, HTML adapter) resolve fonts through PathSpace resources instead of hard-coded bitmap glyphs. The widget gallery should render with fonts supplied by the new manager, and renderers must receive resource fingerprints for atlas reuse.

**Prerequisites**
- Confirm HarfBuzz/ICU build availability on macOS CI; add third-party pulls or stubs if licensing review blocks immediate integration.
- Capture the current ASCII glyph builder footprint (TextBuilder) to preserve fallback behaviour during rollout.
- Align schema with `Plan_SceneGraph_Renderer.md` §"Plan: Resource system (images, fonts, shaders)" and §"Decision: Text shaping, bidi, and font fallback".

**Status Update (October 29, 2025)**  
- Landed `App::resolve_resource` and font resource helpers/tests to codify `resources/fonts/<family>/<style>/` layout.  
- Introduced a scaffolding `FontManager` wrapper that registers fonts and persists metadata (`meta/family`, `meta/style`, `manifest.json`, `active`).  
- Extended `TypographyStyle` and `TextBuilder::BuildResult` with font descriptors so fallback rendering keeps working while exposing style metadata.

**Phase 0 – Schema & Storage (1–2 days)**
- Define app-root resource layout under `resources/fonts/<family>/<style>/` with `manifest.json`, `builds/<revision>/atlas.bin`, and `active` pointer.
- Extend typed helpers: `App::ResolveResourcePath`, `UI::Builders::Fonts::{FontResourcePath, RegisterFont}`.
- Document schema in `docs/AI_PATHS.md` and update `docs/Plan_SceneGraph_Renderer.md` cross-links.

**Phase 1 – Font Manager Foundations (2–3 days)**
- Introduce `SP::UI::FontManager` (singleton scoped to PathSpace UI context) handling:
  - Logical font descriptors (`family`, `weight`, `style`, `features`, `lang`, `direction`).
  - Resource lookup + fallback chain resolution via manifests.
  - HarfBuzz shaping wrapper producing glyph indices/positions.
- Persist shaped run cache keyed by `(text, descriptor digest)` with eviction tied to atlas residency budgets.
- Emit metrics/diagnostics under `diagnostics/metrics/fonts/*` (cache hit rate, atlas pages, eviction counts).

**Phase 2 – Atlas Generation & Publication (3 days)**
- Build atlas generator writing RGBA or signed distance fields into `builds/<revision>/atlas.bin` with metadata for glyph UVs.
- Update snapshot builder to record atlas fingerprints alongside drawables (`DrawableBucket::font_assets`).
- Extend renderer target wiring to prefetch required atlas revisions; ensure `PathRenderer2D` uploads only when fingerprint changes.

**Phase 3 – Widget/Text Integration (2 days)**
- Replace `TextBuilder` bitmap glyphs with FontManager-backed shaping; maintain ASCII fallback by seeding a built-in resource pack.
- Update `Widgets::TypographyStyle` to carry `font_family`, `font_weight`, `font_features`, `language`, `direction`.
- Teach widget builders/examples to register required fonts on startup (gallery selects between default/sunset theme fonts).
- Add regression coverage in `tests/ui/test_TextBuilder.cpp` and widget gallery UITest to assert atlas fingerprints populate in scene revisions.

**Phase 4 – HTML Adapter & Diagnostics (1–2 days)**
- Wire HTML adapter to emit `@font-face` rules referencing `output/v1/html/assets/fonts/<fingerprint>.woff2`.
- Update diagnostics builders to surface active fonts, atlas residency, and shaping cache stats.
- Extend logging/test harnesses to capture font diagnostics during the 15× loop.

**Phase 5 – Rollout & Hardening (ongoing)**
- Run perf guardrail + renderer loop to establish baseline deltas (expect initial regression due to shaping; track metrics).
- Provide rollback flag (`PATHSPACE_UI_FONT_MANAGER_ENABLED`) allowing fallback to bitmap glyphs until confidence is high.
- Update docs (`AI_Debugging_Playbook.md`, `Widget_Contribution_Quickstart.md`, `Plan_SceneGraph_Renderer.md`) with new troubleshooting steps.

**Testing & Validation**
- Augment CTest suite with shaping-specific doctests (Latin kern pairs, Arabic joining, Devanagari reordering).
- Add golden framebuffer comparisons for widget gallery before/after FontManager swap.
- Ensure pre-push hook (`scripts/git-hooks/pre-push.local.sh`) exercises new tests and verifies atlas persistence.

**Risks & Mitigations**
- **Third-party deps**: If HarfBuzz integration blocks, start with FreeType-lite subset and gate advanced scripts behind feature flag.
- **Atlas size creep**: Implement LRU eviction and compression (PNG or basisu) for persisted atlases; document budget tuning knobs.
- **HTML/browser parity**: Validate fonts render identically in native and HTML adapters; capture mismatches in regression harness.

**Success Criteria**
- Widget gallery renders using resource-backed fonts with stable diffs.
- Renderers and HTML adapter consume atlas fingerprints without redundant uploads.
- Diagnostics surfaces atlas/cache metrics, and documentation reflects new workflow.

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
