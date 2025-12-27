#include "Common.hpp"

#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/declarative/PaintSurfaceTypes.hpp>
#include <pathspace/ui/declarative/WidgetMailbox.hpp>
#include <pathspace/ui/declarative/WidgetPrimitives.hpp>
#include <pathspace/ui/declarative/WidgetEventCommon.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/DebugFlags.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::UI::Declarative::Detail {

using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::Widgets::WidgetSpaceRoot;
namespace Primitives = SP::UI::Declarative::Primitives;

namespace {

auto debug_tree_enabled() -> bool {
    return SP::UI::DebugTreeWritesEnabled();
}

class CallbackRegistry {
public:
    static auto instance() -> CallbackRegistry& {
        static CallbackRegistry registry;
        return registry;
    }

    auto store(std::string const& widget_root,
               std::string_view event_name,
               HandlerKind kind,
               HandlerVariant handler) -> std::string {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<std::monostate>(handler)) {
            return {};
        }
        auto id = compose_id(widget_root, event_name);
        auto handler_path = handler_path_string(widget_root, event_name);
        entries_[id] = HandlerEntry{
            .widget_root = widget_root,
            .event_name = std::string(event_name),
            .kind = kind,
            .handler = std::move(handler),
            .handler_path = handler_path,
        };
        path_index_[handler_path] = id;
        return id;
    }

    auto erase_prefix(std::string const& widget_root) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.widget_root == widget_root) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    auto handler_path_string(std::string const& widget_root,
                             std::string_view event_name) -> std::string {
        auto path = WidgetSpacePath(widget_root, "events");
        path = make_path(std::move(path), event_name);
        return make_path(std::move(path), "handler");
    }

    struct HandlerEntry {
        std::string widget_root;
        std::string event_name;
        HandlerKind kind = HandlerKind::None;
        HandlerVariant handler;
        std::string handler_path;
    };

    auto compose_id(std::string const& widget_root,
                    std::string_view event_name) -> std::string {
        std::string id = widget_root;
        id.push_back('#');
        id.append(event_name);
        id.push_back('#');
        id.append(std::to_string(counter_.fetch_add(1, std::memory_order_relaxed) + 1));
        return id;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, HandlerEntry> entries_;
    std::unordered_map<std::string, std::string> path_index_;
    std::atomic<std::uint64_t> counter_{0};

public:
    auto rebind(std::string const& from_root,
                std::string const& to_root)
        -> std::vector<std::pair<std::string, HandlerBinding>> {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, HandlerBinding>> updates;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.widget_root != from_root) {
                ++it;
                continue;
            }
            auto entry = HandlerEntry{
                .widget_root = to_root,
                .event_name = it->second.event_name,
                .kind = it->second.kind,
                .handler = std::move(it->second.handler),
                .handler_path = handler_path_string(to_root, it->second.event_name),
            };
            it = entries_.erase(it);

            auto new_key = compose_id(to_root, entry.event_name);
            HandlerBinding binding{
                .registry_key = new_key,
                .kind = entry.kind,
            };
            entries_.emplace(new_key, entry);
            path_index_[entry.handler_path] = new_key;
            updates.emplace_back(entry.event_name, binding);
        }
        return updates;
    }

    auto rebind_by_key(std::string const& registry_key,
                       std::string const& new_root)
        -> std::optional<HandlerBinding> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(registry_key);
        if (it == entries_.end()) {
            return std::nullopt;
        }

        auto entry = HandlerEntry{
            .widget_root = new_root,
            .event_name = it->second.event_name,
            .kind = it->second.kind,
            .handler = std::move(it->second.handler),
            .handler_path = handler_path_string(new_root, it->second.event_name),
        };
        entries_.erase(it);

        auto new_key = compose_id(new_root, entry.event_name);
        HandlerBinding binding{
            .registry_key = new_key,
            .kind = entry.kind,
        };
        entries_.emplace(new_key, entry);
        path_index_[entry.handler_path] = new_key;
        return binding;
    }

    auto resolve(std::string const& registry_key) -> std::optional<HandlerVariant> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(registry_key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        return it->second.handler;
    }

    auto erase(std::string const& registry_key) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(registry_key);
        for (auto it = path_index_.begin(); it != path_index_.end();) {
            if (it->second == registry_key) {
                it = path_index_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto unlink_path(std::string const& handler_path) -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = path_index_.find(handler_path);
        if (it == path_index_.end()) {
            return std::nullopt;
        }
        auto key = it->second;
        path_index_.erase(it);
        return key;
    }

    auto register_path(std::string handler_path, std::string registry_key) -> void {
        std::lock_guard<std::mutex> lock(mutex_);
        path_index_[std::move(handler_path)] = std::move(registry_key);
    }
};

} // namespace

auto make_path(std::string base, std::string_view component) -> std::string {
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(component);
    return base;
}

auto handler_binding_path(std::string const& root, std::string_view event) -> std::string {
    auto path = WidgetSpacePath(root, "events");
    path = make_path(std::move(path), event);
    return make_path(std::move(path), "handler");
}

auto mount_base(std::string_view parent,
                MountOptions const& options) -> std::string {
    if (!options.slot_override.empty()) {
        return make_path(std::string(parent), options.slot_override);
    }
    if (options.policy == MountPolicy::WindowWidgets) {
        return make_path(std::string(parent), "widgets");
    }
    if (options.policy == MountPolicy::WidgetChildren) {
        return make_path(std::string(parent), "children");
    }
    std::string path{parent};
    auto windows_pos = path.find("/windows/");
    auto widgets_pos = path.find("/widgets/");
    if (widgets_pos == std::string::npos && windows_pos != std::string::npos) {
        return make_path(path, "widgets");
    }
    return make_path(path, "children");
}

auto write_kind(PathSpace& space,
                std::string const& root,
                std::string const& kind) -> SP::Expected<void> {
    return write_value(space, WidgetSpacePath(root, "/meta/kind"), kind);
}

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void> {
    if (auto status = write_value(space,
                                  WidgetSpacePath(root, "/render/synthesize"),
                                  RenderDescriptor{kind});
        !status) {
        return status;
    }
    if (auto status = write_value(space,
                                  WidgetSpacePath(root, "/render/dirty_version"),
                                  std::uint64_t{0});
        !status) {
        return status;
    }
    return mark_render_dirty(space, root);
}

