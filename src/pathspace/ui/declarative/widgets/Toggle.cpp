#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::WidgetPath;

namespace Toggle {

auto Fragment(Args args) -> WidgetFragment {
    auto children = std::move(args.children);
    auto on_toggle = std::move(args.on_toggle);
    auto builder = WidgetDetail::FragmentBuilder{"toggle",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           BuilderWidgets::ToggleState state{};
                                           state.enabled = args.enabled;
                                           state.checked = args.checked;
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
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Toggle);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .with_children(std::move(children));

    if (on_toggle) {
        HandlerVariant handler = ToggleHandler{std::move(*on_toggle)};
        builder.with_handler("toggle", HandlerKind::Toggle, std::move(handler));
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
    if (auto status = WidgetDetail::write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace Toggle

} // namespace SP::UI::Declarative
