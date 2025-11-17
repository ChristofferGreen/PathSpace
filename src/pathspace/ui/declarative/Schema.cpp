#include <pathspace/ui/declarative/Schema.hpp>

#include <algorithm>

namespace SP::UI::Declarative {
namespace {

template <std::size_t N>
constexpr auto make_view(SchemaEntry const (&entries)[N]) -> SchemaEntryView {
    return SchemaEntryView{entries, N};
}

constexpr SchemaEntry kApplicationEntries[] = {
    {"state/title", NodeKind::Value, Requirement::Required, "Human-readable title published for the application."},
    {"windows/<window-id>", NodeKind::Directory, Requirement::RuntimeManaged, "Window namespaces mounted beneath the application."},
    {"scenes/<scene-id>", NodeKind::Directory, Requirement::RuntimeManaged, "Declarative scene namespaces owned by the application."},
    {"themes/default", NodeKind::Value, Requirement::Optional, "Identifier of the default widget theme for the application."},
    {"themes/<theme-name>", NodeKind::Directory, Requirement::Optional, "Theme definitions available to the application."},
    {"events/lifecycle/handler", NodeKind::Callable, Requirement::Optional, "Lifecycle handler invoked for application events."},
};

constexpr SchemaEntry kWindowEntries[] = {
    {"state/title", NodeKind::Value, Requirement::Required, "Window title mirrored into native shells."},
    {"state/visible", NodeKind::Flag, Requirement::RuntimeManaged, "Visibility flag toggled by the runtime when showing or hiding the window."},
    {"style/theme", NodeKind::Value, Requirement::Optional, "Theme override scoped to this window."},
    {"widgets/<widget-name>", NodeKind::Directory, Requirement::RuntimeManaged, "Declarative widget roots mounted under the window."},
    {"events/close/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the window close request fires."},
    {"events/focus/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the window focus state changes."},
    {"render/dirty", NodeKind::Flag, Requirement::RuntimeManaged, "Dirty flag requesting a window-level render pass."},
};

constexpr SchemaEntry kSceneEntries[] = {
    {"structure/widgets/<widget-path>", NodeKind::Directory, Requirement::RuntimeManaged, "Projection of mounted widget buckets consumed by renderers."},
    {"structure/window/<window-id>/focus/current", NodeKind::Value, Requirement::RuntimeManaged, "Current focus path for the window within this scene."},
    {"structure/window/<window-id>/metrics/dpi", NodeKind::Value, Requirement::RuntimeManaged, "Effective DPI reported by the window presenter."},
    {"structure/window/<window-id>/accessibility/dirty", NodeKind::Flag, Requirement::RuntimeManaged, "Flag prompting accessibility bridge refresh for the window."},
    {"snapshot/<revision>", NodeKind::Directory, Requirement::RuntimeManaged, "Immutable snapshot artifacts published per revision."},
    {"snapshot/current", NodeKind::Value, Requirement::RuntimeManaged, "Pointer to the active snapshot revision."},
    {"metrics/<metric-name>", NodeKind::Value, Requirement::RuntimeManaged, "Scene metrics (layout, timing, residency) published for diagnostics."},
    {"events/present/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the scene presents a new frame."},
    {"views/<view-id>/dirty", NodeKind::Flag, Requirement::RuntimeManaged, "Per-view dirty bit so presenters render independently."},
    {"state/attached", NodeKind::Flag, Requirement::RuntimeManaged, "Indicates whether the scene is actively attached to a presenter."},
    {"render/dirty", NodeKind::Flag, Requirement::RuntimeManaged, "Dirty bit driving scene-level re-synthesis."},
};

constexpr SchemaEntry kThemeEntries[] = {
    {"colors/<token>", NodeKind::Value, Requirement::Required, "Color token palette referenced by widgets."},
    {"typography/<token>", NodeKind::Value, Requirement::Optional, "Typography token palette applied to text widgets."},
    {"spacing/<token>", NodeKind::Value, Requirement::Optional, "Spacing tokens used by layout helpers."},
    {"style/inherits", NodeKind::Value, Requirement::Optional, "Parent theme this theme derives from."},
};

constexpr NamespaceSchema kNamespaces[] = {
    {"application", "Root namespace for declarative applications.", make_view(kApplicationEntries)},
    {"window", "Window namespace hosting declarative widget roots.", make_view(kWindowEntries)},
    {"scene", "Declarative scene namespace consumed by presenters.", make_view(kSceneEntries)},
    {"theme", "Theme definitions consumed by declarative widgets.", make_view(kThemeEntries)},
};

constexpr SchemaEntry kWidgetCommonEntries[] = {
    {"state", NodeKind::Directory, Requirement::Required, "Widget state payload visible to application code."},
    {"style/theme", NodeKind::Value, Requirement::Optional, "Theme override applied to the widget subtree."},
    {"focus/order", NodeKind::Value, Requirement::RuntimeManaged, "Depth-first focus order assigned by the runtime."},
    {"focus/disabled", NodeKind::Flag, Requirement::Optional, "Disables participation in focus traversal when true."},
    {"focus/current", NodeKind::Value, Requirement::RuntimeManaged, "Mirror indicating the widget currently holds focus."},
    {"focus/wrap", NodeKind::Flag, Requirement::Optional, "Override to disable wrap-around focus behaviour for the subtree."},
    {"layout/orientation", NodeKind::Value, Requirement::Optional, "Primary axis orientation for container widgets."},
    {"layout/spacing", NodeKind::Value, Requirement::Optional, "Spacing between child widgets in container layouts."},
    {"layout/computed/size", NodeKind::Value, Requirement::RuntimeManaged, "Latest computed widget size in layout units."},
    {"layout/computed/children/<child-name>", NodeKind::Value, Requirement::RuntimeManaged, "Computed layout metrics for each child widget."},
    {"children/<child-name>", NodeKind::Directory, Requirement::Optional, "Child widget fragments keyed by stable names."},
    {"events/<event>/handler", NodeKind::Callable, Requirement::Optional, "Callable executed when the widget event fires."},
    {"events/inbox/queue", NodeKind::Queue, Requirement::RuntimeManaged, "Canonical event queue populated with WidgetAction payloads."},
    {"events/<event>/queue", NodeKind::Queue, Requirement::Optional, "Per-event filtered queue mirroring `events/inbox/queue`."},
    {"render/synthesize", NodeKind::Callable, Requirement::Required, "Callable that produces the widget's DrawableBucketSnapshot."},
    {"render/bucket", NodeKind::Value, Requirement::RuntimeManaged, "Cached render bucket for the current widget state."},
    {"render/dirty", NodeKind::Flag, Requirement::RuntimeManaged, "Dirty flag signaling cached render data must be refreshed."},
    {"log/events", NodeKind::Queue, Requirement::RuntimeManaged, "Runtime event log for diagnostics and instrumentation."},
};

constexpr SchemaEntry kButtonEntries[] = {
    {"state/label", NodeKind::Value, Requirement::Required, "Displayed label string for the button."},
    {"state/enabled", NodeKind::Flag, Requirement::Optional, "Indicates whether the button accepts interaction."},
    {"events/press/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the button is pressed."},
};

constexpr SchemaEntry kToggleEntries[] = {
    {"state/checked", NodeKind::Flag, Requirement::Required, "Current toggle state (true when selected)."},
    {"events/toggle/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the toggle changes state."},
};

constexpr SchemaEntry kSliderEntries[] = {
    {"state/value", NodeKind::Value, Requirement::Required, "Current slider value within the configured range."},
    {"state/range/min", NodeKind::Value, Requirement::Required, "Inclusive lower bound for the slider value."},
    {"state/range/max", NodeKind::Value, Requirement::Required, "Inclusive upper bound for the slider value."},
    {"state/dragging", NodeKind::Flag, Requirement::RuntimeManaged, "Runtime-managed flag indicating the slider is being dragged."},
    {"events/change/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the slider value changes."},
};

constexpr SchemaEntry kListEntries[] = {
    {"layout/orientation", NodeKind::Value, Requirement::Optional, "Layout orientation for list items."},
    {"layout/spacing", NodeKind::Value, Requirement::Optional, "Spacing between list entries."},
    {"state/scroll_offset", NodeKind::Value, Requirement::RuntimeManaged, "Current scroll offset tracked by the runtime."},
    {"events/child_event/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when a child event is emitted."},
};

constexpr SchemaEntry kTreeEntries[] = {
    {"nodes/<node-id>/state", NodeKind::Directory, Requirement::RuntimeManaged, "State payload for a tree node (expanded, selected, metadata)."},
    {"nodes/<node-id>/children", NodeKind::Directory, Requirement::RuntimeManaged, "Child node descriptors linked under the parent node."},
    {"events/node_event/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when a tree node interaction occurs."},
};

constexpr SchemaEntry kStackEntries[] = {
    {"panels/<panel-id>/state", NodeKind::Directory, Requirement::RuntimeManaged, "Panel state metadata hosted by the stack."},
    {"state/active_panel", NodeKind::Value, Requirement::Required, "Identifier of the currently active panel."},
    {"events/panel_select/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the active panel changes."},
};

constexpr SchemaEntry kLabelEntries[] = {
    {"state/text", NodeKind::Value, Requirement::Required, "Text content displayed by the label."},
    {"events/activate/handler", NodeKind::Callable, Requirement::Optional, "Optional handler used when the label is activated for accessibility."},
};

constexpr SchemaEntry kInputFieldEntries[] = {
    {"state/text", NodeKind::Value, Requirement::Required, "Current text content for the input field."},
    {"state/placeholder", NodeKind::Value, Requirement::Optional, "Placeholder text displayed when the field is empty."},
    {"state/focused", NodeKind::Flag, Requirement::RuntimeManaged, "Runtime-managed flag indicating the field has focus."},
    {"events/change/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the field text changes."},
    {"events/submit/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked when the field is submitted."},
};

constexpr SchemaEntry kPaintSurfaceEntries[] = {
    {"state/brush/size", NodeKind::Value, Requirement::Optional, "Brush size used for new strokes."},
    {"state/brush/color", NodeKind::Value, Requirement::Optional, "Brush color used for new strokes."},
    {"state/stroke_mode", NodeKind::Value, Requirement::Optional, "Stroke mode (draw, erase, flood) for the surface."},
    {"state/history/<stroke-id>", NodeKind::Directory, Requirement::RuntimeManaged, "Ordered stroke history persisted for undo/redo."},
    {"render/buffer", NodeKind::Value, Requirement::RuntimeManaged, "CPU-readable paint buffer representing the current picture."},
    {"render/buffer/metrics/width", NodeKind::Value, Requirement::RuntimeManaged, "Width of the paint buffer in pixels."},
    {"render/buffer/metrics/height", NodeKind::Value, Requirement::RuntimeManaged, "Height of the paint buffer in pixels."},
    {"render/buffer/metrics/dpi", NodeKind::Value, Requirement::RuntimeManaged, "Effective DPI used to derive the buffer resolution."},
    {"render/buffer/viewport", NodeKind::Value, Requirement::RuntimeManaged, "Viewport describing the visible region when the buffer is clipped."},
    {"render/gpu/enabled", NodeKind::Flag, Requirement::Optional, "Toggle enabling GPU staging for the paint surface."},
    {"render/gpu/state", NodeKind::Value, Requirement::RuntimeManaged, "GPU staging state machine for the paint buffer."},
    {"render/gpu/dirtyRects", NodeKind::Queue, Requirement::RuntimeManaged, "Dirty rectangles queued for incremental GPU uploads."},
    {"render/gpu/fence/start", NodeKind::Value, Requirement::RuntimeManaged, "Timestamp for the start of the latest GPU upload."},
    {"render/gpu/fence/end", NodeKind::Value, Requirement::RuntimeManaged, "Timestamp for the end of the latest GPU upload."},
    {"render/gpu/log/events", NodeKind::Queue, Requirement::RuntimeManaged, "Log of GPU staging events and fallback transitions."},
    {"render/gpu/stats", NodeKind::Value, Requirement::RuntimeManaged, "Staging metrics (bytes uploaded, last duration, partial updates)."},
    {"assets/texture", NodeKind::Value, Requirement::RuntimeManaged, "GPU texture resource mirroring the paint buffer when staging is enabled."},
    {"events/draw/handler", NodeKind::Callable, Requirement::Optional, "Handler invoked to process draw events for the surface."},
};

constexpr WidgetSchema kWidgetSchemas[] = {
    {"button", "Declarative button widget.", make_view(kWidgetCommonEntries), make_view(kButtonEntries)},
    {"toggle", "Declarative toggle widget.", make_view(kWidgetCommonEntries), make_view(kToggleEntries)},
    {"slider", "Declarative slider widget.", make_view(kWidgetCommonEntries), make_view(kSliderEntries)},
    {"list", "Declarative list container widget.", make_view(kWidgetCommonEntries), make_view(kListEntries)},
    {"tree", "Declarative tree container widget.", make_view(kWidgetCommonEntries), make_view(kTreeEntries)},
    {"stack", "Declarative stack/gallery widget switching between panels.", make_view(kWidgetCommonEntries), make_view(kStackEntries)},
    {"label", "Declarative text label widget.", make_view(kWidgetCommonEntries), make_view(kLabelEntries)},
    {"input_field", "Declarative text input field widget.", make_view(kWidgetCommonEntries), make_view(kInputFieldEntries)},
    {"paint_surface", "Declarative paint surface widget with incremental buffers.", make_view(kWidgetCommonEntries), make_view(kPaintSurfaceEntries)},
};

} // namespace

auto declarative_namespaces() -> std::span<NamespaceSchema const> {
    return {std::begin(kNamespaces), std::end(kNamespaces)};
}

auto widget_schemas() -> std::span<WidgetSchema const> {
    return {std::begin(kWidgetSchemas), std::end(kWidgetSchemas)};
}

auto find_namespace_schema(std::string_view name) -> NamespaceSchema const* {
    auto const schemas = declarative_namespaces();
    auto const it =
        std::find_if(schemas.begin(), schemas.end(), [name](NamespaceSchema const& schema) { return schema.name == name; });
    if (it == schemas.end()) {
        return nullptr;
    }
    return &(*it);
}

auto find_widget_schema(std::string_view kind) -> WidgetSchema const* {
    auto const schemas = widget_schemas();
    auto const it =
        std::find_if(schemas.begin(), schemas.end(), [kind](WidgetSchema const& schema) { return schema.kind == kind; });
    if (it == schemas.end()) {
        return nullptr;
    }
    return &(*it);
}

} // namespace SP::UI::Declarative