auto mark_render_dirty(PathSpaceBase& space,
                       std::string const& root) -> SP::Expected<void> {
    if (auto status = write_value(space, WidgetSpacePath(root, "/render/dirty"), true); !status) {
        return status;
    }
    auto event_path = WidgetSpacePath(root, "/render/events/dirty");
    auto inserted = space.insert(event_path, root);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    auto version_path = WidgetSpacePath(root, "/render/dirty_version");
    auto current_version = space.read<std::uint64_t, std::string>(version_path);
    std::uint64_t next_version = current_version ? (*current_version + 1) : 1;
    if (auto status = replace_single<std::uint64_t>(space, version_path, next_version); !status) {
        return status;
    }
    return {};
}

auto reset_widget_space(PathSpace& space,
                        std::string const& root) -> SP::Expected<void> {
    auto space_root = WidgetSpaceRoot(root);
    auto taken = space.take<std::unique_ptr<PathSpace>>(space_root);
    if (!taken) {
        auto const& error = taken.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
    }

    auto nested = std::make_unique<PathSpace>();
    auto inserted = space.insert(space_root, std::move(nested));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto write_handler(PathSpace& space,
                   std::string const& root,
                   std::string_view event,
                   HandlerKind kind,
                   HandlerVariant handler) -> SP::Expected<void> {
    if (std::holds_alternative<std::monostate>(handler)) {
        return {};
    }
    auto path = handler_binding_path(root, event);
    if (auto previous = CallbackRegistry::instance().unlink_path(path); previous) {
        CallbackRegistry::instance().erase(*previous);
    }
    auto key = CallbackRegistry::instance().store(root, event, kind, std::move(handler));
    if (key.empty()) {
        return {};
    }
    HandlerBinding binding{
        .registry_key = key,
        .kind = kind,
    };
    return write_value(space, path, binding);
}

auto write_fragment_handlers(PathSpace& space,
                             std::string const& root,
                             std::vector<FragmentHandler> const& handlers) -> SP::Expected<void> {
    for (auto const& handler : handlers) {
        if (auto status = write_handler(space,
                                        root,
                                        handler.event,
                                        handler.kind,
                                        handler.handler);
            !status) {
            return status;
        }
    }
    return {};
}

auto clear_handlers(std::string const& widget_root) -> void {
    CallbackRegistry::instance().erase_prefix(widget_root);
}

auto rebind_handlers(PathSpace& space,
                     std::string const& old_root,
                     std::string const& new_root) -> SP::Expected<void> {
    (void)old_root;
    auto events_base = WidgetSpacePath(new_root, "events");
    auto events = space.listChildren(SP::ConcretePathStringView{events_base});
    for (auto const& event : events) {
        auto handler_path = make_path(events_base, event) + "/handler";
        auto binding = space.read<HandlerBinding, std::string>(handler_path);
        if (!binding) {
            continue;
        }
        auto updated = CallbackRegistry::instance().rebind_by_key(binding->registry_key, new_root);
        if (!updated) {
            continue;
        }
        if (auto status = write_value(space, handler_path, *updated); !status) {
            return status;
        }
    }
    return {};
}

auto resolve_handler(std::string const& registry_key) -> std::optional<HandlerVariant> {
    return CallbackRegistry::instance().resolve(registry_key);
}

auto read_handler_binding(PathSpace& space,
                          std::string const& root,
                          std::string_view event)
    -> SP::Expected<std::optional<HandlerBinding>> {
    auto path = handler_binding_path(root, event);
    auto binding = space.read<HandlerBinding, std::string>(path);
    if (!binding) {
        auto const& error = binding.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return std::optional<HandlerBinding>{};
        }
        return std::unexpected(error);
    }
    return std::optional<HandlerBinding>{*binding};
}

auto clear_handler_binding(PathSpace& space,
                           std::string const& root,
                           std::string_view event) -> SP::Expected<void> {
    auto path = handler_binding_path(root, event);
    if (auto previous = CallbackRegistry::instance().unlink_path(path); previous) {
        CallbackRegistry::instance().erase(*previous);
    }
    auto removed = space.take<HandlerBinding>(path);
    if (!removed) {
        auto const& error = removed.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return {};
        }
        return std::unexpected(error);
    }
    return {};
}

namespace {

auto bump_counter(PathSpaceBase& space, std::string const& path) -> void {
    auto current = space.read<std::uint64_t, std::string>(path);
    std::uint64_t next = current ? (*current + 1) : 1;
    (void)DeclarativeDetail::replace_single<std::uint64_t>(space, path, next);
}

template <typename T>
auto write_capsule_value(PathSpace& space,
                         std::string const& widget_root,
                         std::string_view relative,
                         T const& value) -> SP::Expected<void> {
    auto path = WidgetSpacePath(widget_root, relative);
    return DeclarativeDetail::replace_single(space, path, value);
}

auto op_kind_name(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind) -> std::string_view {
    auto topic = Mailbox::TopicFor(kind);
    if (topic.empty()) {
        return "other";
    }
    return topic;
}

auto kind_to_string(WidgetKind kind) -> std::string_view {
    switch (kind) {
    case WidgetKind::Button:
        return "button";
    case WidgetKind::Label:
        return "label";
    case WidgetKind::Toggle:
        return "toggle";
    case WidgetKind::Slider:
        return "slider";
    case WidgetKind::List:
        return "list";
    case WidgetKind::Tree:
        return "tree";
    case WidgetKind::Stack:
        return "stack";
    case WidgetKind::TextArea:
        return "text_area";
    case WidgetKind::InputField:
        return "input_field";
    case WidgetKind::PaintSurface:
        return "paint_surface";
    }
    return "unknown";
}

auto write_button_primitives(PathSpace& space,
                             std::string const& root,
                             BuilderWidgets::ButtonStyle const& style,
                             std::string const& label,
                             std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    WidgetPrimitive surface{
        .id = "surface",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.background_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = style.corner_radius,
            .clip_children = true,
        },
    };

