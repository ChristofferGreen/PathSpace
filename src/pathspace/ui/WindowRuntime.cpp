#include "RuntimeDetail.hpp"

namespace SP::UI::Runtime::Window {

using namespace Detail;

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath> {
    if (auto status = ensure_identifier(params.name, "window name"); !status) {
        return std::unexpected(status.error());
    }

    auto windowPath = combine_relative(appRoot, std::string("windows/") + params.name);
    if (!windowPath) {
        return std::unexpected(windowPath.error());
    }

    auto metaBase = std::string(windowPath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return WindowPath{windowPath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/title", params.title); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/width", params.width); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/height", params.height); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<float>(space, metaBase + "/scale", params.scale); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/background", params.background); !status) {
        return std::unexpected(status.error());
    }

    return WindowPath{windowPath->getPath()};
}

auto AttachSurface(PathSpace& space,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    if (auto status = same_app(ConcretePathView{windowPath.getPath()},
                               ConcretePathView{surfacePath.getPath()}); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    auto surfaceRelative = relative_to_root(SP::App::AppRootPathView{windowRoot->getPath()},
                                            ConcretePathView{surfacePath.getPath()});
    if (!surfaceRelative) {
        return std::unexpected(surfaceRelative.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    if (auto status = replace_single<std::string>(space, viewBase + "/surface", *surfaceRelative); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, viewBase + "/htmlTarget", std::string{}); !status) {
        return status;
    }
    (void)drain_queue<std::string>(space, viewBase + "/windowTarget");
    return {};
}

auto AttachHtmlTarget(PathSpace& space,
                       WindowPath const& windowPath,
                       std::string_view viewName,
                       HtmlTargetPath const& targetPath) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    if (auto status = same_app(ConcretePathView{windowPath.getPath()},
                               ConcretePathView{targetPath.getPath()}); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    auto targetRelative = relative_to_root(SP::App::AppRootPathView{windowRoot->getPath()},
                                           ConcretePathView{targetPath.getPath()});
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    // Ensure the target exists by validating the descriptor.
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    if (auto desc = read_optional<HtmlTargetDesc>(space, descPath); !desc) {
        return std::unexpected(desc.error());
    } else if (!desc->has_value()) {
        return std::unexpected(make_error("html target descriptor missing",
                                          SP::Error::Code::InvalidPath));
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    if (auto status = replace_single<std::string>(space, viewBase + "/htmlTarget", *targetRelative); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, viewBase + "/surface", std::string{}); !status) {
        return status;
    }
    (void)drain_queue<std::string>(space, viewBase + "/windowTarget");
    return {};
}

auto Present(PathSpace& space,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<WindowPresentResult> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return std::unexpected(status.error());
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    auto surfaceRel = read_optional<std::string>(space, viewBase + "/surface");
    if (!surfaceRel) {
        return std::unexpected(surfaceRel.error());
    }
    auto htmlRel = read_optional<std::string>(space, viewBase + "/htmlTarget");
    if (!htmlRel) {
        return std::unexpected(htmlRel.error());
    }

    auto surfaceBinding = surfaceRel->has_value() ? **surfaceRel : std::string{};
    auto htmlBinding = htmlRel->has_value() ? **htmlRel : std::string{};
    bool hasSurface = !surfaceBinding.empty();
    bool hasHtml = !htmlBinding.empty();

    if (hasSurface && hasHtml) {
        return std::unexpected(make_error("view is bound to both surface and html target",
                                          SP::Error::Code::InvalidPath));
    }
    if (!hasSurface && !hasHtml) {
        return std::unexpected(make_error("view is not bound to a presentable target",
                                          SP::Error::Code::InvalidPath));
    }

    if (hasHtml) {
        auto htmlPath = SP::App::resolve_app_relative(SP::App::AppRootPathView{windowRoot->getPath()},
                                                      htmlBinding);
        if (!htmlPath) {
            return std::unexpected(htmlPath.error());
        }

        auto html_render_start = std::chrono::steady_clock::now();
        auto renderStatus = Renderer::RenderHtml(space,
                                                 ConcretePathView{htmlPath->getPath()});
        if (!renderStatus) {
            return std::unexpected(renderStatus.error());
        }
        auto html_render_end = std::chrono::steady_clock::now();
        auto render_ms = std::chrono::duration<double, std::milli>(html_render_end - html_render_start).count();

        auto htmlBase = std::string(htmlPath->getPath()) + "/output/v1/html";

        auto revisionValue = read_optional<uint64_t>(space, htmlBase + "/revision");
        if (!revisionValue) {
            return std::unexpected(revisionValue.error());
        }
        uint64_t revision = revisionValue->value_or(0);

        auto read_string_or = [&](std::string const& path) -> SP::Expected<std::string> {
            auto value = read_optional<std::string>(space, path);
            if (!value) {
                return std::unexpected(value.error());
            }
            return value->value_or(std::string{});
        };

        auto domValue = read_string_or(htmlBase + "/dom");
        if (!domValue) {
            return std::unexpected(domValue.error());
        }
        auto cssValue = read_string_or(htmlBase + "/css");
        if (!cssValue) {
            return std::unexpected(cssValue.error());
        }
        auto commandsValue = read_string_or(htmlBase + "/commands");
        if (!commandsValue) {
            return std::unexpected(commandsValue.error());
        }
        auto modeValue = read_string_or(htmlBase + "/mode");
        if (!modeValue) {
            return std::unexpected(modeValue.error());
        }

        auto usedCanvasValue = read_optional<bool>(space, htmlBase + "/usedCanvasFallback");
        if (!usedCanvasValue) {
            return std::unexpected(usedCanvasValue.error());
        }
        bool usedCanvas = usedCanvasValue->value_or(false);

        std::vector<Html::Asset> assets;
        if (auto assetsValue = read_optional<std::vector<Html::Asset>>(space, htmlBase + "/assets"); !assetsValue) {
            return std::unexpected(assetsValue.error());
        } else if (assetsValue->has_value()) {
            assets = std::move(**assetsValue);
        }
        auto commonBase = std::string(htmlPath->getPath()) + "/output/v1/common";
        uint64_t next_frame_index = 1;
        if (auto previousFrame = read_optional<uint64_t>(space, commonBase + "/frameIndex"); !previousFrame) {
            return std::unexpected(previousFrame.error());
        } else if (previousFrame->has_value()) {
            next_frame_index = **previousFrame + 1;
        }

        PathWindowPresentStats presentStats{};
        presentStats.presented = true;
        presentStats.mode = PathWindowPresentMode::AlwaysLatestComplete;
        presentStats.auto_render_on_present = false;
        presentStats.vsync_aligned = false;
        presentStats.backend_kind = "Html";
        presentStats.frame.frame_index = next_frame_index;
        presentStats.frame.revision = revision;
        presentStats.frame.render_ms = render_ms;
        presentStats.present_ms = 0.0;
        presentStats.gpu_encode_ms = 0.0;
        presentStats.gpu_present_ms = 0.0;
        presentStats.wait_budget_ms = 0.0;
        presentStats.frame_age_ms = 0.0;
        presentStats.frame_age_frames = 0;

        PathWindowPresentPolicy htmlPolicy{};
        htmlPolicy.mode = PathWindowPresentMode::AlwaysLatestComplete;
        htmlPolicy.auto_render_on_present = false;
        htmlPolicy.vsync_align = false;
        htmlPolicy.staleness_budget = std::chrono::milliseconds{0};
        htmlPolicy.staleness_budget_ms_value = 0.0;
        htmlPolicy.frame_timeout = std::chrono::milliseconds{0};
        htmlPolicy.frame_timeout_ms_value = 0.0;
        htmlPolicy.max_age_frames = 0;

        WindowPresentResult result{};
        result.stats = presentStats;
        result.html = WindowPresentResult::HtmlPayload{
            .revision = revision,
            .dom = std::move(*domValue),
            .css = std::move(*cssValue),
            .commands = std::move(*commandsValue),
            .mode = std::move(*modeValue),
            .used_canvas_fallback = usedCanvas,
            .assets = std::move(assets),
        };

        if (auto status = Diagnostics::WritePresentMetrics(space,
                                                           ConcretePathView{htmlPath->getPath()},
                                                           presentStats,
                                                           htmlPolicy); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = Diagnostics::WriteResidencyMetrics(space,
                                                             ConcretePathView{htmlPath->getPath()},
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0,
                                                             0); !status) {
            return std::unexpected(status.error());
        }

        if (auto status = Diagnostics::WriteWindowPresentMetrics(space,
                                                                  ConcretePathView{windowPath.getPath()},
                                                                  viewName,
                                                                  presentStats,
                                                                  htmlPolicy); !status) {
            return std::unexpected(status.error());
        }

        return result;
    }

    // Surface-backed present path.
    auto surfacePath = SP::App::resolve_app_relative(SP::App::AppRootPathView{windowRoot->getPath()},
                                                     surfaceBinding);
    if (!surfacePath) {
        return std::unexpected(surfacePath.error());
    }

    auto context = prepare_surface_render_context(space,
                                                  SurfacePath{surfacePath->getPath()},
                                                  std::nullopt);
    if (!context) {
        return std::unexpected(context.error());
    }

    auto policy = read_present_policy(space, viewBase);
    if (!policy) {
        return std::unexpected(policy.error());
    }
    auto presentPolicy = *policy;

    auto target_key = std::string(context->target_path.getPath());
    auto& surface = acquire_surface(target_key, context->target_desc);

#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(target_key, context->target_desc);
    }
#endif

#if PATHSPACE_UI_METAL
    auto renderStats = render_into_target(space,
                                          *context,
                                          surface,
                                          metal_surface);
#else
    auto renderStats = render_into_target(space, *context, surface);
#endif
    if (!renderStats) {
        return std::unexpected(renderStats.error());
    }
    auto const& stats_value = *renderStats;

    PathSurfaceMetal::TextureInfo metal_texture{};
    bool has_metal_texture = false;
#if PATHSPACE_UI_METAL
    if (metal_surface) {
        has_metal_texture = true;
    }
#endif

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    invoke_before_present_hook(surface, presentPolicy, dirty_tiles);

    PathWindowView presenter;
    std::vector<std::uint8_t> framebuffer;
    std::span<std::uint8_t> framebuffer_span{};
#if !defined(__APPLE__)
    if (framebuffer.empty()) {
        framebuffer.resize(surface.frame_bytes(), 0);
    }
    framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
#else
    if (framebuffer_span.empty()
        && (presentPolicy.capture_framebuffer || !surface.has_buffered())) {
        framebuffer.resize(surface.frame_bytes(), 0);
        framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
    }
#endif
    auto now = std::chrono::steady_clock::now();
    auto vsync_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(presentPolicy.frame_timeout);
    if (vsync_budget < std::chrono::steady_clock::duration::zero()) {
        vsync_budget = std::chrono::steady_clock::duration::zero();
    }
#if defined(__APPLE__)
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .vsync_align = presentPolicy.vsync_align,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_surface = metal_surface,
        .metal_texture = metal_texture,
        .allow_iosurface_sharing = true,
    };
#else
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .vsync_align = presentPolicy.vsync_align,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_surface = metal_surface,
        .metal_texture = metal_texture,
    };
