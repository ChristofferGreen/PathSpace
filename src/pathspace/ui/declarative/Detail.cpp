#include <pathspace/ui/declarative/Detail.hpp>

#include "../RuntimeDetail.hpp"
#include "../WidgetDetail.hpp"

namespace SP::UI::Declarative::Detail {

auto write_stack_metadata(PathSpace& space,
                          std::string const& rootPath,
                          Widgets::StackLayoutStyle const& style,
                          std::vector<Widgets::StackChildSpec> const& children,
                          Widgets::StackLayoutState const& layout) -> SP::Expected<void> {
    return SP::UI::Runtime::Detail::write_stack_metadata(space, rootPath, style, children, layout);
}

auto compute_stack_layout_state(PathSpace& space,
                                Widgets::StackLayoutParams const& params)
    -> SP::Expected<Widgets::StackLayoutState> {
    auto computed = SP::UI::Runtime::Detail::compute_stack(space, params);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    return computed->first.state;
}

void translate_bucket(SP::UI::Scene::DrawableBucketSnapshot& bucket, float x, float y) {
    SP::UI::Runtime::Detail::translate_bucket(bucket, x, y);
}

void append_bucket(SP::UI::Scene::DrawableBucketSnapshot& target,
                   SP::UI::Scene::DrawableBucketSnapshot const& source) {
    SP::UI::Runtime::Detail::append_bucket(target, source);
}

auto build_text_field_bucket(Widgets::TextFieldStyle const& style,
                             Widgets::TextFieldState const& state,
                             std::string_view authoring_root,
                             bool pulsing_highlight)
    -> SP::UI::Scene::DrawableBucketSnapshot {
    return SP::UI::Runtime::Detail::build_text_field_bucket(style,
                                                             state,
                                                             authoring_root,
                                                             pulsing_highlight);
}

auto build_text_area_bucket(Widgets::TextAreaStyle const& style,
                            Widgets::TextAreaState const& state,
                            std::string_view authoring_root,
                            bool pulsing_highlight)
    -> SP::UI::Scene::DrawableBucketSnapshot {
    return SP::UI::Runtime::Detail::build_text_area_bucket(style,
                                                            state,
                                                            authoring_root,
                                                            pulsing_highlight);
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surface,
                                    std::optional<RenderSettings> const& settings_override)
    -> SP::Expected<SurfaceRenderContext> {
    auto builder_context = SP::UI::Runtime::Detail::prepare_surface_render_context(space,
                                                                                   surface,
                                                                                   settings_override);
    if (!builder_context) {
        return std::unexpected(builder_context.error());
    }
    SurfaceRenderContext context{
        .target_path = builder_context->target_path,
        .renderer_path = builder_context->renderer_path,
        .target_desc = builder_context->target_desc,
        .settings = builder_context->settings,
        .renderer_kind = builder_context->renderer_kind,
    };
    return context;
}

auto acquire_surface(std::string const& key,
                     SurfaceDesc const& desc) -> SP::UI::PathSurfaceSoftware& {
    return SP::UI::Runtime::Detail::acquire_surface(key, desc);
}

#if PATHSPACE_UI_METAL
auto acquire_metal_surface(std::string const& key,
                           SurfaceDesc const& desc) -> SP::UI::PathSurfaceMetal& {
    return SP::UI::Runtime::Detail::acquire_metal_surface(key, desc);
}
#endif

auto render_into_target(PathSpace& space,
                        SurfaceRenderContext const& context,
                        SP::UI::PathSurfaceSoftware& surface
#if PATHSPACE_UI_METAL
                        ,
                        SP::UI::PathSurfaceMetal* metal_surface
#endif
                        ) -> SP::Expected<SP::UI::PathRenderer2D::RenderStats> {
    SP::UI::Runtime::Detail::SurfaceRenderContext builder_context{
        .target_path = context.target_path,
        .renderer_path = context.renderer_path,
        .target_desc = context.target_desc,
        .settings = context.settings,
        .renderer_kind = context.renderer_kind,
    };
    return SP::UI::Runtime::Detail::render_into_target(space,
                                                        builder_context,
                                                        surface
#if PATHSPACE_UI_METAL
                                                        ,
                                                        metal_surface
#endif
                                                        );
}

void invoke_before_present_hook(SP::UI::PathSurfaceSoftware& surface,
                                SP::UI::PathWindowView::PresentPolicy& policy,
                                std::vector<std::size_t>& dirty_tiles) {
    SP::UI::Runtime::Detail::invoke_before_present_hook(surface, policy, dirty_tiles);
}

auto renderer_kind_to_string(RendererKind kind) -> std::string {
    return SP::UI::Runtime::Detail::renderer_kind_to_string(kind);
}

auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& target_path,
                                SP::UI::PathWindowView::PresentStats const& stats,
                                SP::UI::PathWindowView::PresentPolicy const& policy)
    -> SP::Expected<bool> {
    return SP::UI::Runtime::maybe_schedule_auto_render(space, target_path, stats, policy);
}

auto write_present_metrics(PathSpace& space,
                           SP::App::ConcretePathView target_path,
                           SP::UI::PathWindowPresentStats const& stats,
                           SP::UI::PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    return SP::UI::Runtime::Diagnostics::WritePresentMetrics(space, target_path, stats, policy);
}

auto write_window_present_metrics(PathSpace& space,
                                  SP::App::ConcretePathView window_path,
                                  std::string_view view_name,
                                  SP::UI::PathWindowPresentStats const& stats,
                                  SP::UI::PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    return SP::UI::Runtime::Diagnostics::WriteWindowPresentMetrics(space, window_path, view_name, stats, policy);
}

auto write_residency_metrics(PathSpace& space,
                             SP::App::ConcretePathView target_path,
                             std::uint64_t cpu_bytes,
                             std::uint64_t gpu_bytes,
                             std::uint64_t cpu_soft_bytes,
                             std::uint64_t cpu_hard_bytes,
                             std::uint64_t gpu_soft_bytes,
                             std::uint64_t gpu_hard_bytes) -> SP::Expected<void> {
    return SP::UI::Runtime::Diagnostics::WriteResidencyMetrics(space,
                                                               target_path,
                                                               cpu_bytes,
                                                               gpu_bytes,
                                                               cpu_soft_bytes,
                                                               cpu_hard_bytes,
                                                               gpu_soft_bytes,
                                                               gpu_hard_bytes);
}

auto submit_dirty_rects(PathSpace& space,
                        SP::App::ConcretePathView target_path,
                        std::span<DirtyRectHint const> rects) -> SP::Expected<void> {
    return SP::UI::Runtime::Renderer::SubmitDirtyRects(space, target_path, rects);
}

} // namespace SP::UI::Declarative::Detail
