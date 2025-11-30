# Plan: Widget Theme Simplification

## Background
- Declarative widget fragments currently serialize a full `meta/style` blob that mirrors the sunrise palette embedded in `WidgetSharedTypes.hpp`.
- The renderer, descriptor loader, tests, and docs assume the serialized style already contains the final colors, so theme overrides are never consulted unless a widget/window explicitly sets `style/theme`.
- Docs such as `Widget_Schema_Reference.md` describe a different intent: widgets should inherit their palette from `style/theme`, and literal `meta/style` data should be reserved for bespoke overrides.
- Result: even though the global theme defaults to “sunset”, samples like `examples/declarative_button_example.cpp` render with hard-coded sunrise colors unless the runtime rewrites `/meta/style` after mount.

## Goals
1. Make every declarative widget derive its palette and typography from the resolved `WidgetTheme`, regardless of whether the fragment serialized any style data.
2. Restrict per-widget serialized data to structural/layout overrides (width/height/spacing/etc.) and typography adjustments when explicitly requested.
3. Keep the docs/tests consistent with the new contract (theme-driven colors by default, optional overrides layered afterward).
4. Provide a migration-friendly rollout so downstream patches can transition without rewriting all widgets at once.

## Non-goals
- Overhauling the `ThemeConfig` storage format or inheritance logic.
- Adding new theme tokens; the existing `WidgetTheme` struct remains the canonical palette.
- Changing how imperative widgets or legacy builder APIs work unless absolutely required for compilation.

## Current State Summary
| Layer | Behavior today | Issue |
| --- | --- | --- |
| Fragment builders (`Button::Fragment`, etc.) | Serialize only structural/layout data plus explicit overrides; palette/typography lanes are zeroed unless an override bit is set. | ✅ Step 1–2 shipped; serialized blobs remain lean and easy to diff. |
| Runtime rewrite (`apply_theme_defaults`) | Removed November 29, 2025 — fragments no longer trigger an immediate `/meta/style` rewrite. | ✅ Eliminated the redundant write; descriptors now own theme layering. |
| Descriptor loading (`DescriptorDetail::Read*`) | Buttons/toggles/sliders/lists/trees/InputField/TextArea now load the resolved `WidgetTheme` and layer serialized overrides. | ✅ Theme-first descriptors are complete for the current widget set; wire future widgets into the same helper as they land. |
| Tests/examples | Many fixtures still inspect literal colors in `/meta/style`. | Update samples/tests to assert against descriptors/theme-aware outputs instead of serialized blobs; expand coverage beyond buttons/lists. |

## Proposed Architecture
1. **Theme-first descriptor assembly** – Descriptor loaders start with the resolved `WidgetTheme` struct, then layer any serialized overrides (width/height/etc.) on top. Colors default to the theme unless a widget explicitly overrides them.
2. **Lean widget serialization** – Fragments only write the fields users can override declaratively (layout metrics, typography tweaks, custom colors). If a field is left at its sentinel value, it is omitted or marked as “inherit”.
3. **Explicit override markers** – When the declarative author wants to override colors, we keep writing them to `/meta/style`, but we also set a bitmask/flag so the merge logic knows which properties were intentionally overridden.
4. **Docs & tests alignment** – Update schema docs, migration guides, and regression tests to reflect “theme by default, overrides explicitly opt-in”.

## Implementation Phases

### Phase 1 — Runtime Plumbing
1. Update `BuilderWidgets::ButtonStyle` (and siblings) to differentiate between “unset” and “explicit” fields (e.g., optional colors or a `StyleOverrideMask`).
2. Modify fragment helpers so they only serialize fields that the caller set. Default-constructed args now zero theme-managed palette arrays (`{0,0,0,0}`) and reset typography blobs to an "inherit" sentinel unless an override bit is set, so `/meta/style` only carries structural/layout data plus the explicit overrides a caller provided.
3. Remove `apply_theme_defaults` from `initialize_render`. Widgets no longer need an immediate rewrite because descriptors will handle theme application.

> **Status (November 29, 2025):** Phase 1 is now complete. Override masks + serialization scrubbing shipped earlier, and this change deleted `apply_theme_defaults` while leaning on descriptor-side theme merges. Declarative widgets only persist structural/layout overrides plus intentional palette edits, and the runtime no longer rewrites `/meta/style` after mounting a fragment. Follow-on work lives in Phase 2 (renderer/descriptors) and Phase 3 (docs/tests).

