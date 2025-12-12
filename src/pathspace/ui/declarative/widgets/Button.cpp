#include "Common.hpp"

#include <algorithm>
#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::WidgetPath;

namespace {

auto sanitize_button_style(BuilderWidgets::ButtonStyle style)
    -> BuilderWidgets::ButtonStyle {
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

} // namespace

namespace Button {

auto Fragment(Args args) -> WidgetFragment {
    args.style = sanitize_button_style(args.style);
    auto children = std::move(args.children);
    auto on_press = std::move(args.on_press);
    bool has_press_handler = static_cast<bool>(on_press);
    auto builder = WidgetDetail::FragmentBuilder{"button",
                                   [args = std::move(args), has_press_handler](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           BuilderWidgets::ButtonState state{};
                                           state.enabled = args.enabled;
                                           if (auto status = WidgetDetail::write_state(ctx.space,
                                                                                 ctx.root,
                                                                                 state);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_style(ctx.space,
                                                                                 ctx.root,
                                                                                 args.style);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/meta/label"),
                                                                                args.label);
                                               !status) {
                                               return status;
                                           }
                                           if (args.theme) {
                                               if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                    WidgetSpacePath(ctx.root,
                                                                                                "/style/theme"),
                                                                                    *args.theme);
                                                   !status) {
                                                   return status;
                                               }
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Button);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::mirror_button_capsule(ctx.space,
                                                                                          ctx.root,
                                                                                          state,
                                                                                          args.style,
                                                                                          args.label,
                                                                                          has_press_handler);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .with_children(std::move(children));

    if (on_press) {
        HandlerVariant handler = ButtonHandler{std::move(*on_press)};
        builder.with_handler("press", HandlerKind::ButtonPress, std::move(handler));
    }

    return builder.build();
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
    if (auto status = WidgetDetail::write_value(space,
                                          WidgetSpacePath(widget.getPath(), "/meta/label"),
                                          std::string(label));
        !status) {
        return status;
    }
    if (auto status = WidgetDetail::update_button_capsule_label(space,
                                                                widget.getPath(),
                                                                std::string(label));
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

auto SetEnabled(PathSpace& space,
                WidgetPath const& widget,
                bool enabled) -> SP::Expected<void> {
    auto state = space.read<BuilderWidgets::ButtonState, std::string>(
        WidgetSpacePath(widget.getPath(), "/state"));
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->enabled == enabled) {
        return {};
    }
    state->enabled = enabled;
    if (auto status = WidgetDetail::write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    if (auto status = WidgetDetail::update_button_capsule_state(space, widget.getPath(), *state);
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace Button

} // namespace SP::UI::Declarative
