# Scene Graph Implementation Plan — Archived

> Archived December 10, 2025. All tracked work is complete; this document now lives under `docs/finished/` as the historical record for the scene graph rollout.
> Completed milestones are archived in `docs/finished/Plan_SceneGraph_Implementation_Finished.md` (snapshot as of October 29, 2025).
> Focus update (November 10, 2025): HarfBuzz shaping now flows through `TextBuilder` and the widget pipeline, glyph buckets persist atlas fingerprints, and `PathRenderer2D` renders shaped text when the pipeline flag is enabled. The shaped path currently lands behind the existing environment/debug toggles while the default remains `GlyphQuads` to keep interactive workloads responsive (Software2D pixel noise baseline now ~28 FPS / 35 ms present-call). Budgets and perf baselines have been updated under `docs/perf/*.json` to reflect the higher text cost while Phase 4 optimization tasks are queued.

## Context and Objectives
- Groundwork for implementing the renderer stack described in `docs/finished/Plan_SceneGraph_Renderer_Finished.md` and the broader architecture contract in `docs/AI_Architecture.md`.
- Deliver an incremental path from today's codebase to the MVP (software renderer, CAMetalLayer-based presenters, surfaces, snapshot builder), while keeping room for GPU rendering backends (Metal/Vulkan) and HTML adapters outlined in the master plan.
- Maintain app-root atomic semantics, immutable snapshot guarantees, and observability expectations throughout the rollout.

## Success Criteria
- A new `src/pathspace/ui/` subsystem shipping the MVP feature set behind build flags.
- Repeatable end-to-end tests (including 5× loop runs) covering snapshot publish/adopt, render, and present flows.
- Documentation, metrics, and diagnostics that let maintainers debug renderer issues without spelunking through the code.

## Active Focus Areas
- Extend diagnostics and tooling: expand error/metrics coverage, normalize `PathSpaceError` reporting, add UI log capture scripts, and refresh the debugging playbook updates before the next hardening pass.
- Add deterministic golden outputs (framebuffers plus `DrawableBucket` metadata) with tolerance-aware comparisons to guard renderer and presenter regressions.
- Broaden stress coverage for concurrent edits versus renders, present policy edge cases, progressive tile churn, input-routing races, and error-path validation.
- Tighten CI gating to require the looped CTest run and lint/format checks before merges.