#endif
    auto presentStats = presenter.present(surface, presentPolicy, request);
    if (renderStats) {
        presentStats.frame.frame_index = stats_value.frame_index;
        presentStats.frame.revision = stats_value.revision;
        presentStats.frame.render_ms = stats_value.render_ms;
        presentStats.damage_ms = stats_value.damage_ms;
        presentStats.encode_ms = stats_value.encode_ms;
        presentStats.progressive_copy_ms = stats_value.progressive_copy_ms;
        presentStats.publish_ms = stats_value.publish_ms;
        presentStats.drawable_count = stats_value.drawable_count;
        presentStats.progressive_tiles_updated = stats_value.progressive_tiles_updated;
        presentStats.progressive_bytes_copied = stats_value.progressive_bytes_copied;
        presentStats.progressive_tile_size = stats_value.progressive_tile_size;
        presentStats.progressive_workers_used = stats_value.progressive_workers_used;
        presentStats.progressive_jobs = stats_value.progressive_jobs;
        presentStats.encode_workers_used = stats_value.encode_workers_used;
        presentStats.encode_jobs = stats_value.encode_jobs;
        presentStats.progressive_tiles_dirty = stats_value.progressive_tiles_dirty;
        presentStats.progressive_tiles_total = stats_value.progressive_tiles_total;
        presentStats.progressive_tiles_skipped = stats_value.progressive_tiles_skipped;
        presentStats.progressive_tile_diagnostics_enabled = stats_value.progressive_tile_diagnostics_enabled;
        presentStats.backend_kind = renderer_kind_to_string(stats_value.backend_kind);
    }