    WidgetPrimitive text{
        .id = "label",
        .kind = WidgetPrimitiveKind::Text,
        .children = {},
        .data = TextPrimitive{
            .text = label,
            .text_path = WidgetSpacePath(root, "/capsule/meta/label"),
            .color = style.text_color,
            .typography = style.typography,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Horizontal;
    layout_data.distribution = LayoutDistribution::Intrinsic;
    layout_data.spacing = 8.0f;
    layout_data.padding = {12.0f, 10.0f, 12.0f, 10.0f};
    layout_data.stretch_children = false;

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = {surface.id, text.id},
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Clickable,
            .topics = topics,
        },
    };

    std::vector<WidgetPrimitive> primitives;
    primitives.push_back(surface);
    primitives.push_back(text);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto write_toggle_primitives(PathSpace& space,
                             std::string const& root,
                             BuilderWidgets::ToggleStyle const& style,
                             BuilderWidgets::ToggleState const& state,
                             std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    auto track_color = state.checked ? style.track_on_color : style.track_off_color;
    float radius = std::max(0.0f, std::min(style.width, style.height) * 0.5f);
    WidgetPrimitive track{
        .id = "track",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = track_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = radius,
            .clip_children = true,
        },
    };

    WidgetPrimitive thumb{
        .id = "thumb",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.thumb_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = radius,
            .clip_children = false,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Horizontal;
    layout_data.distribution = LayoutDistribution::Weighted;
    layout_data.spacing = 4.0f;
    layout_data.padding = {4.0f, 4.0f, 4.0f, 4.0f};
    layout_data.weights = {1.0f, 0.0f};
    layout_data.stretch_children = true;

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = {track.id, thumb.id},
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Toggle,
            .topics = topics,
        },
    };

    std::vector<WidgetPrimitive> primitives;
    primitives.push_back(track);
    primitives.push_back(thumb);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto write_label_primitives(PathSpace& space,
                            std::string const& root,
                            std::string const& text_value,
                            BuilderWidgets::TypographyStyle const& typography,
                            std::array<float, 4> const& color,
                            std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    WidgetPrimitive text{
        .id = "label",
        .kind = WidgetPrimitiveKind::Text,
        .children = {},
        .data = TextPrimitive{
            .text = text_value,
            .text_path = WidgetSpacePath(root, "/capsule/state/text"),
            .color = color,
            .typography = typography,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Horizontal;
    layout_data.distribution = LayoutDistribution::Intrinsic;
    layout_data.spacing = 0.0f;
    layout_data.padding = {0.0f, 0.0f, 0.0f, 0.0f};

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = {text.id},
        .data = layout_data,
    };

    std::vector<WidgetPrimitive> primitives;
    WidgetPrimitiveIndex index{};

    if (!topics.empty()) {
        WidgetPrimitive behavior{
            .id = "behavior",
            .kind = WidgetPrimitiveKind::Behavior,
            .children = {layout.id},
            .data = BehaviorPrimitive{
                .kind = BehaviorKind::Clickable,
                .topics = topics,
            },
        };
        primitives.push_back(behavior);
        index.roots.push_back(behavior.id);
    } else {
        index.roots.push_back(layout.id);
    }

    primitives.push_back(text);
    primitives.push_back(layout);

    return WritePrimitives(space, root, primitives, index);
}

auto write_slider_primitives(PathSpace& space,
                             std::string const& root,
                             BuilderWidgets::SliderStyle const& style,
                             BuilderWidgets::SliderRange const& range,
                             BuilderWidgets::SliderState const& state,
                             std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    float span = std::max(range.maximum - range.minimum, 1e-5f);
    float clamped = std::clamp(state.value, range.minimum, range.maximum);
    float ratio = std::clamp((clamped - range.minimum) / span, 0.0f, 1.0f);

    float track_radius = std::max(0.0f, style.track_height * 0.5f);
    float thumb_radius = std::max(0.0f, style.thumb_radius);

    WidgetPrimitive fill{
        .id = "fill",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.fill_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = track_radius,
            .clip_children = true,
        },
    };

    WidgetPrimitive track{
        .id = "track",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.track_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = track_radius,
            .clip_children = true,
        },
    };

    WidgetPrimitive thumb{
        .id = "thumb",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.thumb_color,
            .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .border_width = 0.0f,
            .corner_radius = thumb_radius,
            .clip_children = false,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Horizontal;
    layout_data.distribution = LayoutDistribution::Weighted;
    layout_data.spacing = 4.0f;
    layout_data.padding = {style.thumb_radius, 0.0f, style.thumb_radius, 0.0f};
    layout_data.weights = {ratio, std::max(0.0f, 1.0f - ratio), 0.0f};
    layout_data.stretch_children = true;

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = {fill.id, track.id, thumb.id},
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Input,
            .topics = topics,
        },
    };

    std::vector<WidgetPrimitive> primitives;
    primitives.push_back(fill);
    primitives.push_back(track);
    primitives.push_back(thumb);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto scale_color(std::array<float, 4> color, float factor) -> std::array<float, 4> {
    for (auto& component : color) {
        component = std::clamp(component * factor, 0.0f, 1.0f);
    }
    return color;
}