## Task Backlog
- ✅ (November 8, 2025) Land single-line and multi-line text input widgets with cursor/selection scaffolding, dirty hint propagation, scene snapshots, and binding hooks. `Widgets::CreateTextField` / `CreateTextArea` now publish focus-aware state scenes, sanitize metadata, and expose bindings that queue `Text*` widget ops; renderer buckets draw selections/caret/composition lanes. Follow-up: finish wiring widget input (pointer/keyboard, scroll deltas, IME composition publishing) and extend the demos/tests to exercise the full editing loop.
- ✅ (December 9, 2025) Wrapped the paint example canvas/root in an `UndoableSpace`, routed declarative paint actions through the history layer, and updated the Undo/Redo buttons plus keyboard shortcuts (Cmd/Ctrl+Z / +Shift+Z) to drive `_history/undo` / `_history/redo`. Buttons, scripted strokes, GPU smoke, and keyboard shortcuts now all exercise the journal-backed history path end-to-end, publishing the existing telemetry and status labels.
- ✅ (November 14, 2025) Completed the undo/redo stack design review with PathSpace maintainers; documentation updated with final API/retention decisions (`docs/AI_Architecture.md`, `docs/finished/Plan_PathSpace_UndoJournal_Rewrite_Finished.md`).
- ✅ (December 9, 2025) Documented how the copy-on-write prototype and the current journal backend plug into `UndoableSpace`, covering stack plumbing, notification passthrough, guardrails, and widget migration guidance. New wiring notes live in `docs/History_Journal_Wiring.md`, and `docs/AI_Architecture.md` now points to the checklist so contributors have a single reference before touching renderer-side history code.
- 🚫 (November 14, 2025) PathSpace event route merger plan abandoned; widget routing stays in C++ lambdas with direct dispatch sequencing. No runtime changes landed.
- ✅ (December 9, 2025) Finalized the `HistoryOptions` defaults and history telemetry schema: `HistoryLimitMetrics` now surfaces max entries/bytes, RAM cache, keep-latest windows, disk budgets, and persistence flags via `_history/stats/limits/*`, and compaction counters mirror under `_history/stats/compaction/*` alongside the existing trim metrics. Inspector tooling and declarative widgets can now bind directly to the limits/compaction nodes without reverse engineering the struct layout.
- ✅ (December 9, 2025) Updated `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`, `docs/History_Journal_Wiring.md`, and this plan to document the finalized routing registry (`HandlerBinding` helpers) plus the shared history telemetry/`HistoryBinding` workflow so the declarative widget roadmap reflects the shipped APIs.
- ✅ (December 9, 2025) Extended renderer diagnostics: `PathSpaceError` writes now normalize severity codes, per-target `diagnostics/errors/stats` counters track total/cleared/errors by severity, and `scripts/ui_capture_logs.py` captures UI logs + metrics from `pathspace_dump_json` snapshots. `docs/AI_Debugging_Playbook.md` documents the workflow so responders can grab renderer histories without spelunking raw nodes.
- ✅ (December 9, 2025) Expanded `scripts/ui_capture_logs.py` to mirror the full `TargetMetrics` surface (present policy, progressive copy counters, encode contention data, residency budgets, and material descriptors) and refreshed `docs/AI_Debugging_Playbook.md` with the new JSON schema.
- ✅ (December 10, 2025) ServeHtml and inspector dashboards now expose the same UI diagnostics summary: `/diagnostics/ui` on the HTML server and `/inspector/metrics/ui_targets` on the inspector, with live JSON mirrors under `<apps_root>/io/metrics/web_server/serve_html/ui_targets` so remote sessions can read the richer metrics without running `ui_capture_logs.py` manually.
- ✅ (December 10, 2025) Drafted the Linux/Wayland bring-up checklist (toolchain flags, presenter shims, input adapters, CI coverage) in `docs/Wayland_Bringup_Checklist.md` so non-macOS contributors can follow a single playbook while the presenter lands.
- ✅ (November 14, 2025) Declarative widgets now publish through `include/pathspace/ui/declarative/Widgets.hpp` (fragments, `Widgets::Mount`, handler bindings, render descriptors). Follow-ups: wire the runtime watchers into the new handler registry and finish the physical `Widgets::Move` helper so reparenting doesn’t require recreating widgets.
- ✅ (December 10, 2025) Runtime widget bindings now resolve handler registry entries (button/toggle/slider/list/tree/input) and `Widgets::Move` preserves handlers during reparenting, so widgets keep callbacks without recreation.
- ✅ (November 15, 2025) Focus metadata now updates automatically: the controller computes depth-first `focus/order` values, toggles `widgets/<id>/focus/current`, and mirrors the active widget under `structure/window/<window-id>/focus/current`. Declarative UITests verify the metadata and the new dispatcher helper that consumes it, so keyboard/gamepad routing no longer maintains bespoke focus lists.
- ✅ (October 28, 2025) Wrapped every widget preview in `examples/widgets_example.cpp` with horizontal/vertical stack containers so the gallery reflows as the window resizes; reused the layout helpers landed in the stack container milestone (see archive doc for context).
- ✅ (October 29, 2025) Hardened slider focus handoffs by storing default slider footprints at creation time and extending `PathSpaceUITests` with `Widget focus slider-to-list transition marks previous footprint without slider binding`. Slider → list transitions now queue dirty hints for both widgets even before bindings are attached, keeping the newly added highlight coverage test and the existing framebuffer diff case green.
- ✅ (October 29, 2025) Built `examples/widgets_example_minimal.cpp`, a pared-down demo for declarative widgets. **Update (November 28, 2025):** the sample now mirrors the plan doc’s LaunchStandard → App::Create → Window::Create → Scene::Create → `Button`/`List` flow and runs via `SP::App::RunUI`, giving contributors a doc-aligned “hello widgets” reference without the extra helper plumbing.
- Long-term follow-up: evaluate API ergonomics and refactor the minimal example toward a ≤400 LOC target once higher-level layout/focus helpers arrive.
- ✅ (October 29, 2025) Font/resource plan: widget demos now register fonts through `FontManager`, typography carries resource roots/revisions, and TextBuilder emits font fingerprints so renderers/presenters can reuse atlases.
- ✅ (October 29, 2025) Enhanced `examples/paint_example.cpp` with a widget-driven palette (red/green/blue/yellow/purple/orange) plus a brush-size slider so demos can swap colors and brush widths via bindings without editing code; PathSpace emits dirty hints for the control stack and persists the selected color/size under `/config`.
- ✅ (October 29, 2025) Extended PathSpaceUITests slider → listbox focus coverage to assert dirty hints for both widgets, locking in the regression before rolling into the font/resource manager milestone.
- ✅ (October 29, 2025) Add widget action callbacks: allow attaching callable/lambda payloads directly to widget bindings so button/toggle/list/tree dispatchers fire immediate callbacks in addition to queuing ops; UITests now cover the callback fan-out and clearing flows.
- ✅ (October 29, 2025) Extend `examples/widgets_example.cpp` with a demo button wired to the callback plumbing; the sample now logs “Hello from PathSpace!” on press/activate so contributors can see the immediate action flow.