#if defined(__APPLE__)
    auto copy_iosurface_into = [&](PathSurfaceSoftware::SharedIOSurface const& handle,
                                   std::vector<std::uint8_t>& out) {
        auto retained = handle.retain_for_external_use();
        if (!retained) {
            return;
        }
        bool locked = IOSurfaceLock(retained, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess;
        auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(retained));
        auto const row_bytes = IOSurfaceGetBytesPerRow(retained);
        auto const height = handle.height();
        auto const row_stride = surface.row_stride_bytes();
        auto const copy_bytes = std::min<std::size_t>(row_bytes, row_stride);
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));
        if (locked && base && copy_bytes > 0 && height > 0) {
            out.resize(total_bytes);
            for (int row = 0; row < height; ++row) {
                auto* dst = out.data() + static_cast<std::size_t>(row) * row_stride;
                auto const* src = base + static_cast<std::size_t>(row) * row_bytes;
                std::memcpy(dst, src, copy_bytes);
            }
        }
        if (locked) {
            IOSurfaceUnlock(retained, kIOSurfaceLockAvoidSync, nullptr);
        }
        CFRelease(retained);
    };

    if (presentStats.iosurface && presentStats.iosurface->valid()) {
        auto const row_stride = surface.row_stride_bytes();
        auto const height = presentStats.iosurface->height();
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));

        if (presentPolicy.capture_framebuffer) {
            copy_iosurface_into(*presentStats.iosurface, framebuffer);
        } else {
            framebuffer.clear();
        }
    }
    if (presentPolicy.capture_framebuffer
        && presentStats.buffered_frame_consumed
        && framebuffer.empty()) {
        auto required = static_cast<std::size_t>(surface.frame_bytes());
        framebuffer.resize(required);
        auto copy = surface.copy_buffered_frame(framebuffer);
        if (!copy) {
            framebuffer.clear();
        }
    }
