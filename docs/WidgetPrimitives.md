# Widget Primitives (Capsule Runtime)

_Last updated: December 12, 2025_

## Purpose
Document the initial primitive schema used by widget capsules so renderer/mailbox consumers can rely on a stable structure while the composable runtime is rolled out.

## Storage layout
- Per widget under `capsule/primitives/<id>`.
- `capsule/primitives/index` lists the root primitive ids for the widget.
- Each `WidgetPrimitive` stores:
  - `id` – stable identifier within the widget.
  - `kind` – `Surface`, `Text`, `Icon`, `BoxLayout`, or `Behavior`.
  - `children` – ordered ids for composition.
  - `data` – kind-specific payload:
    - **Surface:** `fill_color`, optional `border_color`/`border_width`, `corner_radius`, `clip_children`.
    - **Text:** `text`, `text_path` (source node), `color`, `typography`.
    - **Icon:** `glyph`, `atlas_path`, `size`.
    - **BoxLayout:** `axis` (`Horizontal`/`Vertical`), `distribution` (`Even`/`Weighted`/`Intrinsic`), `spacing`, `padding{l,t,r,b}`, optional `weights`, `stretch_children` flag.
    - **Behavior:** `kind` (`Clickable`, `Toggle`, `Focusable`, `Scroll`, `Input`), `topics` (mailbox subscriptions/topics).

Capsule runtime is now enabled by default; the former `PATHSPACE_WIDGET_CAPSULES` flag is no longer required for primitives or mailboxes.

## Current coverage (capsule runtime default)
- **Button**
  - Roots: `behavior -> layout(surface, label)`.
  - Surface color + radius derive from button style; text color/typography mirror style; topics mirror mailbox subscriptions (hover/press/release/activate when present).
  - `Button::SetLabel` rewrites the text primitive alongside `capsule/meta/label`.
- **Toggle**
  - Roots: `behavior -> layout(track, thumb)`.
  - Track color follows `checked` state (on/off colors); thumb color mirrors style. `Toggle::SetChecked` rewrites the track primitive after state changes.
  - Topics mirror mailbox subscriptions (hover/press/release/toggle/activate when present).
- **Label**
  - Roots: `layout(label)` or `behavior -> layout(label)` when `activate` is registered.
  - Text pulls from `capsule/state/text` with stored typography/color; updates from `Label::SetText` rewrite the primitive.
- **Slider**
  - Roots: `behavior -> layout(fill, track, thumb)` with weighted horizontal distribution to reflect the current value.
  - Fill/track colors mirror style; weights rewrite when the slider value changes; topics cover hover/begin/update/commit for mailbox dispatch.
- **List**
  - Roots: `behavior -> background, layout(row_*)` where layout is vertical/weighted and each row surface nests a text primitive.
  - Row colors reflect hover/selection state and fall back to the default item color; selection updates rewrite the row primitives. Topics mirror list hover/select/activate/scroll mailboxes.
- **Tree**
  - Roots: `behavior -> background, layout(row_*)` where rows are surfaces containing a padded horizontal layout of optional toggle text plus label text.
  - Row padding encodes tree depth (`indent_per_level`), toggles use `+`/`-` glyphs colored by `style.toggle_color`, and row fill colors follow hover/selection/disabled state.
  - Behavior topics mirror tree hover/select/toggle/expand/collapse/scroll subscriptions and include `tree_request_load` when a node handler is registered.
- **Stack**
  - Roots: `behavior -> layout(panel_*)` with weighted distribution so the active panel holds the available space.
  - Panel surfaces clip children based on `StackLayoutStyle::clip_contents`; weights rewrite when `active_panel` changes and mailbox subscriptions include `stack_select`.

## TODO
- Extend primitives to presenters.
- Add icon/text compound primitives for buttons with glyphs.
- Document rendering semantics (z-ordering, surface/behavior interaction) once the capsule walker consumes primitives.
- Expose default window `BoxLayout` root in the scene readiness helpers.