### Detailed Plan — Font & Resource Manager Integration

**Goal**  
Ship the resource-backed font pipeline described in `docs/finished/Plan_SceneGraph_Renderer_Finished.md` so TypographyStyle consumers (widgets, labels, HTML adapter) resolve fonts through PathSpace resources instead of hard-coded bitmap glyphs. The widget gallery should render with fonts supplied by the new manager, and renderers must receive resource fingerprints for atlas reuse.

**Prerequisites**
- ✅ (December 10, 2025) Confirmed HarfBuzz/ICU availability on macOS CI: GitHub Actions now installs `harfbuzz`, `icu4c`, and `pkg-config` via Homebrew and exports a `PKG_CONFIG_PATH` covering `/opt/homebrew` + `/usr/local` pkgconfig roots so the UI/Metal builds pick up the shaping libs during compile/test runs.
- ✅ (December 10, 2025) Captured the ASCII TextBuilder fallback footprint (command count + bounds) in `tests/ui/test_TextBuilder.cpp` to preserve the legacy bitmap path during the FontManager rollout.
- ✅ (December 10, 2025) Aligned schema with `Plan_SceneGraph_Renderer_Finished.md` §"Plan: Resource system (images, fonts, shaders)" and §"Decision: Text shaping, bidi, and font fallback"; `docs/AI_PATHS.md` now documents images/font/shader resources and `docs/AI_ARCHITECTURE.md` captures the shaping stack decisions.

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
- Document schema in `docs/AI_PATHS.md` and update `docs/finished/Plan_SceneGraph_Renderer_Finished.md` cross-links.

**Phase 1 – Font Manager Foundations (2–3 days)**
- ✅ (October 29, 2025) Landed the `SP::UI::FontManager` singleton with descriptor fingerprinting, fallback shaping, an LRU shaped-run cache, and diagnostics metrics under `diagnostics/metrics/fonts/*`. Current cache eviction is capacity-based; wire atlas-aware budgets once atlas persistence ships. HarfBuzz/ICU shaping remains stubbed out pending dependency review.
- ✅ (October 29, 2025) Resource lookup + fallback chain resolution via PathSpace metadata. `FontManager::resolve_font` reads `meta/{family,style,weight,fallbacks,active_revision}`, deduplicates fallback lists, surfaces the active atlas revision, and widgets/examples hydrate typography directly from those nodes with doctest coverage for success and defaulted metadata.
- ✅ (October 30, 2025) HarfBuzz shaping wrapper ships with CoreText-backed font discovery; shaped-run cache capacity now derives from stored atlas residency budgets (`meta/atlas/{softBytes,hardBytes,shapedRunApproxBytes}`) so eviction pressure tracks the intended atlas residency watermarks.

**Phase 2 – Atlas Generation & Publication (3 days)**
- ✅ (October 30, 2025) Build atlas generator writing Alpha8 atlas payloads into `builds/<revision>/atlas.bin` with glyph UV metadata plus per-revision `meta/atlas.json`; seeded default ASCII atlas so registered fonts persist usable glyph pages immediately.
- ✅ (October 30, 2025) Update snapshot builder to emit `DrawableBucket::font_assets` alongside drawables and persist them under `bucket/font-assets.bin`, keeping fingerprints, resource roots, and revisions latched per drawable.
- ✅ (October 30, 2025) Extend renderer target wiring to prefetch persisted atlases: `PathRenderer2D` now loads `atlas.bin` revisions via `FontAtlasCache`, publishes residency stats, and tracks font atlas fingerprints to avoid redundant uploads.

