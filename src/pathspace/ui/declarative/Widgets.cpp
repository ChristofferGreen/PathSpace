#include <pathspace/ui/declarative/Widgets.hpp>

#include "../BuildersDetail.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace SP::UI::Declarative {
namespace {

using namespace SP::UI::Builders;
using namespace SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;

auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return Detail::make_error(std::move(message), code);
}

enum class HandlerKind : std::uint8_t {
    None = 0,
    ButtonPress,
    Toggle,
    Slider,
    ListChild,
    TreeNode,
    StackPanel,
    LabelActivate,
    InputChange,
    InputSubmit,
    PaintDraw,
};

struct HandlerBinding {
    std::string registry_key;
    HandlerKind kind = HandlerKind::None;
};

enum class WidgetKind : std::uint8_t {
    Button,
    Toggle,
    Slider,
    List,
    Tree,
    Stack,
    Label,
    InputField,
    PaintSurface,
};

struct RenderDescriptor {
    WidgetKind kind = WidgetKind::Button;
};

using HandlerVariant = std::variant<std::monostate,
                                    ButtonHandler,
                                    ToggleHandler,
                                    SliderHandler,
                                    ListChildHandler,
                                    TreeNodeHandler,
                                    StackPanelHandler,
                                    LabelHandler,
                                    InputFieldHandler,
                                    PaintSurfaceHandler>;

class CallbackRegistry {
public:
    static auto instance() -> CallbackRegistry& {
        static CallbackRegistry registry;
        return registry;
    }