auto write_list_primitives(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::ListStyle const& style,
                           std::vector<BuilderWidgets::ListItem> const& items,
                           BuilderWidgets::ListState state,
                           std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    auto effective_style = style;
    auto effective_state = state;

    if (!effective_state.enabled) {
        constexpr float kDisabledFactor = 0.6f;
        effective_style.background_color = scale_color(effective_style.background_color, kDisabledFactor);
        effective_style.border_color = scale_color(effective_style.border_color, kDisabledFactor);
        effective_style.item_color = scale_color(effective_style.item_color, kDisabledFactor);
        effective_style.item_hover_color = scale_color(effective_style.item_hover_color, kDisabledFactor);
        effective_style.item_selected_color = scale_color(effective_style.item_selected_color, kDisabledFactor);
        effective_style.separator_color = scale_color(effective_style.separator_color, kDisabledFactor);
        effective_style.item_text_color = scale_color(effective_style.item_text_color, kDisabledFactor);
        effective_state.hovered_index = -1;
        effective_state.selected_index = -1;
    }

    WidgetPrimitive background{
        .id = "background",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = effective_style.background_color,
            .border_color = effective_style.border_color,
            .border_width = effective_style.border_thickness,
            .corner_radius = effective_style.corner_radius,
            .clip_children = true,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Vertical;
    layout_data.distribution = LayoutDistribution::Weighted;
    layout_data.spacing = effective_style.border_thickness;
    layout_data.padding = {0.0f, 0.0f, 0.0f, 0.0f};
    layout_data.stretch_children = true;

    std::vector<std::string> layout_children;
    layout_children.reserve(items.size());
    layout_data.weights.reserve(items.size());

    std::vector<WidgetPrimitive> primitives;
    primitives.reserve(2 + items.size() * 2);

    auto item_color_for = [&](std::size_t index, bool enabled) {
        auto base = effective_style.item_color;
        if (enabled && static_cast<std::int32_t>(index) == effective_state.selected_index) {
            base = effective_style.item_selected_color;
        } else if (enabled && static_cast<std::int32_t>(index) == effective_state.hovered_index) {
            base = effective_style.item_hover_color;
        }
        if (!enabled) {
            base = scale_color(base, 0.6f);
        }
        return base;
    };

    for (std::size_t index = 0; index < items.size(); ++index) {
        auto const& item = items[index];
        bool item_enabled = effective_state.enabled && item.enabled;
        auto row_id = std::string{"row_"}.append(std::to_string(index));
        auto label_id = std::string{"row_label_"}.append(std::to_string(index));

        WidgetPrimitive row{
            .id = row_id,
            .kind = WidgetPrimitiveKind::Surface,
            .children = {label_id},
            .data = SurfacePrimitive{
                .shape = SurfaceShape::Rectangle,
                .fill_color = item_color_for(index, item_enabled),
                .border_color = effective_style.separator_color,
                .border_width = effective_style.border_thickness > 0.0f ? effective_style.border_thickness : 0.0f,
                .corner_radius = 0.0f,
                .clip_children = false,
            },
        };

        WidgetPrimitive label{
            .id = label_id,
            .kind = WidgetPrimitiveKind::Text,
            .children = {},
            .data = TextPrimitive{
                .text = item.label,
                .text_path = WidgetSpacePath(root, "/meta/items"),
                .color = item_enabled ? effective_style.item_text_color
                                      : scale_color(effective_style.item_text_color, 0.6f),
                .typography = effective_style.item_typography,
            },
        };

        primitives.push_back(row);
        primitives.push_back(label);
        layout_children.push_back(row_id);
        layout_data.weights.push_back(1.0f);
    }

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = std::move(layout_children),
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {background.id, layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Scroll,
            .topics = topics,
        },
    };

    primitives.push_back(background);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto row_color_for(TreeRowInfo const& row,
                   BuilderWidgets::TreeState const& state,
                   BuilderWidgets::TreeStyle const& style,
                   bool row_enabled) -> std::array<float, 4> {
    if (!row_enabled || !state.enabled) {
        return style.row_disabled_color;
    }
    if (!state.selected_id.empty() && state.selected_id == row.id) {
        return style.row_selected_color;
    }
    if (!state.hovered_id.empty() && state.hovered_id == row.id) {
        return style.row_hover_color;
    }
    return style.row_color;
}

auto write_tree_primitives(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::TreeStyle const& style,
                           std::vector<BuilderWidgets::TreeNode> const& nodes,
                           BuilderWidgets::TreeState state,
                           std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    auto effective_style = style;
    auto effective_state = state;
    if (!effective_state.enabled) {
        constexpr float kDisabledFactor = 0.6f;
        effective_style.background_color = scale_color(effective_style.background_color, kDisabledFactor);
        effective_style.border_color = scale_color(effective_style.border_color, kDisabledFactor);
        effective_style.row_color = scale_color(effective_style.row_color, kDisabledFactor);
        effective_style.row_hover_color = scale_color(effective_style.row_hover_color, kDisabledFactor);
        effective_style.row_selected_color = scale_color(effective_style.row_selected_color, kDisabledFactor);
        effective_style.row_disabled_color = scale_color(effective_style.row_disabled_color, kDisabledFactor);
        effective_style.connector_color = scale_color(effective_style.connector_color, kDisabledFactor);
        effective_style.toggle_color = scale_color(effective_style.toggle_color, kDisabledFactor);
        effective_style.text_color = scale_color(effective_style.text_color, kDisabledFactor);
        effective_state.hovered_id.clear();
        effective_state.selected_id.clear();
    }

    std::unordered_map<std::string, BuilderWidgets::TreeNode const*> node_lookup;
    node_lookup.reserve(nodes.size());
    for (auto const& node : nodes) {
        node_lookup.emplace(node.id, &node);
    }

    TreeData tree_data{};
    tree_data.state = effective_state;
    tree_data.style = effective_style;
    tree_data.nodes = nodes;
    auto rows = build_tree_rows(tree_data);

    WidgetPrimitive background{
        .id = "background",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = effective_style.background_color,
            .border_color = effective_style.border_color,
            .border_width = effective_style.border_thickness,
            .corner_radius = effective_style.corner_radius,
            .clip_children = true,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Vertical;
    layout_data.distribution = LayoutDistribution::Weighted;
    layout_data.spacing = effective_style.border_thickness;
    layout_data.padding = {effective_style.border_thickness,
                           effective_style.border_thickness,
                           effective_style.border_thickness,
                           effective_style.border_thickness};
    layout_data.stretch_children = true;

    std::vector<std::string> layout_children;
    layout_children.reserve(rows.size());
    layout_data.weights.reserve(rows.size());

    std::vector<WidgetPrimitive> primitives;
    primitives.reserve(2 + rows.size() * 4);

    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto const& row = rows[index];
        auto it = node_lookup.find(row.id);
        std::string label = it != node_lookup.end() ? it->second->label : std::string{};
        bool row_enabled = (it == node_lookup.end()) ? effective_state.enabled
                                                     : (effective_state.enabled && it->second->enabled);

        auto row_color = row_color_for(row, effective_state, effective_style, row_enabled);
        auto text_color = row_enabled ? effective_style.text_color
                                      : scale_color(effective_style.text_color, 0.6f);

        std::string toggle_id;
        std::vector<std::string> row_children;
        row_children.reserve(row.expandable ? 2u : 1u);

        if (row.expandable) {
            toggle_id = std::string{"row_toggle_"}.append(std::to_string(index));
            WidgetPrimitive toggle{
                .id = toggle_id,
                .kind = WidgetPrimitiveKind::Text,
                .children = {},
                .data = TextPrimitive{
                    .text = row.expanded ? "-" : "+",
                    .text_path = WidgetSpacePath(root, "/capsule/meta/nodes"),
                    .color = effective_style.toggle_color,
                    .typography = effective_style.label_typography,
                },
            };
            primitives.push_back(toggle);
            row_children.push_back(toggle_id);
        }

        auto label_id = std::string{"row_label_"}.append(std::to_string(index));
        WidgetPrimitive label_prim{
            .id = label_id,
            .kind = WidgetPrimitiveKind::Text,
            .children = {},
            .data = TextPrimitive{
                .text = label,
                .text_path = WidgetSpacePath(root, "/capsule/meta/nodes"),
                .color = text_color,
                .typography = effective_style.label_typography,
            },
        };
        primitives.push_back(label_prim);
        row_children.push_back(label_id);

        auto row_layout_id = std::string{"row_layout_"}.append(std::to_string(index));
        BoxLayoutPrimitive row_layout{};
        row_layout.axis = LayoutAxis::Horizontal;
        row_layout.distribution = LayoutDistribution::Intrinsic;
        row_layout.spacing = 8.0f;
        float indent = std::max(0.0f,
                                effective_style.indent_per_level * static_cast<float>(row.depth));
        row_layout.padding = {indent, 6.0f, 6.0f, 6.0f};
        row_layout.stretch_children = false;

        WidgetPrimitive layout{
            .id = row_layout_id,
            .kind = WidgetPrimitiveKind::BoxLayout,
            .children = std::move(row_children),
            .data = row_layout,
        };
        primitives.push_back(layout);

        auto row_id = std::string{"row_"}.append(std::to_string(index));
        WidgetPrimitive row_surface{
            .id = row_id,
            .kind = WidgetPrimitiveKind::Surface,
            .children = {row_layout_id},
            .data = SurfacePrimitive{
                .shape = SurfaceShape::Rectangle,
                .fill_color = row_color,
                .border_color = effective_style.connector_color,
                .border_width = 0.0f,
                .corner_radius = 0.0f,
                .clip_children = false,
            },
        };
        primitives.push_back(row_surface);

        layout_children.push_back(row_id);
        layout_data.weights.push_back(1.0f);
    }

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = std::move(layout_children),
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {background.id, layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Scroll,
            .topics = topics,
        },
    };

    primitives.push_back(background);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto write_stack_primitives(PathSpace& space,
                            std::string const& root,
                            BuilderWidgets::StackLayoutStyle const& style,
                            std::vector<std::string> const& panel_ids,
                            std::string const& active_panel,
                            std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = style.axis == BuilderWidgets::StackAxis::Horizontal ? LayoutAxis::Horizontal
                                                                           : LayoutAxis::Vertical;
    layout_data.distribution = LayoutDistribution::Weighted;
    layout_data.spacing = style.spacing;
    layout_data.stretch_children = true;

    if (style.axis == BuilderWidgets::StackAxis::Horizontal) {
        layout_data.padding = {style.padding_main_start,
                               style.padding_cross_start,
                               style.padding_main_end,
                               style.padding_cross_end};
    } else {
        layout_data.padding = {style.padding_cross_start,
                               style.padding_main_start,
                               style.padding_cross_end,
                               style.padding_main_end};
    }

    std::vector<WidgetPrimitive> primitives;
    primitives.reserve(panel_ids.size() + 2);

    std::vector<std::string> layout_children;
    layout_children.reserve(panel_ids.size());
    layout_data.weights.reserve(panel_ids.size());

    for (auto const& panel_id : panel_ids) {
        WidgetPrimitive panel_surface{
            .id = std::string{"panel_"}.append(panel_id),
            .kind = WidgetPrimitiveKind::Surface,
            .children = {},
            .data = SurfacePrimitive{
                .shape = SurfaceShape::Rectangle,
                .fill_color = {0.0f, 0.0f, 0.0f, 0.0f},
                .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
                .border_width = 0.0f,
                .corner_radius = 0.0f,
                .clip_children = style.clip_contents,
            },
        };

        layout_children.push_back(panel_surface.id);
        layout_data.weights.push_back(panel_id == active_panel ? 1.0f : 0.0f);
        primitives.push_back(panel_surface);
    }

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = std::move(layout_children),
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Clickable,
            .topics = topics,
        },
    };

    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