**Phase 3 – Widget/Text Integration (2 days)**
- ✅ (November 10, 2025) TextBuilder swaps bitmap quads for shaped glyph buckets when a shaping context is active, records font asset fingerprints, and stores glyph vertices for snapshot persistence. PathRenderer2D consumes the metadata and renders shaped text when the pipeline is forced to `Shaped`, falling back to glyph quads otherwise until perf optimizations land.
- ✅ (December 9, 2025) Replaced `TextBuilder` bitmap glyph fallback with FontManager-backed shaping by default: scoped shaping contexts now seed a built-in PathSpaceSans resource pack, typography resolves FontManager metadata automatically, and `tests/ui/test_TextBuilder.cpp` exercises the shaped pipeline.
- ✅ (December 10, 2025) `Widgets::TypographyStyle` carries `font_family`, `font_weight`, `font_features`, `language`, and `direction`; the struct in `include/pathspace/ui/WidgetSharedTypes.hpp` already exposes these fields and they flow through the declarative text pipeline with coverage in `tests/ui/test_TextBuilder.cpp` and `tests/ui/test_WidgetGallery.cpp`.
- ✅ (December 9, 2025) Widget theme loading now auto-registers fonts via `Widgets::LoadTheme`: `src/pathspace/ui/WidgetRuntime.cpp` seeds PathSpaceSans regular + semibold packs, resolves overrides per theme, and `tests/ui/test_WidgetGallery.cpp` asserts the resources exist so the gallery can switch between default/sunset typography without manual setup.
- ✅ (December 9, 2025) Added regression coverage in `tests/ui/test_TextBuilder.cpp` and the new widget gallery UITest (`tests/ui/test_WidgetGallery.cpp`) to assert atlas fingerprints populate in published scene revisions, so shaped text snapshots and renderer telemetry stay guarded end-to-end.
- ✅ (December 10, 2025) Introduced the dual-path atlas strategy for multi-color glyphs: font registration now writes both `atlas.bin` (Alpha8 SDF) and `atlas_color.bin` (RGBA8) plus metadata (`meta/atlas/{hasColor,preferredFormat}`), the text builder chooses the requested lane per typography, snapshots tag `font_assets` with a kind bit, and PathRenderer2D loads the matching atlas so emoji retain their native chroma and residency reporting distinguishes color uploads.
- ✅ (December 10, 2025) Implemented widget-side text input plumbing for the builder `TextField`/`TextArea` widgets: the bindings now apply UTF-8 aware cursor moves, selection updates, clipboard copy/cut/paste, and IME composition start/update/commit flows while emitting the existing widget ops so reducer handlers keep working. Clipboard excerpts persist under `/ops/clipboard/last_text` and the composition payloads reuse the existing `composition_*` fields, so demos/tests can exercise both widgets via the new `tests/ui/test_WidgetBindingsText.cpp` coverage.
- ✅ (December 10, 2025) Wired the HTML adapter’s font-face output to the snapshot fingerprints: `Html::Adapter` now inspects `DrawableBucketSnapshot::font_assets`, derives `fonts/<fingerprint>.woff2` logical paths, infers family/style/weight metadata from the resource roots, emits `@font-face` rules that reference `assets/fonts/<fingerprint>.woff2`, and resolves the matching assets so `output/v1/html/assets/fonts/*` populates deterministically. `tests/ui/test_HtmlAdapter.cpp` covers the new path alongside the legacy manifest flow, and the follow-up Phase 4 diagnostics items remain (adapter metrics + loop logging).

**Phase 4 – HTML Adapter & Diagnostics (1–2 days)**
- ✅ (December 10, 2025) HTML adapter now emits `@font-face` rules referencing `output/v1/html/assets/fonts/<fingerprint>.woff2`, wiring snapshot fingerprints into the HTML asset set.
- ✅ (December 10, 2025) Diagnostics builders now write per-target font telemetry: `PathRenderer2D` copies the current font asset list plus atlas residency bytes into `output/v1/common/font*`, mirrors the font cache stats from `/diagnostics/metrics/fonts`, and `Diagnostics::ReadTargetMetrics` exposes the new fields (also surfaced by `scripts/ui_capture_logs.py`).
- ✅ (December 10, 2025) Logging/test harnesses now capture font diagnostics during the 5× loop: the UITest runner enables `PATHSPACE_CAPTURE_FONT_DIAGNOSTICS=1`, `PathRenderer2D` writes `font_diagnostics.jsonl` into each `PATHSPACE_TEST_ARTIFACT_DIR`, and loop runs keep the artifact alongside the saved logs for font atlas/cache triage.
- ✅ (December 10, 2025) Added HTML adapter diagnostics to the target summaries: `Diagnostics::ReadTargetMetrics` now reads DOM/command/asset counts plus mode and option flags from `output/v1/html`, `DiagnosticsSummaryJson` and `scripts/ui_capture_logs.py` surface them under `metrics.html`, and loop captures include the adapter stats for ServeHtml regressions.

