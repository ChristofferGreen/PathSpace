#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Runtime::WidgetPath;

namespace InputField {

auto Fragment(Args args) -> WidgetFragment {
    auto on_change = std::move(args.on_change);
    auto on_submit = std::move(args.on_submit);
    auto builder = WidgetDetail::FragmentBuilder{"input_field",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/text",
                                                                                args.text);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/placeholder",
                                                                                args.placeholder);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::InputField);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        ;

    if (on_change) {
        HandlerVariant handler = InputFieldHandler{std::move(*on_change)};
        builder.with_handler("change", HandlerKind::InputChange, std::move(handler));
    }
    if (on_submit) {
        HandlerVariant handler = InputFieldHandler{std::move(*on_submit)};
        builder.with_handler("submit", HandlerKind::InputSubmit, std::move(handler));
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

auto SetText(PathSpace& space,
             WidgetPath const& widget,
             std::string_view text) -> SP::Expected<void> {
    if (auto status = WidgetDetail::write_value(space,
                                          widget.getPath() + "/state/text",
                                          std::string(text));
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace InputField

} // namespace SP::UI::Declarative
