# Scene Graph Implementation Plan

> Completed milestones are archived in `docs/Plan_SceneGraph_Implementation_Finished.md` (snapshot as of October 29, 2025).
> Focus update (October 30, 2025): HarfBuzz shaping is live behind `FontManager::shape_text` with CoreText-backed font discovery, shaped-run cache capacity honors the atlas residency budgets stored under `meta/atlas/*`, and Phase 2 atlas persistence is online (default atlas writer, snapshot font-asset manifests, renderer prefetch). Next priority: Phase 3 widget/text integration to replace bitmap quads with shaped glyph draws and ensure atlas fingerprints flow end-to-end.

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
- ⚠️ (October 30, 2025) Resolve performance guardrail regression: pre-push run shows `path_renderer2d` incremental pass at 21.32 ms avg (baseline 7.57 ms, −85 FPS) and `pixel_noise_software` down to 123.7 FPS with render time 2.30 ms (baseline 0.74 ms) and present-call 8.09 ms (baseline 6.73 ms). Reproduce on an isolated run, identify renderer/presenter changes causing the slowdown, and restore metrics within tolerances before updating baselines.
- ✅ (October 28, 2025) Wrapped every widget preview in `examples/widgets_example.cpp` with horizontal/vertical stack containers so the gallery reflows as the window resizes; reused the layout helpers landed in the stack container milestone (see archive doc for context).
- ✅ (October 29, 2025) Hardened slider focus handoffs by storing default slider footprints at creation time and extending `PathSpaceUITests` with `Widget focus slider-to-list transition marks previous footprint without slider binding`. Slider → list transitions now queue dirty hints for both widgets even before bindings are attached, keeping the newly added highlight coverage test and the existing framebuffer diff case green.
- ✅ (October 29, 2025) Built `examples/widgets_example_minimal.cpp`, a pared-down demo with slider/list/tree widgets plus keyboard focus navigation that skips diagnostics, trace capture, and screenshot plumbing to spotlight the ergonomic GUI builder surface.
- Long-term follow-up: evaluate API ergonomics and refactor the minimal example toward a ≤400 LOC target once higher-level layout/focus helpers arrive.
- ✅ (October 29, 2025) Font/resource plan: widget demos now register fonts through `FontManager`, typography carries resource roots/revisions, and TextBuilder emits font fingerprints so renderers/presenters can reuse atlases.
- ✅ (October 29, 2025) Enhanced `examples/paint_example.cpp` with a widget-driven palette (red/green/blue/yellow/purple/orange) plus a brush-size slider so demos can swap colors and brush widths via bindings without editing code; PathSpace emits dirty hints for the control stack and persists the selected color/size under `/config`.
- ✅ (October 29, 2025) Extended PathSpaceUITests slider → listbox focus coverage to assert dirty hints for both widgets, locking in the regression before rolling into the font/resource manager milestone.
- ✅ (October 29, 2025) Add widget action callbacks: allow attaching callable/lambda payloads directly to widget bindings so button/toggle/list/tree dispatchers fire immediate callbacks in addition to queuing ops; UITests now cover the callback fan-out and clearing flows.
- ✅ (October 29, 2025) Extend `examples/widgets_example.cpp` with a demo button wired to the callback plumbing; the sample now logs “Hello from PathSpace!” on press/activate so contributors can see the immediate action flow.

### Detailed Plan — Font & Resource Manager Integration

**Goal**  
Ship the resource-backed font pipeline described in `docs/Plan_SceneGraph_Renderer.md` so TypographyStyle consumers (widgets, labels, HTML adapter) resolve fonts through PathSpace resources instead of hard-coded bitmap glyphs. The widget gallery should render with fonts supplied by the new manager, and renderers must receive resource fingerprints for atlas reuse.

**Prerequisites**
- Confirm HarfBuzz/ICU build availability on macOS CI; add third-party pulls or stubs if licensing review blocks immediate integration.
- Capture the current ASCII glyph builder footprint (TextBuilder) to preserve fallback behaviour during rollout.
- Align schema with `Plan_SceneGraph_Renderer.md` §"Plan: Resource system (images, fonts, shaders)" and §"Decision: Text shaping, bidi, and font fallback".