    template <typename Handler>
    auto store(std::string const& widget_root,
               std::string const& event_name,
               HandlerKind kind,
               Handler handler) -> std::string {
        if (!handler) {
            return {};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto id = compose_id(widget_root, event_name);
        HandlerEntry entry{
            .widget_root = widget_root,
            .kind = kind,
            .handler = HandlerVariant{std::move(handler)},
        };
        entries_[id] = std::move(entry);
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
    struct HandlerEntry {
        std::string widget_root;
        HandlerKind kind = HandlerKind::None;
        HandlerVariant handler;
    };

    auto compose_id(std::string const& widget_root,
                    std::string const& event_name) -> std::string {
        std::string id = widget_root;
        id.push_back('#');
        id.append(event_name);
        id.push_back('#');
        id.append(std::to_string(counter_++));
        return id;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, HandlerEntry> entries_;
    std::atomic<std::uint64_t> counter_{1};
};

template <typename T>
auto write_value(PathSpace& space,
                 std::string const& path,
                 T const& value) -> SP::Expected<void> {
    if (auto status = replace_single<T>(space, path, value); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto ensure_widget_name(std::string_view name) -> SP::Expected<void> {
    return ensure_identifier(name, "widget name");
}

auto ensure_child_name(std::string_view name) -> SP::Expected<void> {
    return ensure_identifier(name, "child name");
}

auto make_path(std::string base, std::string_view component) -> std::string {
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    base.append(component);
    return base;
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
    auto path = std::string(parent);
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
    return write_value(space, root + "/meta/kind", kind);
}

auto initialize_render(PathSpace& space,
                       std::string const& root,
                       WidgetKind kind) -> SP::Expected<void> {
    if (auto status = write_value(space, root + "/render/synthesize", RenderDescriptor{kind}); !status) {
        return status;
    }
    return write_value(space, root + "/render/dirty", true);
}

auto write_handler(PathSpace& space,
                   std::string const& root,
                   std::string_view event_name,
                   HandlerKind kind,
                   HandlerVariant handler) -> SP::Expected<void> {
    auto event_path = make_path(make_path(root, "events"), event_name);
    event_path.append("/handler");
    std::optional<std::string> key;
    std::visit(
        [&](auto&& value) {
            using HandlerT = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<HandlerT, std::monostate>) {
                key = CallbackRegistry::instance().store(root, std::string(event_name), kind, value);
            }
        },
        handler);

    if (!key) {
        return {};
    }

    HandlerBinding binding{
        .registry_key = *key,
        .kind = kind,
    };
    if (auto status = write_value(space, event_path, binding); !status) {
        return status;
    }
    return {};
}

template <typename State>
auto write_state(PathSpace& space,
                 std::string const& root,
                 State const& state) -> SP::Expected<void> {
    return write_value(space, root + "/state", state);
}

template <typename Style>
auto write_style(PathSpace& space,
                 std::string const& root,
                 Style const& style) -> SP::Expected<void> {
    return write_value(space, root + "/meta/style", style);
}

auto sanitize_button_style(SP::UI::Builders::Widgets::ButtonStyle style)
    -> SP::UI::Builders::Widgets::ButtonStyle {
    style.width = std::max(style.width, 1.0f);
    style.height = std::max(style.height, 1.0f);
    float radius_limit = std::min(style.width, style.height) * 0.5f;
    style.corner_radius = std::clamp(style.corner_radius, 0.0f, radius_limit);
    style.typography.font_size = std::max(style.typography.font_size, 1.0f);
    style.typography.line_height =
        std::max(style.typography.line_height, style.typography.font_size);
    style.typography.letter_spacing = std::max(style.typography.letter_spacing, 0.0f);
    return style;
}

auto sanitize_slider_range(float min_value,
                           float max_value,
                           float step) -> SP::UI::Builders::Widgets::SliderRange {
    SP::UI::Builders::Widgets::SliderRange range{};
    range.minimum = std::min(min_value, max_value);
    range.maximum = std::max(min_value, max_value);
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }
    range.step = std::max(step, 0.0f);
    return range;
}

auto clamp_slider_value(float value,
                        SP::UI::Builders::Widgets::SliderRange const& range) -> float {
    float clamped = std::clamp(value, range.minimum, range.maximum);
    if (range.step > 0.0f) {
        float steps = std::round((clamped - range.minimum) / range.step);
        clamped = range.minimum + steps * range.step;
        clamped = std::clamp(clamped, range.minimum, range.maximum);
    }
    return clamped;
}

struct FragmentBuilder {
    WidgetFragment fragment;

    template <typename Fn>
    FragmentBuilder(std::string kind, Fn&& fn) {
        fragment.kind = std::move(kind);
        fragment.populate = std::forward<Fn>(fn);
    }

    auto with_children(std::vector<std::pair<std::string, WidgetFragment>> children)
        -> FragmentBuilder&& {
        fragment.children = std::move(children);
        return std::move(*this);
    }

    auto build() && -> WidgetFragment {
        return std::move(fragment);
    }
};

auto sanitize_list_items(std::vector<List::ListItem> items) -> std::vector<List::ListItem> {
    for (auto& item : items) {
        if (item.id.empty()) {
            item.id = item.label;
        }
    }
    return items;
}

} // namespace

auto MountFragment(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   WidgetFragment const& fragment,
                   MountOptions const& options) -> SP::Expected<WidgetPath> {
    if (auto status = ensure_widget_name(name); !status) {
        return std::unexpected(status.error());
    }
    auto base = mount_base(parent.getPath(), options);
    auto root = make_path(base, name);

    if (auto status = write_kind(space, root, fragment.kind); !status) {
        return std::unexpected(status.error());
    }

    FragmentContext ctx{space, root};
    if (fragment.populate) {
        if (auto populated = fragment.populate(ctx); !populated) {
            return std::unexpected(populated.error());
        }
    }

    for (auto const& child : fragment.children) {
        if (auto mounted = MountFragment(space,
                                         SP::App::ConcretePathView{root},
                                         child.first,
                                         child.second,
                                         MountOptions{.policy = MountPolicy::WidgetChildren});
            !mounted) {
            return std::unexpected(mounted.error());
        }
    }

    return WidgetPath{root};
}

auto Widgets::Mount(PathSpace& space,
                    SP::App::ConcretePathView parent,
                    std::string_view name,
                    WidgetFragment const& fragment,
                    MountOptions const& options) -> SP::Expected<WidgetPath> {
    return MountFragment(space, parent, name, fragment, options);
}

auto Remove(PathSpace& space, WidgetPath const& widget) -> SP::Expected<void> {
    auto removed_path = std::string(widget.getPath()) + "/state/removed";
    if (auto status = write_value(space, removed_path, true); !status) {
        return status;
    }
    CallbackRegistry::instance().erase_prefix(widget.getPath());
    return {};
}

auto Move(PathSpace&,
          WidgetPath const&,
          SP::App::ConcretePathView,
          std::string_view,
          MountOptions const&) -> SP::Expected<WidgetPath> {
    return std::unexpected(make_error("Declarative widget move is not implemented yet",
                                      SP::Error::Code::NotSupported));
}

namespace Button {

auto Fragment(Args args) -> WidgetFragment {
    args.style = sanitize_button_style(args.style);
    auto children = std::move(args.children);
    return FragmentBuilder{"button",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               BuilderWidgets::ButtonState state{};
                               state.enabled = args.enabled;
                               if (auto status = write_state(ctx.space, ctx.root, state); !status) {
                                   return status;
                               }
                               if (auto status = write_style(ctx.space, ctx.root, args.style); !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/label",
                                                             args.label);
                                   !status) {
                                   return status;
                               }
                               if (args.theme) {
                                   if (auto status = write_value(ctx.space,
                                                                 ctx.root + "/style/theme",
                                                                 *args.theme);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (args.on_press) {
                                   HandlerVariant handler = ButtonHandler{*args.on_press};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "press",
                                                                   HandlerKind::ButtonPress,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Button);
                                   !status) {
                                   return status;
                               }
                               return SP::Expected<void>{};
                           }}
        .with_children(std::move(children))
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetLabel(PathSpace& space,
              WidgetPath const& widget,
              std::string_view label) -> SP::Expected<void> {
    if (auto status = write_value(space, widget.getPath() + "/meta/label", std::string(label));
        !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

auto SetEnabled(PathSpace& space,
                WidgetPath const& widget,
                bool enabled) -> SP::Expected<void> {
    auto state = space.read<BuilderWidgets::ButtonState, std::string>(widget.getPath() + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->enabled == enabled) {
        return {};
    }
    state->enabled = enabled;
    if (auto status = write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Button

namespace Toggle {

auto Fragment(Args args) -> WidgetFragment {
    auto children = std::move(args.children);
    return FragmentBuilder{"toggle",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               BuilderWidgets::ToggleState state{};
                               state.enabled = args.enabled;
                               state.checked = args.checked;
                               if (auto status = write_state(ctx.space, ctx.root, state); !status) {
                                   return status;
                               }
                               if (auto status = write_style(ctx.space, ctx.root, args.style); !status) {
                                   return status;
                               }
                               if (args.on_toggle) {
                                   HandlerVariant handler = ToggleHandler{*args.on_toggle};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "toggle",
                                                                   HandlerKind::Toggle,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Toggle);
                                   !status) {
                                   return status;
                               }
                               return SP::Expected<void>{};
                           }}
        .with_children(std::move(children))
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetChecked(PathSpace& space,
                WidgetPath const& widget,
                bool checked) -> SP::Expected<void> {
    auto state = space.read<BuilderWidgets::ToggleState, std::string>(widget.getPath() + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->checked == checked) {
        return {};
    }
    state->checked = checked;
    if (auto status = write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Toggle

namespace Slider {

auto Fragment(Args args) -> WidgetFragment {
    auto range = sanitize_slider_range(args.minimum, args.maximum, args.step);
    args.style.width = std::max(args.style.width, 32.0f);
    args.style.height = std::max(args.style.height, 16.0f);
    args.style.track_height = std::clamp(args.style.track_height, 1.0f, args.style.height);
    args.style.thumb_radius =
        std::clamp(args.style.thumb_radius, args.style.track_height * 0.5f, args.style.height * 0.5f);
    auto clamped_value = clamp_slider_value(args.value, range);

    return FragmentBuilder{"slider",
                           [args = std::move(args),
                            range,
                            clamped_value](FragmentContext const& ctx) -> SP::Expected<void> {
                               BuilderWidgets::SliderState state{};
                               state.enabled = args.enabled;
                               state.value = clamped_value;
                               if (auto status = write_state(ctx.space, ctx.root, state); !status) {
                                   return status;
                               }
                               if (auto status = write_style(ctx.space, ctx.root, args.style); !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/range",
                                                             range);
                                   !status) {
                                   return status;
                               }
                               if (args.on_change) {
                                   HandlerVariant handler = SliderHandler{*args.on_change};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "change",
                                                                   HandlerKind::Slider,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Slider);
                                   !status) {
                                   return status;
                               }
                               return {};
                           }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetValue(PathSpace& space,
              WidgetPath const& widget,
              float value) -> SP::Expected<void> {
    auto range = space.read<BuilderWidgets::SliderRange, std::string>(widget.getPath() + "/meta/range");
    if (!range) {
        return std::unexpected(range.error());
    }
    auto clamped = clamp_slider_value(value, *range);
    auto state = space.read<BuilderWidgets::SliderState, std::string>(widget.getPath() + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->value == clamped) {
        return {};
    }
    state->value = clamped;
    if (auto status = write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Slider

namespace Label {

auto Fragment(Args args) -> WidgetFragment {
    return FragmentBuilder{"label",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/text",
                                                             args.text);
                                   !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/typography",
                                                             args.typography);
                                   !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/color",
                                                             args.color);
                                   !status) {
                                   return status;
                               }
                               if (args.on_activate) {
                                   HandlerVariant handler = LabelHandler{*args.on_activate};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "activate",
                                                                   HandlerKind::LabelActivate,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Label);
                                   !status) {
                                   return status;
                               }
                               return {};
                           }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetText(PathSpace& space,
             WidgetPath const& widget,
             std::string_view text) -> SP::Expected<void> {
    if (auto status = write_value(space, widget.getPath() + "/state/text", std::string(text));
        !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Label

namespace List {

auto Fragment(Args args) -> WidgetFragment {
    auto items = sanitize_list_items(std::move(args.items));
    auto children = std::move(args.children);
    return FragmentBuilder{"list",
                           [args = std::move(args),
                            items = std::move(items)](FragmentContext const& ctx) -> SP::Expected<void> {
                               BuilderWidgets::ListState state{};
                               if (auto status = write_state(ctx.space, ctx.root, state); !status) {
                                   return status;
                               }
                               if (auto status = write_style(ctx.space, ctx.root, args.style); !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/items",
                                                             items);
                                   !status) {
                                   return status;
                               }
                               if (args.on_child_event) {
                                   HandlerVariant handler = ListChildHandler{*args.on_child_event};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "child_event",
                                                                   HandlerKind::ListChild,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::List);
                                   !status) {
                                   return status;
                               }
                               return SP::Expected<void>{};
                           }}
        .with_children(std::move(children))
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetItems(PathSpace& space,
              WidgetPath const& widget,
              std::vector<ListItem> items) -> SP::Expected<void> {
    items = sanitize_list_items(std::move(items));
    if (auto status =
            write_value(space, widget.getPath() + "/meta/items", std::move(items)); !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace List

namespace Tree {

auto Fragment(Args args) -> WidgetFragment {
    return FragmentBuilder{"tree",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               BuilderWidgets::TreeState state{};
                               if (auto status = write_state(ctx.space, ctx.root, state); !status) {
                                   return status;
                               }
                               if (auto status = write_style(ctx.space, ctx.root, args.style); !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/meta/nodes",
                                                             args.nodes);
                                   !status) {
                                   return status;
                               }
                               if (args.on_node_event) {
                                   HandlerVariant handler = TreeNodeHandler{*args.on_node_event};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "node_event",
                                                                   HandlerKind::TreeNode,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Tree);
                                   !status) {
                                   return status;
                               }
                               return {};
                           }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetNodes(PathSpace& space,
              WidgetPath const& widget,
              std::vector<TreeNode> nodes) -> SP::Expected<void> {
    if (auto status =
            write_value(space, widget.getPath() + "/meta/nodes", std::move(nodes)); !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace Tree

namespace Stack {

auto Fragment(Args args) -> WidgetFragment {
    std::vector<std::string> panel_ids;
    panel_ids.reserve(args.panels.size());
    for (auto const& panel : args.panels) {
        panel_ids.push_back(panel.id);
    }
    std::vector<std::pair<std::string, WidgetFragment>> child_fragments;
    child_fragments.reserve(args.panels.size());
    for (auto& panel : args.panels) {
        child_fragments.emplace_back(panel.id, std::move(panel.fragment));
    }

    return FragmentBuilder{"stack",
                           [panel_ids = std::move(panel_ids),
                            active_panel = std::move(args.active_panel),
                            handler = std::move(args.on_select)](FragmentContext const& ctx)
                               -> SP::Expected<void> {
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/active_panel",
                                                             active_panel);
                                   !status) {
                                   return status;
                               }
                               if (handler) {
                                   HandlerVariant stored = StackPanelHandler{*handler};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "panel_select",
                                                                   HandlerKind::StackPanel,
                                                                   stored);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::Stack);
                                   !status) {
                                   return status;
                               }
                               for (auto const& panel_id : panel_ids) {
                                   if (auto status = ensure_child_name(panel_id); !status) {
                                       return status;
                                   }
                                   auto panel_root = ctx.root + "/panels/" + panel_id;
                                   auto target = ctx.root + "/children/" + panel_id;
                                   if (auto write = write_value(ctx.space,
                                                                panel_root + "/target",
                                                                target);
                                       !write) {
                                       return write;
                                   }
                               }
                               return SP::Expected<void>{};
                           }}
        .with_children(std::move(child_fragments))
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetActivePanel(PathSpace& space,
                    WidgetPath const& widget,
                    std::string_view panel_id) -> SP::Expected<void> {
    return write_value(space,
                       widget.getPath() + "/state/active_panel",
                       std::string(panel_id));
}

} // namespace Stack

namespace InputField {

auto Fragment(Args args) -> WidgetFragment {
    return FragmentBuilder{"input_field",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/text",
                                                             args.text);
                                   !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/placeholder",
                                                             args.placeholder);
                                   !status) {
                                   return status;
                               }
                               if (args.on_change) {
                                   HandlerVariant handler = InputFieldHandler{*args.on_change};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "change",
                                                                   HandlerKind::InputChange,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (args.on_submit) {
                                   HandlerVariant handler = InputFieldHandler{*args.on_submit};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "submit",
                                                                   HandlerKind::InputSubmit,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::InputField);
                                   !status) {
                                   return status;
                               }
                               return {};
                           }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetText(PathSpace& space,
             WidgetPath const& widget,
             std::string_view text) -> SP::Expected<void> {
    if (auto status = write_value(space, widget.getPath() + "/state/text", std::string(text));
        !status) {
        return status;
    }
    return write_value(space, widget.getPath() + "/render/dirty", true);
}

} // namespace InputField

namespace PaintSurface {

auto Fragment(Args args) -> WidgetFragment {
    return FragmentBuilder{"paint_surface",
                           [args = std::move(args)](FragmentContext const& ctx) -> SP::Expected<void> {
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/brush/size",
                                                             args.brush_size);
                                   !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/state/brush/color",
                                                             args.brush_color);
                                   !status) {
                                   return status;
                               }
                               if (auto status = write_value(ctx.space,
                                                             ctx.root + "/render/gpu/enabled",
                                                             args.gpu_enabled);
                                   !status) {
                                   return status;
                               }
                               if (args.on_draw) {
                                   HandlerVariant handler = PaintSurfaceHandler{*args.on_draw};
                                   if (auto status = write_handler(ctx.space,
                                                                   ctx.root,
                                                                   "draw",
                                                                   HandlerKind::PaintDraw,
                                                                   handler);
                                       !status) {
                                       return status;
                                   }
                               }
                               if (auto status = initialize_render(ctx.space,
                                                                   ctx.root,
                                                                   WidgetKind::PaintSurface);
                                   !status) {
                                   return status;
                               }
                               return {};
                           }}
        .build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

} // namespace PaintSurface

} // namespace SP::UI::Declarative
