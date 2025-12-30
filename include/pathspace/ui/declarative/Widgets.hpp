#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace SP::UI::Declarative {

namespace BuilderWidgets = SP::UI::Runtime::Widgets;

struct FragmentContext {
    PathSpace& space;
    std::string root;
};

struct WidgetFragment;

enum class WidgetKind : std::uint8_t {
    Button,
    Toggle,
    Slider,
    List,
    Tree,
    Stack,
    Label,
    TextArea,
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
                   MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

auto Remove(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget) -> SP::Expected<void>;

auto Move(PathSpace& space,
          SP::UI::Runtime::WidgetPath const& widget,
          SP::App::ConcretePathView new_parent,
          std::string_view new_name,
          MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

struct WidgetContext {
    WidgetContext(PathSpace& space_in, SP::UI::Runtime::WidgetPath widget_in)
        : space(space_in)
        , widget(std::move(widget_in)) {}

    PathSpace& space;
    SP::UI::Runtime::WidgetPath widget;
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

struct FragmentHandler {
    std::string event;
    HandlerKind kind = HandlerKind::None;
    HandlerVariant handler;
};

struct WidgetFragment {
    std::string kind;
    std::function<SP::Expected<void>(FragmentContext const&)> populate;
    std::vector<std::pair<std::string, WidgetFragment>> children;
    std::vector<FragmentHandler> handlers;
    std::function<SP::Expected<void>(FragmentContext const&)> finalize;
};

struct HandlerOverrideToken {
    std::string widget_path;
    std::string event;
    HandlerKind kind = HandlerKind::None;
    bool had_previous = false;
    std::optional<HandlerVariant> previous_handler;
};

namespace Handlers {

using HandlerTransformer = std::function<HandlerVariant(HandlerVariant const&)>;

auto Read(PathSpace& space,
          SP::UI::Runtime::WidgetPath const& widget,
          std::string_view event) -> SP::Expected<std::optional<HandlerVariant>>;

auto Replace(PathSpace& space,
             SP::UI::Runtime::WidgetPath const& widget,
             std::string_view event,
             HandlerKind kind,
             HandlerVariant handler) -> SP::Expected<HandlerOverrideToken>;

auto Wrap(PathSpace& space,
          SP::UI::Runtime::WidgetPath const& widget,
          std::string_view event,
          HandlerKind kind,
          HandlerTransformer const& transformer) -> SP::Expected<HandlerOverrideToken>;

auto Restore(PathSpace& space, HandlerOverrideToken const& token) -> SP::Expected<void>;

} // namespace Handlers

namespace Button {

struct Args {
    std::string label = "Button";
    bool enabled = true;
    SP::UI::Runtime::Widgets::ButtonStyle style{};
    std::optional<std::string> theme;
    std::optional<ButtonHandler> on_press;
    std::vector<std::pair<std::string, WidgetFragment>> children;

    struct StyleOverrides {
        BuilderWidgets::ButtonStyle* target = nullptr;

        auto background_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->background_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ButtonStyleOverrideField::BackgroundColor);
            }
            return *this;
        }

        auto text_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->text_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ButtonStyleOverrideField::TextColor);
            }
            return *this;
        }

        auto typography(BuilderWidgets::TypographyStyle typography) -> StyleOverrides& {
            if (target) {
                target->typography = std::move(typography);
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ButtonStyleOverrideField::Typography);
            }
            return *this;
        }
    };

    [[nodiscard]] auto style_override() -> StyleOverrides {
        return StyleOverrides{&style};
    }
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

template <typename Label,
          typename = std::enable_if_t<std::is_convertible_v<Label, std::string_view>>>
inline auto Create(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   Label&& label,
                   ButtonHandler handler = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    Args args{};
    args.label = std::string(std::string_view{label});
    if (handler) {
        args.on_press = std::move(handler);
    }
    return Create(space, parent, name, std::move(args));
}
auto SetLabel(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, std::string_view label)
    -> SP::Expected<void>;
