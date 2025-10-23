#include "BuildersDetail.hpp"

namespace SP::UI::Builders::App {

using namespace Detail;

auto Bootstrap(PathSpace& space,
               AppRootPathView appRoot,
               ScenePath const& scene,
               BootstrapParams const& params) -> SP::Expected<BootstrapResult> {
    if (auto status = ensure_identifier(params.view_name, "view name"); !status) {
        return std::unexpected(status.error());
    }

    auto renderer_params = params.renderer;
    if (renderer_params.description.empty()) {
        renderer_params.description = "bootstrap renderer";
    }
    auto renderer = Renderer::Create(space, appRoot, renderer_params);
    if (!renderer) {
        return std::unexpected(renderer.error());
    }

    auto surface_params = params.surface;
    auto resolve_dimension = [&](int value, int fallback_from_window, int fallback_settings, int fallback_default) {
        if (value > 0) {
            return value;
        }
        if (fallback_from_window > 0) {
            return fallback_from_window;
        }
        if (fallback_settings > 0) {
            return fallback_settings;
        }
        return fallback_default;
    };
    auto settings_width = params.renderer_settings_override
                              ? params.renderer_settings_override->surface.size_px.width
                              : 0;
    auto settings_height = params.renderer_settings_override
                               ? params.renderer_settings_override->surface.size_px.height
                               : 0;
    surface_params.desc.size_px.width = resolve_dimension(surface_params.desc.size_px.width,
                                                          params.window.width,
                                                          settings_width,
                                                          1280);
    surface_params.desc.size_px.height = resolve_dimension(surface_params.desc.size_px.height,
                                                           params.window.height,
                                                           settings_height,
                                                           720);
    surface_params.renderer = renderer->getPath();

    auto surface = Surface::Create(space, appRoot, surface_params);
    if (!surface) {
        return std::unexpected(surface.error());
    }

    if (auto status = Surface::SetScene(space, *surface, scene); !status) {
        return std::unexpected(status.error());
    }

    auto target_relative = read_value<std::string>(space,
                                                   std::string(surface->getPath()) + "/target");
    if (!target_relative) {
        return std::unexpected(target_relative.error());
    }

    auto target_absolute = SP::App::resolve_app_relative(appRoot, *target_relative);
    if (!target_absolute) {
        return std::unexpected(target_absolute.error());
    }

    auto window_params = params.window;
    window_params.width = resolve_dimension(window_params.width,
                                            surface_params.desc.size_px.width,
                                            settings_width,
                                            1280);
    window_params.height = resolve_dimension(window_params.height,
                                             surface_params.desc.size_px.height,
                                             settings_height,
                                             720);
    if (window_params.scale <= 0.0f) {
        window_params.scale = 1.0f;
    }

    auto window = Window::Create(space, appRoot, window_params);
    if (!window) {
        return std::unexpected(window.error());
    }

    if (auto status = Window::AttachSurface(space,
                                            *window,
                                            params.view_name,
                                            *surface); !status) {
        return std::unexpected(status.error());
    }

    auto surface_desc = read_value<SurfaceDesc>(space, std::string(surface->getPath()) + "/desc");
    if (!surface_desc) {
        return std::unexpected(surface_desc.error());
    }
    auto target_desc = read_value<SurfaceDesc>(space,
                                               std::string(target_absolute->getPath()) + "/desc");
    if (!target_desc) {
        return std::unexpected(target_desc.error());
    }

    auto present_policy = params.present_policy;
    auto view_base = std::string(window->getPath()) + "/views/" + params.view_name;
    if (params.configure_present_policy) {
        auto policy_string = present_mode_to_string(present_policy.mode);
        if (auto status = replace_single<std::string>(space,
                                                      view_base + "/present/policy",
                                                      policy_string); !status) {
            return std::unexpected(status.error());
        }

        auto to_ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        present_policy.staleness_budget_ms_value = to_ms(present_policy.staleness_budget);
        present_policy.frame_timeout_ms_value = to_ms(present_policy.frame_timeout);

        auto params_base = view_base + "/present/params";
        if (auto status = replace_single<double>(space,
                                                 params_base + "/staleness_budget_ms",
                                                 present_policy.staleness_budget_ms_value); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<double>(space,
                                                 params_base + "/frame_timeout_ms",
                                                 present_policy.frame_timeout_ms_value); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<uint64_t>(space,
                                                   params_base + "/max_age_frames",
                                                   static_cast<uint64_t>(present_policy.max_age_frames)); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/vsync_align",
                                               present_policy.vsync_align); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/auto_render_on_present",
                                               present_policy.auto_render_on_present); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = replace_single<bool>(space,
                                               params_base + "/capture_framebuffer",
                                               present_policy.capture_framebuffer); !status) {
            return std::unexpected(status.error());
        }
    }

    RenderSettings applied_settings{};
    if (params.configure_renderer_settings) {
        if (params.renderer_settings_override) {
            applied_settings = *params.renderer_settings_override;
        } else {
            applied_settings.clear_color = {0.11f, 0.12f, 0.15f, 1.0f};
        }
        if (applied_settings.surface.size_px.width <= 0) {
            applied_settings.surface.size_px.width = target_desc->size_px.width;
        }
        if (applied_settings.surface.size_px.height <= 0) {
            applied_settings.surface.size_px.height = target_desc->size_px.height;
        }
        if (applied_settings.surface.dpi_scale <= 0.0f) {
            applied_settings.surface.dpi_scale = window_params.scale > 0.0f ? window_params.scale : 1.0f;
        }
        applied_settings.surface.visibility = true;
        applied_settings.renderer.backend_kind = params.renderer.kind;
        applied_settings.renderer.metal_uploads_enabled = (params.renderer.kind == RendererKind::Metal2D);

        auto update = Renderer::UpdateSettings(space,
                                               ConcretePathView{target_absolute->getPath()},
                                               applied_settings);
        if (!update) {
            return std::unexpected(update.error());
        }
    }

    if (params.submit_initial_dirty_rect) {
        DirtyRectHint hint{};
        if (params.initial_dirty_rect_override) {
            hint = ensure_valid_hint(*params.initial_dirty_rect_override);
        } else {
            hint = make_default_dirty_rect(static_cast<float>(target_desc->size_px.width),
                                           static_cast<float>(target_desc->size_px.height));
        }
        if (hint.max_x > hint.min_x && hint.max_y > hint.min_y) {
            std::array<DirtyRectHint, 1> hints{hint};
            if (auto status = Renderer::SubmitDirtyRects(space,
                                                         ConcretePathView{target_absolute->getPath()},
                                                         hints); !status) {
                return std::unexpected(status.error());
            }
        }
    }

    BootstrapResult result{};
    result.renderer = *renderer;
    result.surface = *surface;
    result.window = *window;
    result.view_name = params.view_name;
    result.target = ConcretePath{target_absolute->getPath()};
    result.surface_desc = *target_desc;
    result.present_policy = present_policy;
    result.applied_settings = applied_settings;
    return result;
}

} // namespace SP::UI::Builders::App


