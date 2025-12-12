#include "Common.hpp"

#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include <string>
#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Runtime::WidgetPath;
using SP::UI::Runtime::Widgets::WidgetSpacePath;

namespace PaintSurface {

auto Fragment(Args args) -> WidgetFragment {
    auto on_draw = std::move(args.on_draw);
    auto builder = WidgetDetail::FragmentBuilder{"paint_surface",
                                   [args = std::move(args)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/state/brush/size"),
                                                                                args.brush_size);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/state/brush/color"),
                                                                                args.brush_color);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/render/gpu/enabled"),
                                                                                args.gpu_enabled);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                std::string(ctx.root)
                                                                                    + "/render/gpu/enabled",
                                                                                args.gpu_enabled);
                                               !status) {
                                               return status;
                                           }
                                           PaintBufferMetrics buffer_defaults{
                                               .width = args.buffer_width,
                                               .height = args.buffer_height,
                                               .dpi = args.buffer_dpi,
                                           };
                                           if (auto status = PaintRuntime::EnsureBufferDefaults(ctx.space,
                                                                                            ctx.root,
                                                                                            buffer_defaults);
                                               !status) {
                                               return status;
                                           }

                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::PaintSurface);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::mirror_paint_surface_capsule(ctx.space,
                                                                                                 ctx.root,
                                                                                                 args.brush_size,
                                                                                                 args.brush_color,
                                                                                                 args.buffer_width,
                                                                                                 args.buffer_height,
                                                                                                 args.buffer_dpi,
                                                                                                 args.gpu_enabled);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        ;

    if (on_draw) {
        HandlerVariant handler = PaintSurfaceHandler{std::move(*on_draw)};
        builder.with_handler("draw", HandlerKind::PaintDraw, std::move(handler));
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

} // namespace PaintSurface

} // namespace SP::UI::Declarative