auto SetEnabled(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, bool enabled)
    -> SP::Expected<void>;

} // namespace Button

namespace Toggle {

struct Args {
    bool enabled = true;
    bool checked = false;
    SP::UI::Runtime::Widgets::ToggleStyle style{};
    std::optional<ToggleHandler> on_toggle;
    std::vector<std::pair<std::string, WidgetFragment>> children;

    struct StyleOverrides {
        BuilderWidgets::ToggleStyle* target = nullptr;

        auto track_off(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->track_off_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ToggleStyleOverrideField::TrackOff);
            }
            return *this;
        }

        auto track_on(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->track_on_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ToggleStyleOverrideField::TrackOn);
            }
            return *this;
        }

        auto thumb(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->thumb_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ToggleStyleOverrideField::Thumb);
            }
            return *this;
        }
    };

    [[nodiscard]] auto style_override() -> StyleOverrides {
        return StyleOverrides{&style};
    }
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetChecked(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, bool checked)
    -> SP::Expected<void>;

} // namespace Toggle

namespace Slider {

struct Args {
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.5f;
    float step = 0.0f;
    bool enabled = true;
    SP::UI::Runtime::Widgets::SliderStyle style{};
    std::optional<SliderHandler> on_change;

    struct StyleOverrides {
        BuilderWidgets::SliderStyle* target = nullptr;

        auto track_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->track_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::SliderStyleOverrideField::Track);
            }
            return *this;
        }

        auto fill_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->fill_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::SliderStyleOverrideField::Fill);
            }
            return *this;
        }

        auto thumb_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->thumb_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::SliderStyleOverrideField::Thumb);
            }
            return *this;
        }

        auto label_color(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->label_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::SliderStyleOverrideField::LabelColor);
            }
            return *this;
        }

        auto label_typography(BuilderWidgets::TypographyStyle typography) -> StyleOverrides& {
            if (target) {
                target->label_typography = std::move(typography);
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::SliderStyleOverrideField::LabelTypography);
            }
            return *this;
        }
    };

    [[nodiscard]] auto style_override() -> StyleOverrides {
        return StyleOverrides{&style};
    }
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetValue(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, float value)
    -> SP::Expected<void>;

} // namespace Slider

namespace Label {

struct Args {
    std::string text;
    SP::UI::Runtime::Widgets::TypographyStyle typography{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<LabelHandler> on_activate;
};

auto Fragment(Args args) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

template <typename Text,
          typename = std::enable_if_t<std::is_convertible_v<Text, std::string_view>>>
inline auto Create(PathSpace& space,
                   SP::App::ConcretePathView parent,
                   std::string_view name,
                   Text&& text) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    Args args{};
    args.text = std::string(std::string_view{text});
    return Create(space, parent, name, std::move(args));
}
auto SetText(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, std::string_view text)
    -> SP::Expected<void>;

} // namespace Label

namespace List {

using ListItem = SP::UI::Runtime::Widgets::ListItem;

struct Args {
    std::vector<ListItem> items;
    SP::UI::Runtime::Widgets::ListStyle style{};
    std::optional<ListChildHandler> on_child_event;
    std::vector<std::pair<std::string, WidgetFragment>> children;

    struct StyleOverrides {
        BuilderWidgets::ListStyle* target = nullptr;

        auto background(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->background_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::Background);
            }
            return *this;
        }

        auto border(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->border_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::Border);
            }
            return *this;
        }

        auto item(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->item_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::Item);
            }
            return *this;
        }

        auto item_hover(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->item_hover_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::ItemHover);
            }
            return *this;
        }

        auto item_selected(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->item_selected_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::ItemSelected);
            }
            return *this;
        }

        auto separator(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->separator_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::Separator);
            }
            return *this;
        }

        auto item_text(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->item_text_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::ItemText);
            }
            return *this;
        }

        auto item_typography(BuilderWidgets::TypographyStyle typography) -> StyleOverrides& {
            if (target) {
                target->item_typography = std::move(typography);
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::ListStyleOverrideField::ItemTypography);
            }
            return *this;
        }
    };

    [[nodiscard]] auto style_override() -> StyleOverrides {
        return StyleOverrides{&style};
    }
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetItems(PathSpace& space,
              SP::UI::Runtime::WidgetPath const& widget,
              std::vector<ListItem> items) -> SP::Expected<void>;

} // namespace List