**Phase 5 – Rollout & Hardening (ongoing)**
- ✅ (December 10, 2025) Ran PathRenderer2D and widget_pipeline benchmarks after the FontManager rollout; stored traces in `docs/perf/renderer_fps_traces.json` and `docs/perf/widget_pipeline_benchmark.json` (1280x720 full repaint ~103 FPS / incremental ~1423 FPS; 3840x2160 full ~13.6 FPS; widget_pipeline bucketAvgMs ~0.118 ms, dirty widgets/sec ~6.8k, paint GPU upload ~3.6 ms) to serve as the post-shaping baseline.
- ✅ (December 10, 2025) Added rollback flag `PATHSPACE_UI_FONT_MANAGER_ENABLED` (default on) to let deployments fall back to the bitmap glyph path. The flag short-circuits `ScopedShapingContext` and forces the legacy TextBuilder bucket so shaped text issues can be isolated quickly; UITest coverage guards the toggle.
- ✅ (December 10, 2025) Updated `AI_Debugging_Playbook.md`, `Widget_Contribution_Quickstart.md`, and `Plan_SceneGraph_Renderer_Finished.md` with FontManager troubleshooting steps (rollback/diagnostics, font-asset checks, HTML font fingerprint triage) so the rollout has a documented recovery path.
- ✅ (December 10, 2025) Finalized the DrawableBucket binary schema: every bucket binary now carries a `DBKT` header (version 1, little-endian marker, payload size, reserved zeros) plus an FNV-1a checksum and 8-byte padding. Decode paths verify the checksum/padding and still accept legacy payloads; UITests cover header validation and checksum corruption.
- ✅ (December 10, 2025) Turned on `diagnostics/errors/stats` by default (opt-out via `PATHSPACE_UI_ERROR_STATS=0`) and mirrored code/severity/revision/timestamp/detail into `output/v1/common/lastError*` so legacy tooling and `scripts/ui_capture_logs.py` always pick up structured PathSpaceError data without extra flags.

**Testing & Validation**
- ✅ (December 10, 2025) Augmented the CTest suite with shaping-specific doctests covering Latin kerning pairs, Arabic joining, and Devanagari reordering in `tests/ui/test_TextBuilder.cpp`.
- ✅ (December 10, 2025) Added golden framebuffer comparisons for the widget gallery before/after the FontManager swap. New goldens (`widget_gallery_font_manager_enabled.golden`, `widget_gallery_font_manager_legacy.golden`) live under `tests/ui/golden/` and are exercised by `tests/ui/test_PathRenderer2D.cpp`.
- ✅ (December 10, 2025) Captured drawable bucket metadata digests alongside the widget gallery framebuffers. Bucket goldens (`widget_gallery_font_manager_enabled.bucket.golden`, `widget_gallery_font_manager_legacy.bucket.golden`) hash IDs, transforms, bounds, commands, and font assets with quantized tolerance to guard snapshot structure regressions in `tests/ui/test_PathRenderer2D.cpp`.
- ✅ (December 10, 2025) Pre-push hook now exports `PATHSPACE_TEST_ARTIFACT_DIR`, requires `widget_gallery_font_assets.bin` from the UITest, and fails early if the artifact is missing so atlas persistence is always verified before pushes.
- ✅ (December 10, 2025) Widget gallery UITest (`tests/ui/test_WidgetGallery.cpp`) decodes the latest scene snapshot, asserts persisted `font_assets` fingerprints, and writes the atlas reference artifact for the pre-push guard.

**Risks & Mitigations**
- **Third-party deps**: If HarfBuzz integration blocks, start with FreeType-lite subset and gate advanced scripts behind feature flag.
- **Atlas size creep**: Implement LRU eviction and compression (PNG or basisu) for persisted atlases; document budget tuning knobs.
- **HTML/browser parity**: Validate fonts render identically in native and HTML adapters; capture mismatches in regression harness.