#endif

    auto metricsBase = std::string(context->target_path.getPath()) + "/output/v1/common";
    std::uint64_t previous_age_frames = 0;
    if (auto previous = read_optional<uint64_t>(space, metricsBase + "/presentedAgeFrames"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_frames = **previous;
    }

    double previous_age_ms = 0.0;
    if (auto previous = read_optional<double>(space, metricsBase + "/presentedAgeMs"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_ms = **previous;
    }

    auto frame_timeout_ms = static_cast<double>(presentPolicy.frame_timeout.count());
    bool reuse_previous_frame = !presentStats.buffered_frame_consumed;
#if defined(__APPLE__)
    if (presentStats.used_iosurface) {
        reuse_previous_frame = false;
    }
#endif
    if (!reuse_previous_frame && presentStats.skipped) {
        reuse_previous_frame = true;
    }

    if (reuse_previous_frame) {
        presentStats.frame_age_frames = previous_age_frames + 1;
        presentStats.frame_age_ms = previous_age_ms + frame_timeout_ms;
    } else {
        presentStats.frame_age_frames = 0;
        presentStats.frame_age_ms = 0.0;
    }
    presentStats.stale = presentStats.frame_age_frames > presentPolicy.max_age_frames;

    if (auto scheduled = maybe_schedule_auto_render(space,
                                                    std::string(context->target_path.getPath()),
                                                    presentStats,
                                                    presentPolicy); !scheduled) {
        return std::unexpected(scheduled.error());
    }

    if (auto status = Diagnostics::WritePresentMetrics(space,
                                                       SP::ConcretePathStringView{context->target_path.getPath()},
                                                       presentStats,
                                                       presentPolicy); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = Diagnostics::WriteResidencyMetrics(space,
                                                         SP::ConcretePathStringView{context->target_path.getPath()},
                                                         stats_value.resource_cpu_bytes,
                                                         stats_value.resource_gpu_bytes,
                                                         context->settings.cache.cpu_soft_bytes,
                                                         context->settings.cache.cpu_hard_bytes,
                                                         context->settings.cache.gpu_soft_bytes,
                                                         context->settings.cache.gpu_hard_bytes); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = Diagnostics::WriteWindowPresentMetrics(space,
                                                              ConcretePathView{windowPath.getPath()},
                                                              viewName,
                                                              presentStats,
                                                              presentPolicy); !status) {
        return std::unexpected(status.error());
    }

    auto framebufferPath = std::string(context->target_path.getPath()) + "/output/v1/software/framebuffer";

    WindowPresentResult result{};
    result.stats = presentStats;
    if (presentPolicy.capture_framebuffer) {
        SoftwareFramebuffer stored_framebuffer{};
        stored_framebuffer.width = context->target_desc.size_px.width;
        stored_framebuffer.height = context->target_desc.size_px.height;
        stored_framebuffer.row_stride_bytes = static_cast<std::uint32_t>(surface.row_stride_bytes());
        stored_framebuffer.pixel_format = context->target_desc.pixel_format;
        stored_framebuffer.color_space = context->target_desc.color_space;
        stored_framebuffer.premultiplied_alpha = context->target_desc.premultiplied_alpha;
        stored_framebuffer.pixels = std::move(framebuffer);

        if (auto status = replace_single<SoftwareFramebuffer>(space, framebufferPath, stored_framebuffer); !status) {
            return std::unexpected(status.error());
        }
        result.framebuffer = std::move(stored_framebuffer.pixels);
    } else {
        if (auto cleared = drain_queue<SoftwareFramebuffer>(space, framebufferPath); !cleared) {
            return std::unexpected(cleared.error());
        }
        result.framebuffer = std::move(framebuffer);
    }
    return result;
}

} // namespace SP::UI::Runtime::Window