namespace Tree {

using TreeNode = SP::UI::Runtime::Widgets::TreeNode;

struct Args {
    std::vector<TreeNode> nodes;
    SP::UI::Runtime::Widgets::TreeStyle style{};
    std::optional<TreeNodeHandler> on_node_event;

    struct StyleOverrides {
        BuilderWidgets::TreeStyle* target = nullptr;

        auto background(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->background_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Background);
            }
            return *this;
        }

        auto border(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->border_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Border);
            }
            return *this;
        }

        auto row(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->row_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Row);
            }
            return *this;
        }

        auto row_hover(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->row_hover_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::RowHover);
            }
            return *this;
        }

        auto row_selected(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->row_selected_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::RowSelected);
            }
            return *this;
        }

        auto row_disabled(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->row_disabled_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::RowDisabled);
            }
            return *this;
        }

        auto connector(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->connector_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Connector);
            }
            return *this;
        }

        auto toggle(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->toggle_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Toggle);
            }
            return *this;
        }

        auto text(std::array<float, 4> color) -> StyleOverrides& {
            if (target) {
                target->text_color = color;
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::Text);
            }
            return *this;
        }

        auto label_typography(BuilderWidgets::TypographyStyle typography) -> StyleOverrides& {
            if (target) {
                target->label_typography = std::move(typography);
                BuilderWidgets::SetStyleOverride(target->overrides,
                                                 BuilderWidgets::TreeStyleOverrideField::LabelTypography);
            }
            return *this;
        }
    };

    [[nodiscard]] auto style_override() -> StyleOverrides {
        return StyleOverrides{&style};
    }
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetNodes(PathSpace& space,
              SP::UI::Runtime::WidgetPath const& widget,
              std::vector<TreeNode> nodes) -> SP::Expected<void>;

} // namespace Tree

namespace Stack {

struct Panel {
    std::string id;
    WidgetFragment fragment;
    BuilderWidgets::StackChildConstraints constraints{};
};

struct Args {
    std::vector<Panel> panels;
    std::string active_panel;
    BuilderWidgets::StackLayoutStyle style{};
    std::optional<StackPanelHandler> on_select;
};

auto Fragment(Args args) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetActivePanel(PathSpace& space,
                    SP::UI::Runtime::WidgetPath const& widget,
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
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;
auto SetText(PathSpace& space, SP::UI::Runtime::WidgetPath const& widget, std::string_view text)
    -> SP::Expected<void>;

} // namespace InputField

namespace PaintSurface {

struct Args {
    float brush_size = 6.0f;
    std::array<float, 4> brush_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool gpu_enabled = false;
    std::uint32_t buffer_width = 512;
    std::uint32_t buffer_height = 512;
    float buffer_dpi = 96.0f;
    std::optional<PaintSurfaceHandler> on_draw;
};

auto Fragment(Args args = {}) -> WidgetFragment;
auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args = {},
            MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

} // namespace PaintSurface

namespace Widgets {

auto Mount(PathSpace& space,
           SP::App::ConcretePathView parent,
           std::string_view name,
           WidgetFragment const& fragment,
           MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath>;

inline auto Move(PathSpace& space,
                 SP::UI::Runtime::WidgetPath const& widget,
                 SP::App::ConcretePathView new_parent,
                 std::string_view new_name,
                 MountOptions const& options = {}) -> SP::Expected<SP::UI::Runtime::WidgetPath> {
    return SP::UI::Declarative::Move(space, widget, new_parent, new_name, options);
}

} // namespace Widgets

} // namespace SP::UI::Declarative