**Success Criteria**
- Widget gallery renders using resource-backed fonts with stable diffs.
- Renderers and HTML adapter consume atlas fingerprints without redundant uploads.
- Diagnostics surfaces atlas/cache metrics, and documentation reflects new workflow.

## Open Questions
- ✅ (December 10, 2025) Decision: constrain MVP targets to 8-bit RGBA/BGRA outputs in sRGB or linear light; defer DisplayP3 and FP (RGBA16F/32F) framebuffers to the post-MVP GPU track. Surface descriptors now reject unsupported color spaces or FP formats so render paths align with the MVP scope.
- ✅ (December 10, 2025) Decision: Fonts stay on the Runtime::Resources::Fonts path (app-root `resources/fonts/<family>/<style>/builds/<rev>` with atlas metadata and residency metrics) and renderer/HTML consumers read the published `font_assets` from snapshots. Images remain revision-local under `scenes/<sid>/builds/<rev>/assets/images/<fingerprint>.png`, with PathRenderer2D + HtmlAdapter decoding/caching by fingerprint and surfacing per-target residency stats. The digest-indexed, policy-driven resource manager (assets index, LRU watermarks, shared shader/image pools) moves to the deferred GPU/resource track.
- ✅ (December 10, 2025) Validated the Metal presenter during live resize/fullscreen: CAMetalLayer blits now clamp to drawable and source bounds, the LocalWindow bridge flushes/recycles IOSurfaces when resize/fullscreen begins, and presents defer until the resize completes to avoid main-runloop stalls or stale drawables.
- ✅ (December 10, 2025) Decision: per-target renderer metrics now publish under `diagnostics/metrics/*` with a compatibility mirror in `output/v1/common/*`; dashboards should follow the diagnostics tree for stability.
- ✅ (December 10, 2025) Decision: stage path-traced lighting and tetrahedral acceleration in two steps. Fold TLAS/BLAS emission and tetra adjacency generation into the ongoing GPU backend track under an `rt_foundations` flag so snapshot builder/presenter artifacts stay aligned while renderers remain raster/compute-only. Ship the full path-traced integrator (CPU fallback plus GPU compute/RT) after the GPU baseline stabilizes, reusing the shared acceleration/cache plumbing and gating it behind a separate path-tracing toggle to keep the MVP risk low.
- ✅ (December 10, 2025) Decision: history metadata accepts user-provided tags. Callers may set `_history/set_tag = "<label>"` (std::string payload) before committing mutations; the tag is persisted on each journal entry, surfaces under `_history/lastOperation/tag`, and flows through undo/redo telemetry so UI/inspector tooling can display command names alongside history stats.
- ✅ (December 10, 2025) History telemetry now mirrors under `/diagnostics/history/<encoded-root>/` with compatibility links in `/output/v1/diagnostics/history/`, exposing stats plus per-entry summaries (operation, path, tag, timestamps, payload byte counts, presence flags) so tooling can read history state without touching user namespaces.

## Documentation and Rollout Checklist
- ✅ (December 10, 2025) Updated `README.md` build instructions with UI flags (`PATHSPACE_ENABLE_UI`, `PATHSPACE_UI_SOFTWARE`, `PATHSPACE_UI_METAL`) and platform notes so contributors enable the renderer/presenter stack when building.
- Keep `docs/finished/Plan_SceneGraph_Renderer_Finished.md` cross-references aligned; link to this implementation plan from that doc and vice versa.
- ✅ (December 10, 2025) Added developer onboarding snippets (test loop + artifact inspection) to `docs/AI_Onboarding_Next.md` and `docs/AI_Onboarding.md` so new contributors can run the loop and find captured outputs without digging through playbooks.
- Track milestone completion in `AI_Todo.task` or the equivalent planning artifact.

## References
- Completed implementation details: `docs/finished/Plan_SceneGraph_Implementation_Finished.md`.
- Specification: `docs/finished/Plan_SceneGraph_Renderer_Finished.md`.
- Core architecture overview: `docs/AI_Architecture.md`.

## Maintenance Considerations
- Ensure feature flags allow partial builds (e.g., disable the UI pipeline when unsupported).
- Monitor binary artifact sizes for snapshots; consider tooling to inspect revisions.
- Plan for future GPU backend work without blocking the MVP—keep interfaces abstract and avoid hard-coding software assumptions.
- Establish guardrails for progressive mode defaults to avoid regressing latency-sensitive apps.
- Respect residency policies and track resource lifecycle metrics so caches stay healthy.