### Phase 2 — Descriptor & Renderer Merge
1. Extend `DescriptorDetail::ResolveThemeForWidget` to return both the `WidgetTheme` and the canonical theme name. ✅ (`ThemeContext` already exposes both.)
2. In each `Read*Descriptor`, start from the `WidgetTheme` style and merge serialized overrides (respecting the override mask). Colors that were not serialized remain theme-provided. ✅ Buttons/toggles/sliders/lists/trees/InputField/TextArea now share the helper; keep extending the same plumbing to new widgets as they land.
3. Remove `ApplyThemeOverride` (done) and ensure no downstream cache re-applies themes on top of descriptor output.
4. Update renderer buckets and widget-event helpers so they no longer assume `/meta/style` contains fully-resolved colors. ✅ (November 29, 2025) Declarative descriptor/bucket plumbing already consumes theme-resolved styles, and WidgetEventHelpers now resolve the active theme + descriptor before computing slider/list/tree interactions, so the live pointer/focus math no longer depends on serialized palette data.

### Phase 3 — API & Content Updates
1. Update `include/pathspace/ui/declarative/Widgets.hpp` so `Args` structs expose explicit override helpers (e.g., `Args::style_override` or setters that mark fields as intentional overrides).
2. Audit samples/tests: remove direct color tweaks where possible, add explicit overrides where customization is required (paint examples, themed demos, etc.).
3. Refresh docs (`Widget_Schema_Reference.md`, `WidgetDeclarativeAPI.md`, relevant plan/status docs) to describe the new behavior and migration guidance.
> **Status (November 30, 2025):** Step 1 landed via the new `Args::style_override()` builders for buttons, toggles, sliders, lists, and trees. Step 2 is now complete after rewriting `examples/declarative_button_example.cpp` to read descriptor/theme colors instead of `/meta/style` and adding slider/toggle coverage to `tests/ui/test_DeclarativeTheme.cpp`; those tests, along with the existing samples, now assert via descriptors so palette tweaks always flow through `style_override()`. The schema/API docs were refreshed in Step 3 to call out the helper usage, and future samples should follow the same pattern.

### Phase 4 — Validation & Migration Support
1. Expand `tests/ui/test_DeclarativeTheme.cpp` to cover the new merge logic (no serialized colors → theme colors; serialized overrides → override wins; inheritance chain still works). ✅ Buttons/lists/sliders/toggles are covered, and as of November 30, 2025 the suite now includes tree palette overrides (plus mask-bit assertions) and InputField/TextArea layout regression tests demonstrating that structural overrides survive theme layering.
2. Add regression tests for each widget type to ensure serialized layout overrides survive theme application.
> **Status (November 30, 2025):** Coverage now includes buttons, toggles, sliders, lists, trees, InputField, and TextArea via the `tests/ui/test_DeclarativeTheme.cpp` cases (“Button/Toggle/Slider/List/Tree descriptor preserves layout overrides while layering theme colors”, plus the existing input/text-area cases). Layout-only overrides flow through descriptors while palette values keep following the active theme, so every widget exercises the theme-first contract.
3. Provide a short migration note in `docs/Memory.md` (or the tracking doc) so downstream contributors know to drop literal palette blobs.
> **Status (November 30, 2025):** Documented the reminder in `docs/Memory.md` (“Drop literal palette blobs; rely on theme + style_override()”). Contributors now have an easily discoverable pointer before editing widgets or examples.
4. Monitor the CI loop (especially screenshot baselines) because palette changes will finally show up; update baselines as needed.

## Remaining TODOs (November 30, 2025)
> **Update (November 30, 2025):** Loop stability regained after rewriting the Dining Philosophers subcase in `tests/unit/test_PathSpace_multithreading.cpp` to cap runtime at ~1.2 s (shared deadline, bounded `Block` waits, meal quotas, and latch-based shutdown). `PathSpaceTests` now finishes in ~9.2 s locally, keeping the mandated 5× loop under the 20 s per-test budget.

- **Palette monitoring:** Keep screenshot baselines + CI loop telemetry under review while palette changes propagate (Phase 4 Step 4); refresh baselines when declarative theme tweaks intentionally shift colors and log each intentional palette delta in the plan.

## Risks & Mitigations
- **Risk:** Breaking existing widgets that relied on implicit sunrise palette. *Mitigation:* Provide a compatibility flag during transition or explicitly rewrite the handful of examples/tests that depend on the old colors.
- **Risk:** Increased descriptor cost when merging styles. *Mitigation:* Keep the merge lightweight (stack-allocated structs, bitmasks) and benchmark against current runs.
- **Risk:** Third-party contributors might still serialize full styles out of habit. *Mitigation:* Update lint/tests to fail when a fragment writes unused style fields without marking them as overrides.

## Open Questions
1. Do we want to support partial overrides at the color-token level (e.g., override only button background but keep text)? If so, we need a consistent override mask per widget.
2. How do we migrate existing serialized data under `/system/applications/*/widgets/**/meta/style`? One option is to keep honoring old blobs while emitting warnings until a cleanup tool rewrites them.
3. Should typography (font family/size) also default entirely to themes, or are per-widget font overrides still desirable by default?

## Success Criteria
- Samples (`declarative_button_example`, `widgets_example*`, paint demos) render with the active theme colors without manual overrides.
- Docs and schema references no longer instruct contributors to persist full style blobs.
- Tests explicitly validate theme inheritance and override layering for each widget type.