auto write_input_primitives(PathSpace& space,
                            std::string const& root,
                            BuilderWidgets::TextFieldStyle const& style,
                            BuilderWidgets::TextFieldState const& state,
                            std::vector<std::string> const& topics) -> SP::Expected<void> {
    using namespace Primitives;

    auto placeholder_text = state.text.empty() ? state.placeholder : std::string{};

    WidgetPrimitive background{
        .id = "background",
        .kind = WidgetPrimitiveKind::Surface,
        .children = {},
        .data = SurfacePrimitive{
            .shape = SurfaceShape::RoundedRect,
            .fill_color = style.background_color,
            .border_color = style.border_color,
            .border_width = style.border_thickness,
            .corner_radius = style.corner_radius,
            .clip_children = true,
        },
    };

    WidgetPrimitive text{
        .id = "text",
        .kind = WidgetPrimitiveKind::Text,
        .children = {},
        .data = TextPrimitive{
            .text = state.text,
            .text_path = WidgetSpacePath(root, "/capsule/state/text"),
            .color = style.text_color,
            .typography = style.typography,
        },
    };

    WidgetPrimitive placeholder{
        .id = "placeholder",
        .kind = WidgetPrimitiveKind::Text,
        .children = {},
        .data = TextPrimitive{
            .text = placeholder_text,
            .text_path = WidgetSpacePath(root, "/capsule/state/placeholder"),
            .color = style.placeholder_color,
            .typography = style.typography,
        },
    };

    BoxLayoutPrimitive layout_data{};
    layout_data.axis = LayoutAxis::Horizontal;
    layout_data.distribution = LayoutDistribution::Intrinsic;
    layout_data.spacing = 0.0f;
    layout_data.padding = {style.padding_x, style.padding_y, style.padding_x, style.padding_y};
    layout_data.stretch_children = false;

    WidgetPrimitive layout{
        .id = "layout",
        .kind = WidgetPrimitiveKind::BoxLayout,
        .children = {text.id, placeholder.id},
        .data = layout_data,
    };

    WidgetPrimitive behavior{
        .id = "behavior",
        .kind = WidgetPrimitiveKind::Behavior,
        .children = {background.id, layout.id},
        .data = BehaviorPrimitive{
            .kind = BehaviorKind::Input,
            .topics = topics,
        },
    };

    std::vector<WidgetPrimitive> primitives;
    primitives.push_back(background);
    primitives.push_back(text);
    primitives.push_back(placeholder);
    primitives.push_back(layout);
    primitives.push_back(behavior);

    WidgetPrimitiveIndex index{{behavior.id}};
    return WritePrimitives(space, root, primitives, index);
}

} // namespace

