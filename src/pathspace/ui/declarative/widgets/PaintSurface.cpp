#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Builders::WidgetPath;

namespace PaintSurface {

auto Fragment(Args args) -> WidgetFragment {
    return WidgetDetail::FragmentBuilder{"paint_surface",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/brush/size",
                                                                                args.brush_size);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/brush/color",
                                                                                args.brush_color);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/render/gpu/enabled",
                                                                                args.gpu_enabled);
                                               !status) {
                                               return status;
                                           }
                                           if (args.on_draw) {
                                               HandlerVariant handler = PaintSurfaceHandler{*args.on_draw};
                                               if (auto status = WidgetDetail::write_handler(ctx.space,
                                                                                      ctx.root,
                                                                                      "draw",
                                                                                      HandlerKind::PaintDraw,
                                                                                      std::move(handler));
                                                   !status) {
                                                   return status;
                                               }
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::PaintSurface);
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

} // namespace PaintSurface

} // namespace SP::UI::Declarative
