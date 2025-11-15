#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Builders::WidgetPath;

namespace Label {

auto Fragment(Args args) -> WidgetFragment {
    return WidgetDetail::FragmentBuilder{"label",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/text",
                                                                                args.text);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/meta/typography",
                                                                                args.typography);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/meta/color",
                                                                                args.color);
                                               !status) {
                                               return status;
                                           }
                                           if (args.on_activate) {
                                               HandlerVariant handler = LabelHandler{*args.on_activate};
                                               if (auto status = WidgetDetail::write_handler(ctx.space,
                                                                                      ctx.root,
                                                                                      "activate",
                                                                                      HandlerKind::LabelActivate,
                                                                                      std::move(handler));
                                                   !status) {
                                                   return status;
                                               }
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Label);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
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
    if (auto status = WidgetDetail::write_value(space,
                                          widget.getPath() + "/state/text",
                                          std::string(text));
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace Label

} // namespace SP::UI::Declarative