auto mirror_button_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::ButtonState const& state,
                           BuilderWidgets::ButtonStyle const& style,
                           std::string const& label,
                           bool has_press_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);

    auto lambda = std::string{"declarative.widget.button.render_bucket"};
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "press",
        "release",
    };
    if (has_press_handler) {
        subscriptions.push_back("activate");
    }

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"button"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/label", label); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_button_primitives(space, root, prepared_style, label, subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_toggle_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::ToggleState const& state,
                           BuilderWidgets::ToggleStyle const& style,
                           bool has_toggle_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);

    auto lambda = std::string{"declarative.widget.toggle.render_bucket"};
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "press",
        "release",
        "toggle",
    };
    if (has_toggle_handler) {
        subscriptions.push_back("activate");
    }

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"toggle"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_toggle_primitives(space, root, prepared_style, state, subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_label_capsule(PathSpace& space,
                          std::string const& root,
                          std::string const& text,
                          BuilderWidgets::TypographyStyle const& typography,
                          std::array<float, 4> const& color,
                          bool has_activate_handler) -> SP::Expected<void> {
    auto lambda = std::string{"declarative.widget.label.render_bucket"};
    std::vector<std::string> subscriptions{"hover_enter", "hover_exit", "press", "release"};
    if (has_activate_handler) {
        subscriptions.emplace_back("activate");
    }

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"label"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state/text", text); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/typography", typography); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/color", color); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_label_primitives(space,
                                             root,
                                             text,
                                             typography,
                                             color,
                                             subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_slider_capsule(PathSpace& space,
                           std::string const& root,
                           BuilderWidgets::SliderState const& state,
                           BuilderWidgets::SliderStyle const& style,
                           BuilderWidgets::SliderRange const& range,
                           bool has_change_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);
    (void)has_change_handler;

    auto lambda = std::string{"declarative.widget.slider.render_bucket"};
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "slider_begin",
        "slider_update",
        "slider_commit",
    };

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"slider"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/range", range); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_slider_primitives(space, root, prepared_style, range, state, subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_list_capsule(PathSpace& space,
                         std::string const& root,
                         BuilderWidgets::ListState const& state,
                         BuilderWidgets::ListStyle const& style,
                         std::vector<BuilderWidgets::ListItem> const& items,
                         bool has_child_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);
    (void)has_child_handler;

    auto lambda = std::string{"declarative.widget.list.render_bucket"};
    std::vector<std::string> subscriptions{
        "list_hover",
        "list_select",
        "list_activate",
        "list_scroll",
    };

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"list"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/items", items); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_list_primitives(space, root, prepared_style, items, state, subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_tree_capsule(PathSpace& space,
                         std::string const& root,
                         BuilderWidgets::TreeState const& state,
                         BuilderWidgets::TreeStyle const& style,
                         std::vector<BuilderWidgets::TreeNode> const& nodes,
                         bool has_node_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);

    auto lambda = std::string{"declarative.widget.tree.render_bucket"};
    std::vector<std::string> subscriptions{
        "tree_hover",
        "tree_select",
        "tree_toggle",
        "tree_expand",
        "tree_collapse",
        "tree_scroll",
    };
    if (has_node_handler) {
        subscriptions.push_back("tree_request_load");
    }

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"tree"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/nodes", nodes); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_tree_primitives(space, root, prepared_style, nodes, state, subscriptions);
        !status) {
        return status;
    }
    return SP::Expected<void>{};
}

auto mirror_stack_capsule(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::StackLayoutStyle const& style,
                          std::vector<std::string> const& panel_ids,
                          std::string const& active_panel,
                          bool has_select_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);
    (void)has_select_handler;

    auto lambda = std::string{"declarative.widget.stack.render_bucket"};
    std::vector<std::string> subscriptions{"stack_select"};

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"stack"}); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state/active_panel", active_panel);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/panels", panel_ids); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }

    return write_stack_primitives(space, root, prepared_style, panel_ids, active_panel, subscriptions);
}

auto mirror_input_capsule(PathSpace& space,
                          std::string const& root,
                          BuilderWidgets::TextFieldState const& state,
                          BuilderWidgets::TextFieldStyle const& style,
                          bool has_change_handler,
                          bool has_submit_handler) -> SP::Expected<void> {
    auto prepared_style = prepare_style_for_serialization(style);
    (void)has_change_handler;

    auto lambda = std::string{"declarative.widget.input_field.render_bucket"};
    std::vector<std::string> subscriptions{
        "text_hover",
        "text_focus",
        "text_input",
        "text_delete",
        "text_move_cursor",
        "text_set_selection",
        "text_composition_start",
        "text_composition_update",
        "text_composition_commit",
        "text_composition_cancel",
        "text_clipboard_copy",
        "text_clipboard_cut",
        "text_clipboard_paste",
        "text_scroll",
    };
    if (has_submit_handler) {
        subscriptions.push_back("text_submit");
    }

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"input_field"});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/style", prepared_style);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/render/metrics/invocations_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/events_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/dispatch_failures_total",
                                          std::uint64_t{0});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/metrics/last_dispatch_ns",
                                          std::uint64_t{0});
        !status) {
        return status;
    }

    return write_input_primitives(space, root, prepared_style, state, subscriptions);
}

