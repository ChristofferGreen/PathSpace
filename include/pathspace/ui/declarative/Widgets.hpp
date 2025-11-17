#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace SP::UI::Declarative {

struct FragmentContext {
    PathSpace& space;
    std::string root;
};

struct WidgetFragment;

struct WidgetFragment {
    std::string kind;
    std::function<SP::Expected<void>(FragmentContext const&)> populate;
    std::vector<std::pair<std::string, WidgetFragment>> children;
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

enum class MountPolicy {
    Auto,
    WindowWidgets,
    WidgetChildren,
};

struct MountOptions {
    MountPolicy policy = MountPolicy::Auto;
    std::string slot_override;
};

auto MountFragment(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   WidgetFragment const& fragment,
                   MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;

auto Remove(PathSpace& space, SP::UI::Builders::WidgetPath const& widget) -> SP::Expected<void>;

auto Move(PathSpace& space,
          SP::UI::Builders::WidgetPath const& widget,
          SP::App::ConcretePathView new_parent,
          std::string_view new_name,
          MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;

struct WidgetContext {
    WidgetContext(PathSpace& space_in, SP::UI::Builders::WidgetPath widget_in)
        : space(space_in)
        , widget(std::move(widget_in)) {}

    PathSpace& space;
    SP::UI::Builders::WidgetPath widget;
};

struct ButtonContext : WidgetContext {
    using WidgetContext::WidgetContext;
};

struct ToggleContext : WidgetContext {
    using WidgetContext::WidgetContext;
};

struct SliderContext : WidgetContext {
    using WidgetContext::WidgetContext;
    float value = 0.0f;
};

struct ListChildContext : WidgetContext {
    using WidgetContext::WidgetContext;
    std::string child_id;
};

struct TreeNodeContext : WidgetContext {
    using WidgetContext::WidgetContext;
    std::string node_id;
};

struct StackPanelContext : WidgetContext {
    using WidgetContext::WidgetContext;
    std::string panel_id;
};

struct LabelContext : WidgetContext {
    using WidgetContext::WidgetContext;
};

struct InputFieldContext : WidgetContext {
    using WidgetContext::WidgetContext;
};

struct PaintSurfaceContext : WidgetContext {
    using WidgetContext::WidgetContext;
};

using ButtonHandler = std::function<void(ButtonContext&)>;
using ToggleHandler = std::function<void(ToggleContext&)>;
using SliderHandler = std::function<void(SliderContext&)>;
using ListChildHandler = std::function<void(ListChildContext&)>;
using TreeNodeHandler = std::function<void(TreeNodeContext&)>;
using StackPanelHandler = std::function<void(StackPanelContext&)>;
using LabelHandler = std::function<void(LabelContext&)>;
using InputFieldHandler = std::function<void(InputFieldContext&)>;
using PaintSurfaceHandler = std::function<void(PaintSurfaceContext&)>;

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

namespace Button {

struct Args {
    std::string label = "Button";
    bool enabled = true;
    SP::UI::Builders::Widgets::ButtonStyle style{};
    std::optional<std::string> theme;
    std::optional<ButtonHandler> on_press;
    std::vector<std::pair<std::string, WidgetFragment>> children;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetLabel(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, std::string_view label)
    -> SP::Expected<void>;
auto SetEnabled(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, bool enabled)
    -> SP::Expected<void>;

} // namespace Button

namespace Toggle {

struct Args {
    bool enabled = true;
    bool checked = false;
    SP::UI::Builders::Widgets::ToggleStyle style{};
    std::optional<ToggleHandler> on_toggle;
    std::vector<std::pair<std::string, WidgetFragment>> children;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetChecked(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, bool checked)
    -> SP::Expected<void>;

} // namespace Toggle

namespace Slider {

struct Args {
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.5f;
    float step = 0.0f;
    bool enabled = true;
    SP::UI::Builders::Widgets::SliderStyle style{};
    std::optional<SliderHandler> on_change;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetValue(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, float value)
    -> SP::Expected<void>;

} // namespace Slider

namespace Label {

struct Args {
    std::string text;
    SP::UI::Builders::Widgets::TypographyStyle typography{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<LabelHandler> on_activate;
};

auto Fragment(Args args) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetText(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, std::string_view text)
    -> SP::Expected<void>;

} // namespace Label

namespace List {

using ListItem = SP::UI::Builders::Widgets::ListItem;

struct Args {
    std::vector<ListItem> items;
    SP::UI::Builders::Widgets::ListStyle style{};
    std::optional<ListChildHandler> on_child_event;
    std::vector<std::pair<std::string, WidgetFragment>> children;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetItems(PathSpace& space,
              SP::UI::Builders::WidgetPath const& widget,
              std::vector<ListItem> items) -> SP::Expected<void>;

} // namespace List

namespace Tree {

using TreeNode = SP::UI::Builders::Widgets::TreeNode;

struct Args {
    std::vector<TreeNode> nodes;
    SP::UI::Builders::Widgets::TreeStyle style{};
    std::optional<TreeNodeHandler> on_node_event;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetNodes(PathSpace& space,
              SP::UI::Builders::WidgetPath const& widget,
              std::vector<TreeNode> nodes) -> SP::Expected<void>;

} // namespace Tree

namespace Stack {

struct Panel {
    std::string id;
    WidgetFragment fragment;
};

struct Args {
    std::vector<Panel> panels;
    std::string active_panel;
    std::optional<StackPanelHandler> on_select;
};

auto Fragment(Args args) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetActivePanel(PathSpace& space,
                    SP::UI::Builders::WidgetPath const& widget,
                    std::string_view panel_id) -> SP::Expected<void>;

} // namespace Stack

namespace InputField {

struct Args {
    std::string text;
    std::string placeholder;
    bool focused = false;
    std::optional<InputFieldHandler> on_change;
    std::optional<InputFieldHandler> on_submit;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;
auto SetText(PathSpace& space, SP::UI::Builders::WidgetPath const& widget, std::string_view text)
    -> SP::Expected<void>;

} // namespace InputField

namespace PaintSurface {

struct Args {
    float brush_size = 6.0f;
    std::array<float, 4> brush_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool gpu_enabled = false;
    std::optional<PaintSurfaceHandler> on_draw;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;

} // namespace PaintSurface

namespace Widgets {

auto Mount(PathSpace& space,
           SP::App::ConcretePathView parent,
           std::string_view name,
           WidgetFragment const& fragment,
           MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath>;

inline auto Move(PathSpace& space,
                 SP::UI::Builders::WidgetPath const& widget,
                 SP::App::ConcretePathView new_parent,
                 std::string_view new_name,
                 MountOptions const& options = {}) -> SP::Expected<SP::UI::Builders::WidgetPath> {
    return SP::UI::Declarative::Move(space, widget, new_parent, new_name, options);
}

} // namespace Widgets

} // namespace SP::UI::Declarative