**Status Update (October 29, 2025)**  
- Landed `App::resolve_resource` and font resource helpers/tests to codify `resources/fonts/<family>/<style>/` layout.  
- Introduced a scaffolding `FontManager` wrapper that registers fonts and persists metadata (`meta/family`, `meta/style`, `meta/weight`, `meta/fallbacks`, `meta/active_revision`).  
- Extended `TypographyStyle` and `TextBuilder::BuildResult` with font descriptors so fallback rendering keeps working while exposing style metadata.  
- Widget gallery demos now register PathSpaceSans regular/semibold fonts via `FontManager`, propagate resource roots/revisions into `TypographyStyle`, and emit font fingerprints from `TextBuilder`; updated doctests cover the new metadata paths and active revision handling.
- Widget themes persist under `config/theme/<name>/value`; examples default to storing skylight/sunset palettes and `Widgets::LoadTheme` loads the active theme from PathSpace before applying defaults.
- FontManager now fingerprints typography descriptors, caches fallback-shaped runs with an LRU policy, and publishes cache/registration metrics under `diagnostics/metrics/fonts/*`; UITests cover registration, caching hits, and eviction behaviour.

**Phase 0 – Schema & Storage (1–2 days)**
- Define app-root resource layout under `resources/fonts/<family>/<style>/` with `meta/{family,style,weight,fallbacks,active_revision}`, `builds/<revision>/atlas.bin`, and optional per-revision metadata.
- Extend typed helpers: `App::ResolveResourcePath`, `UI::Builders::Fonts::{FontResourcePath, RegisterFont}`.
- Document schema in `docs/AI_PATHS.md` and update `docs/Plan_SceneGraph_Renderer.md` cross-links.

**Phase 1 – Font Manager Foundations (2–3 days)**
- ✅ (October 29, 2025) Landed the `SP::UI::FontManager` singleton with descriptor fingerprinting, fallback shaping, an LRU shaped-run cache, and diagnostics metrics under `diagnostics/metrics/fonts/*`. Current cache eviction is capacity-based; wire atlas-aware budgets once atlas persistence ships. HarfBuzz/ICU shaping remains stubbed out pending dependency review.
- ✅ (October 29, 2025) Resource lookup + fallback chain resolution via PathSpace metadata. `FontManager::resolve_font` reads `meta/{family,style,weight,fallbacks,active_revision}`, deduplicates fallback lists, surfaces the active atlas revision, and widgets/examples hydrate typography directly from those nodes with doctest coverage for success and defaulted metadata.
- ✅ (October 30, 2025) HarfBuzz shaping wrapper ships with CoreText-backed font discovery; shaped-run cache capacity now derives from stored atlas residency budgets (`meta/atlas/{softBytes,hardBytes,shapedRunApproxBytes}`) so eviction pressure tracks the intended atlas residency watermarks.

**Phase 2 – Atlas Generation & Publication (3 days)**
- ✅ (October 30, 2025) Build atlas generator writing Alpha8 atlas payloads into `builds/<revision>/atlas.bin` with glyph UV metadata plus per-revision `meta/atlas.json`; seeded default ASCII atlas so registered fonts persist usable glyph pages immediately.
- ✅ (October 30, 2025) Update snapshot builder to emit `DrawableBucket::font_assets` alongside drawables and persist them under `bucket/font-assets.bin`, keeping fingerprints, resource roots, and revisions latched per drawable.
- ✅ (October 30, 2025) Extend renderer target wiring to prefetch persisted atlases: `PathRenderer2D` now loads `atlas.bin` revisions via `FontAtlasCache`, publishes residency stats, and tracks font atlas fingerprints to avoid redundant uploads.

**Phase 3 – Widget/Text Integration (2 days)**
- Replace `TextBuilder` bitmap glyphs with FontManager-backed shaping; maintain ASCII fallback by seeding a built-in resource pack.
- Update `Widgets::TypographyStyle` to carry `font_family`, `font_weight`, `font_features`, `language`, `direction`.
- Teach widget builders/examples to register required fonts on startup (gallery selects between default/sunset theme fonts).
- Add regression coverage in `tests/ui/test_TextBuilder.cpp` and widget gallery UITest to assert atlas fingerprints populate in scene revisions.
- Introduce a dual-path atlas strategy for multi-color glyphs (emoji, COLR/CPAL fonts): persist RGBA atlas pages alongside the existing Alpha8 SDF lane, flag color glyphs in `font_assets`, and teach the renderer/text pipeline to bind them via the image/material path so emoji retain native chroma.

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