auto mirror_paint_surface_capsule(PathSpace& space,
                                  std::string const& root,
                                  float brush_size,
                                  std::array<float, 4> const& brush_color,
                                  std::uint32_t buffer_width,
                                  std::uint32_t buffer_height,
                                  float buffer_dpi,
                                  bool gpu_enabled) -> SP::Expected<void> {
    PaintBufferMetrics buffer_defaults{
        .width = buffer_width,
        .height = buffer_height,
        .dpi = buffer_dpi,
    };

    auto lambda = std::string{"declarative.widget.paint_surface.render_bucket"};
    std::vector<std::string> subscriptions{
        "paint_stroke_begin",
        "paint_stroke_update",
        "paint_stroke_commit",
    };

    if (auto status = write_capsule_value(space, root, "/capsule/kind", std::string{"paint_surface"});
        !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state/brush/size", brush_size); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state/brush/color", brush_color); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/state/gpu/enabled", gpu_enabled); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/meta/buffer", buffer_defaults); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space, root, "/capsule/render/lambda", lambda); !status) {
        return status;
    }
    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/mailbox/subscriptions",
                                          subscriptions);
        !status) {
        return status;
    }
    (void)write_capsule_value(space,
                              root,
                              "/capsule/render/metrics/invocations_total",
                              std::uint64_t{0});
    (void)write_capsule_value(space,
                              root,
                              "/capsule/mailbox/metrics/events_total",
                              std::uint64_t{0});
    (void)write_capsule_value(space,
                              root,
                              "/capsule/mailbox/metrics/dispatch_failures_total",
                              std::uint64_t{0});
    (void)write_capsule_value(space,
                              root,
                              "/capsule/mailbox/metrics/last_dispatch_ns",
                              std::uint64_t{0});

    return SP::Expected<void>{};
}

auto update_button_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::ButtonState const& state) -> SP::Expected<void> {
    return write_capsule_value(space, root, "/capsule/state", state);
}

auto update_button_capsule_label(PathSpace& space,
                                 std::string const& root,
                                 std::string const& label) -> SP::Expected<void> {
    if (auto status = write_capsule_value(space, root, "/capsule/meta/label", label); !status) {
        return status;
    }

    auto style = space.read<BuilderWidgets::ButtonStyle, std::string>(
        WidgetSpacePath(root, "/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto topics_path = WidgetSpacePath(root, "/capsule/mailbox/subscriptions");
    auto topics = space.read<std::vector<std::string>, std::string>(topics_path);
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "press",
        "release",
    };
    if (topics) {
        subscriptions = *topics;
    }

    auto prepared_style = prepare_style_for_serialization(*style);
    return write_button_primitives(space, root, prepared_style, label, subscriptions);
}

auto update_toggle_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::ToggleState const& state) -> SP::Expected<void> {
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }

    auto style = space.read<BuilderWidgets::ToggleStyle, std::string>(
        WidgetSpacePath(root, "/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto topics_path = WidgetSpacePath(root, "/capsule/mailbox/subscriptions");
    auto topics = space.read<std::vector<std::string>, std::string>(topics_path);
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "press",
        "release",
        "toggle",
    };
    if (topics) {
        subscriptions = *topics;
    }

    auto prepared_style = prepare_style_for_serialization(*style);
    return write_toggle_primitives(space, root, prepared_style, state, subscriptions);
}

auto update_label_capsule_text(PathSpace& space,
                               std::string const& root,
                               std::string const& text) -> SP::Expected<void> {
    if (auto status = write_capsule_value(space, root, "/capsule/state/text", text); !status) {
        return status;
    }

    auto typography = space.read<BuilderWidgets::TypographyStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/typography"));
    if (!typography) {
        return std::unexpected(typography.error());
    }

    auto color = space.read<std::array<float, 4>, std::string>(
        WidgetSpacePath(root, "/capsule/meta/color"));
    if (!color) {
        return std::unexpected(color.error());
    }

    auto topics_path = WidgetSpacePath(root, "/capsule/mailbox/subscriptions");
    auto topics = space.read<std::vector<std::string>, std::string>(topics_path);
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "press",
        "release",
    };
    if (topics) {
        subscriptions = *topics;
    }

    return write_label_primitives(space, root, text, *typography, *color, subscriptions);
}

auto update_slider_capsule_state(PathSpace& space,
                                 std::string const& root,
                                 BuilderWidgets::SliderState const& state) -> SP::Expected<void> {
    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }

    auto style = space.read<BuilderWidgets::SliderStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto range = space.read<BuilderWidgets::SliderRange, std::string>(
        WidgetSpacePath(root, "/capsule/meta/range"));
    if (!range) {
        return std::unexpected(range.error());
    }

    auto topics_path = WidgetSpacePath(root, "/capsule/mailbox/subscriptions");
    auto topics = space.read<std::vector<std::string>, std::string>(topics_path);
    std::vector<std::string> subscriptions{
        "hover_enter",
        "hover_exit",
        "slider_begin",
        "slider_update",
        "slider_commit",
    };
    if (topics) {
        subscriptions = *topics;
    }

    auto prepared_style = prepare_style_for_serialization(*style);
    return write_slider_primitives(space, root, prepared_style, *range, state, subscriptions);
}

auto update_list_capsule_state(PathSpace& space,
                               std::string const& root,
                               BuilderWidgets::ListState const& state) -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::ListStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto items = space.read<std::vector<BuilderWidgets::ListItem>, std::string>(
        WidgetSpacePath(root, "/capsule/meta/items"));
    if (!items) {
        return std::unexpected(items.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }

    return write_list_primitives(space, root, *style, *items, state, *topics);
}

auto update_list_capsule_items(PathSpace& space,
                               std::string const& root,
                               std::vector<BuilderWidgets::ListItem> const& items)
    -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::ListStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto state = space.read<BuilderWidgets::ListState, std::string>(
        WidgetSpacePath(root, "/capsule/state"));
    if (!state) {
        return std::unexpected(state.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space, root, "/capsule/meta/items", items); !status) {
        return status;
    }

    return write_list_primitives(space, root, *style, items, *state, *topics);
}

auto update_tree_capsule_state(PathSpace& space,
                               std::string const& root,
                               BuilderWidgets::TreeState const& state) -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::TreeStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto nodes = space.read<std::vector<BuilderWidgets::TreeNode>, std::string>(
        WidgetSpacePath(root, "/capsule/meta/nodes"));
    if (!nodes) {
        return std::unexpected(nodes.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }

    return write_tree_primitives(space, root, *style, *nodes, state, *topics);
}

auto update_tree_capsule_nodes(PathSpace& space,
                               std::string const& root,
                               std::vector<BuilderWidgets::TreeNode> const& nodes)
    -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::TreeStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto state = space.read<BuilderWidgets::TreeState, std::string>(
        WidgetSpacePath(root, "/capsule/state"));
    if (!state) {
        return std::unexpected(state.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space, root, "/capsule/meta/nodes", nodes); !status) {
        return status;
    }

    return write_tree_primitives(space, root, *style, nodes, *state, *topics);
}

auto update_stack_capsule_state(PathSpace& space,
                                std::string const& root,
                                std::string const& active_panel) -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::StackLayoutStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto panels = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/meta/panels"));
    if (!panels) {
        return std::unexpected(panels.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space,
                                          root,
                                          "/capsule/state/active_panel",
                                          active_panel);
        !status) {
        return status;
    }

    return write_stack_primitives(space, root, *style, *panels, active_panel, *topics);
}

auto update_input_capsule_state(PathSpace& space,
                                std::string const& root,
                                BuilderWidgets::TextFieldState const& state)
    -> SP::Expected<void> {
    auto style = space.read<BuilderWidgets::TextFieldStyle, std::string>(
        WidgetSpacePath(root, "/capsule/meta/style"));
    if (!style) {
        return std::unexpected(style.error());
    }

    auto topics = space.read<std::vector<std::string>, std::string>(
        WidgetSpacePath(root, "/capsule/mailbox/subscriptions"));
    if (!topics) {
        return std::unexpected(topics.error());
    }

    if (auto status = write_capsule_value(space, root, "/capsule/state", state); !status) {
        return status;
    }

    return write_input_primitives(space, root, *style, state, *topics);
}

void record_capsule_render_invocation(PathSpace& space,
                                      std::string const& widget_root,
                                      WidgetKind kind) {
    if (!debug_tree_enabled()) {
        return;
    }
    auto meta_kind = space.read<std::string, std::string>(WidgetSpacePath(widget_root, "/meta/kind"));
    if (!meta_kind
        || (*meta_kind != "button" && *meta_kind != "label" && *meta_kind != "toggle"
            && *meta_kind != "slider" && *meta_kind != "list" && *meta_kind != "tree"
            && *meta_kind != "input_field" && *meta_kind != "stack" && *meta_kind != "paint_surface")) {
        return;
    }

    auto count_path = WidgetSpacePath(widget_root, "/capsule/render/metrics/invocations_total");
    bump_counter(space, count_path);

    auto now_ns = DeclarativeDetail::to_epoch_ns(std::chrono::system_clock::now());
    (void)write_capsule_value(space,
                              widget_root,
                              "/capsule/render/metrics/last_invocation/ns",
                              now_ns);
    (void)write_capsule_value(space,
                              widget_root,
                              "/capsule/render/metrics/last_invocation/kind",
                              std::string{kind_to_string(kind)});
}

void record_capsule_mailbox_event(PathSpace& space,
                                  std::string const& widget_root,
                                  SP::UI::Runtime::Widgets::Bindings::WidgetOpKind op_kind,
                                  std::string_view target_id,
                                  std::uint64_t dispatch_ns,
                                  std::uint64_t sequence) {
    if (!debug_tree_enabled()) {
        return;
    }
    auto op_name = op_kind_name(op_kind);
    auto events_root = WidgetSpacePath(widget_root, "/capsule/mailbox/metrics");
    bump_counter(space, events_root + "/events_total");

    auto per_event = WidgetSpacePath(widget_root, "/capsule/mailbox/events/")
        + std::string(op_name) + "/total";
    bump_counter(space, per_event);

    auto metrics_root = std::string{"/capsule/mailbox/metrics/last_event/"};
    auto existing_kind = space.read<std::string, std::string>(
        WidgetSpacePath(widget_root, metrics_root + "kind"));
    auto existing_priority = space.read<std::uint32_t, std::string>(
        WidgetSpacePath(widget_root, metrics_root + "priority"));
    auto sequence_path = WidgetSpacePath(widget_root, metrics_root + "sequence");
    auto existing_sequence = space.read<std::uint64_t, std::string>(sequence_path);

    auto is_hover = op_kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::HoverEnter
        || op_kind == SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::HoverExit;
    auto priority = static_cast<std::uint32_t>(is_hover ? 0 : 1);
    auto current_priority = existing_priority.value_or(0u);

    if (priority < current_priority) {
        return;
    }
    if (priority == current_priority && existing_sequence && sequence > 0 && *existing_sequence >= sequence) {
        return;
    }

    auto timestamp = dispatch_ns == 0
        ? DeclarativeDetail::to_epoch_ns(std::chrono::system_clock::now())
        : dispatch_ns;

    (void)write_capsule_value(space,
                              widget_root,
                              "/capsule/mailbox/metrics/last_dispatch_ns",
                              timestamp);
    (void)write_capsule_value(space, widget_root, metrics_root + "kind", std::string{op_name});
    (void)write_capsule_value(space, widget_root, metrics_root + "ns", timestamp);
    (void)write_capsule_value(space, widget_root, metrics_root + "sequence", sequence == 0 ? 0 : sequence);
    (void)write_capsule_value(space, widget_root, metrics_root + "priority", priority);
    if (!target_id.empty()) {
        (void)write_capsule_value(space,
                                  widget_root,
                                  "/capsule/mailbox/metrics/last_event/target",
                                  std::string{target_id});
    }
}

void record_capsule_mailbox_failure(PathSpace& space, std::string const& widget_root) {
    if (!debug_tree_enabled()) {
        return;
    }
    auto metrics_root = WidgetSpacePath(widget_root, "/capsule/mailbox/metrics");
    bump_counter(space, metrics_root + "/dispatch_failures_total");
    auto timestamp = DeclarativeDetail::to_epoch_ns(std::chrono::system_clock::now());
    (void)write_capsule_value(space,
                              widget_root,
                              "/capsule/mailbox/metrics/last_dispatch_ns",
                              timestamp);
}

} // namespace SP::UI::Declarative::Detail
