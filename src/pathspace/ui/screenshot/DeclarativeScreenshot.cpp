#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>
#include <pathspace/ui/screenshot/ScreenshotSlot.hpp>

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using SP::UI::Screenshot::DeclarativeScreenshotOptions;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;

namespace {

auto make_invalid_argument_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::InvalidType, std::move(message)};
}

auto screenshot_trace_enabled() -> bool {
    static bool enabled = [] {
        if (const char* flag = std::getenv("PATHSPACE_SCREENSHOT_TRACE")) {
            return std::strcmp(flag, "0") != 0;
        }
        return false;
    }();
    return enabled;
}

void screenshot_trace(char const* message) {
    if (!screenshot_trace_enabled()) {
        return;
    }
    std::fprintf(stderr, "CaptureDeclarative: %s\n", message);
}

auto resolve_view_name(SP::PathSpace& space,
                       SP::UI::WindowPath const& window,
                       std::optional<std::string> const& override_name)
    -> SP::Expected<std::string> {
    if (override_name && !override_name->empty()) {
        return *override_name;
    }
    auto views_root = std::string(window.getPath()) + "/views";
    auto views = space.listChildren(SP::ConcretePathStringView{views_root});
    if (views.empty()) {
        return std::unexpected(make_invalid_argument_error("window has no views; specify view_name"));
    }
    if (views.size() > 1) {
        return std::unexpected(make_invalid_argument_error("window has multiple views; specify view_name"));
    }
    return views.front();
}

auto enable_capture_framebuffer(SP::PathSpace& space,
                                SP::UI::WindowPath const& window,
                                std::string const& view_name,
                                bool enabled) -> SP::Expected<void> {
    auto path = std::string(window.getPath()) + "/views/" + view_name + "/present/params/capture_framebuffer";
    auto inserted = space.insert(path, enabled);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto copy_software_framebuffer(SP::UI::Runtime::SoftwareFramebuffer const& framebuffer,
                               int width,
                               int height) -> SP::Expected<std::vector<std::uint8_t>> {
    if (framebuffer.width != width || framebuffer.height != height) {
        return std::unexpected(make_invalid_argument_error("software framebuffer dimensions mismatch"));
    }
    if (framebuffer.row_stride_bytes <= 0) {
        return std::unexpected(make_invalid_argument_error("software framebuffer stride missing"));
    }
    auto row_stride = static_cast<std::size_t>(framebuffer.row_stride_bytes);
    auto row_pixels = static_cast<std::size_t>(width) * 4u;
    if (row_stride < row_pixels) {
        return std::unexpected(make_invalid_argument_error("software framebuffer stride smaller than row size"));
    }
    auto required = row_pixels * static_cast<std::size_t>(std::max(height, 0));
    if (framebuffer.pixels.size() < row_stride * static_cast<std::size_t>(std::max(height, 0))) {
        return std::unexpected(make_invalid_argument_error("software framebuffer truncated"));
    }
    std::vector<std::uint8_t> packed(required);
    for (int y = 0; y < height; ++y) {
        auto const* src = framebuffer.pixels.data() + row_stride * static_cast<std::size_t>(y);
        auto* dst = packed.data() + row_pixels * static_cast<std::size_t>(y);
        std::memcpy(dst, src, row_pixels);
    }
    return packed;
}

auto read_presented_framebuffer(SP::PathSpace& space,
                                SP::UI::Declarative::PresentHandles const& handles,
                                int width,
                                int height) -> SP::Expected<std::vector<std::uint8_t>> {
    auto framebuffer = SP::UI::Runtime::Diagnostics::ReadSoftwareFramebuffer(space,
                                                                             SP::App::ConcretePathView{handles.target.getPath()});
    if (!framebuffer) {
        return std::unexpected(framebuffer.error());
    }
    return copy_software_framebuffer(*framebuffer, width, height);
}

struct CachedFramebuffer {
    std::vector<std::uint8_t> pixels;
    std::string backend_kind = "software2d";
    bool hardware_capture = false;
};

auto read_cached_framebuffer(SP::PathSpace& space,
                             SP::UI::Declarative::PresentHandles const& handles,
                             int width,
                             int height) -> SP::Expected<std::optional<CachedFramebuffer>> {
    auto framebuffer = read_presented_framebuffer(space, handles, width, height);
    if (!framebuffer) {
        auto const& error = framebuffer.error();
        if (error.code == SP::Error::Code::NoSuchPath || error.code == SP::Error::Code::NoObjectFound) {
            return std::optional<CachedFramebuffer>{};
        }
        return std::unexpected(error);
    }

    CachedFramebuffer cached{};
    cached.pixels = std::move(*framebuffer);

    auto metrics = SP::UI::Runtime::Diagnostics::ReadTargetMetrics(space,
                                                                   SP::App::ConcretePathView{handles.target.getPath()});
    if (metrics) {
        if (!metrics->backend_kind.empty()) {
            cached.backend_kind = metrics->backend_kind;
        }
        cached.hardware_capture = cached.backend_kind != "software2d";
    } else {
        auto const code = metrics.error().code;
        if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(metrics.error());
        }
    }

    return cached;
}

auto build_present_handles_for_window(SP::PathSpace& space,
                                      SP::UI::WindowPath const& window,
                                      std::string const& view_name)
    -> SP::Expected<SP::UI::Declarative::PresentHandles> {
    auto app_root = SP::App::derive_app_root(SP::App::ConcretePathView{window.getPath()});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    return SP::UI::Declarative::BuildPresentHandles(space,
                                                    SP::App::AppRootPathView{app_root->getPath()},
                                                    window,
                                                    view_name);
}

auto app_component_from_window(std::string const& window_path) -> std::optional<std::string> {
    constexpr char kPrefix[] = "/system/applications/";
    constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (window_path.size() < kPrefixLen) {
        return std::nullopt;
    }
    if (window_path.compare(0, kPrefixLen, kPrefix) != 0) {
        return std::nullopt;
    }
    auto remainder = window_path.substr(kPrefixLen);
    auto slash = remainder.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }
    return remainder.substr(0, slash);
}

auto select_theme_defaults(std::string const& sanitized)
    -> SP::UI::Runtime::Widgets::WidgetTheme {
    if (sanitized == "sunset") {
        return SP::UI::Runtime::Widgets::MakeSunsetWidgetTheme();
    }
    if (sanitized == "sunrise" || sanitized == "skylight") {
        return SP::UI::Runtime::Widgets::MakeSunriseWidgetTheme();
    }
    return SP::UI::Runtime::Widgets::MakeDefaultWidgetTheme();
}

auto apply_theme_override(SP::PathSpace& space,
                          SP::UI::WindowPath const& window,
                          std::string const& theme_name) -> SP::Expected<std::string> {
    auto app_root = SP::App::derive_app_root(SP::App::ConcretePathView{window.getPath()});
    if (!app_root) {
        return std::unexpected(app_root.error());
    }
    auto sanitized = SP::UI::Declarative::ThemeConfig::SanitizeName(theme_name);
    if (sanitized.empty()) {
        return std::unexpected(make_invalid_argument_error("theme name must not be empty"));
    }
    auto defaults = select_theme_defaults(sanitized);
    auto ensured = SP::UI::Declarative::ThemeConfig::Ensure(space,
                                                            SP::App::AppRootPathView{app_root->getPath()},
                                                            sanitized,
                                                            defaults);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    if (auto update_value = DeclarativeDetail::replace_single<SP::UI::Runtime::Widgets::WidgetTheme>(space,
                                                                                                     ensured->value.getPath(),
                                                                                                     defaults);
        !update_value) {
        return std::unexpected(update_value.error());
    }
    auto status = SP::UI::Declarative::ThemeConfig::SetActive(space,
                                                              SP::App::AppRootPathView{app_root->getPath()},
                                                              sanitized);
    if (!status) {
        return std::unexpected(status.error());
    }
    auto window_theme_path = std::string(window.getPath()) + "/style/theme";
    if (auto window_status = DeclarativeDetail::replace_single<std::string>(space,
                                                                            window_theme_path,
                                                                            sanitized);
        !window_status) {
        return std::unexpected(window_status.error());
    }
    if (const char* debug_env = std::getenv("PATHSPACE_SCREENSHOT_DEBUG_THEME")) {
        (void)debug_env;
        auto debug_value = space.read<std::string, std::string>(window_theme_path);
        if (debug_value) {
            std::fprintf(stderr,
                         "CaptureDeclarative: window theme now '%s'\n",
                         debug_value->c_str());
        }
        auto debug_theme = space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(ensured->value.getPath());
        if (debug_theme) {
            std::fprintf(stderr,
                         "CaptureDeclarative: button background rgba = (%f,%f,%f,%f)\n",
                         (*debug_theme).button.background_color[0],
                         (*debug_theme).button.background_color[1],
                         (*debug_theme).button.background_color[2],
                         (*debug_theme).button.background_color[3]);
        }
    }
    return sanitized;
}
auto derive_surface_dimensions(SP::PathSpace& space,
                               SP::UI::WindowPath const& window,
                               std::string const& view_name)
    -> SP::Expected<std::pair<int, int>> {
    auto view_base = SP::UI::Declarative::MakeWindowViewPath(window, view_name);
    auto surface_rel = space.read<std::string, std::string>(view_base + "/surface");
    if (!surface_rel) {
        return std::unexpected(surface_rel.error());
    }
    if (surface_rel->empty()) {
        return std::unexpected(make_invalid_argument_error("window view missing surface binding"));
    }
    auto app_root = SP::UI::Declarative::AppRootFromWindow(window);
    if (app_root.empty()) {
        return std::unexpected(make_invalid_argument_error("window missing app root"));
    }
    auto resolved = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root}, *surface_rel);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    auto desc_path = std::string(resolved->getPath()) + "/desc";
    auto surface_desc = space.read<SP::UI::Runtime::SurfaceDesc, std::string>(desc_path);
    if (!surface_desc) {
        return std::unexpected(surface_desc.error());
    }
    return std::pair(surface_desc->size_px.width, surface_desc->size_px.height);
}

auto build_readiness_options(DeclarativeScreenshotOptions const& options,
                             std::string const& view_name) -> SP::UI::Declarative::DeclarativeReadinessOptions {
    auto readiness = options.readiness_options;
    if (options.readiness_timeout.count() > 0) {
        readiness.widget_timeout = options.readiness_timeout;
        if (readiness.revision_timeout.count() <= 0) {
            readiness.revision_timeout = options.readiness_timeout;
        }
    }
    readiness.wait_for_runtime_metrics = options.wait_for_runtime_metrics;
    readiness.force_scene_publish = options.force_publish;
    readiness.scene_view_override = view_name;
    return readiness;
}

auto ensure_capture_runtimes(SP::PathSpace& space) -> SP::Expected<void> {
    SP::System::LaunchOptions launch{};
    launch.start_input_runtime = true;
    launch.start_widget_event_trellis = true;
    launch.start_io_trellis = false;
    launch.start_io_pump = false;
    launch.start_io_telemetry_control = false;
    launch.start_paint_gpu_uploader = false;
    auto started = SP::System::LaunchStandard(space, launch);
    if (!started) {
        return std::unexpected(started.error());
    }
    return {};
}

void write_slot_error(SP::PathSpace& space,
                      SP::UI::Screenshot::ScreenshotSlotPaths const& slot_paths,
                      std::string const& message) {
    SP::UI::Screenshot::ScreenshotResult empty{};
    empty.artifact = std::filesystem::path{};
    auto write = SP::UI::Screenshot::WriteScreenshotSlotResult(space,
                                                               slot_paths,
                                                               empty,
                                                               "error",
                                                               "unknown",
                                                               message);
    (void)write;
    if (screenshot_trace_enabled()) {
        auto traced = std::string{"CaptureDeclarativeSimple error: "} + message;
        screenshot_trace(traced.c_str());
    }
}

} // namespace

namespace SP::UI::Screenshot {

auto CaptureDeclarative(SP::PathSpace& space,
                        SP::UI::ScenePath const& scene,
                        SP::UI::WindowPath const& window,
                        DeclarativeScreenshotOptions const& options)
    -> SP::Expected<ScreenshotResult> {
    auto runtimes = ensure_capture_runtimes(space);
    if (!runtimes) {
        return std::unexpected(runtimes.error());
    }

    auto view_name = resolve_view_name(space, window, options.view_name);
    if (!view_name) {
        return std::unexpected(view_name.error());
    }

    if (!options.output_png) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath,
                                         "Declarative screenshot requires an output_png"});
    }

    std::optional<std::string> theme_override = options.theme_override;

    auto readiness = build_readiness_options(options, *view_name);
    auto readiness_result = SP::UI::Declarative::EnsureDeclarativeSceneReady(space,
                                                                             scene,
                                                                             window,
                                                                             *view_name,
                                                                             readiness);
    if (!readiness_result) {
        return std::unexpected(readiness_result.error());
    }

    std::optional<std::string> applied_theme;
    bool theme_changed = false;
    if (theme_override && !theme_override->empty()) {
        auto applied = apply_theme_override(space, window, *theme_override);
        if (!applied) {
            return std::unexpected(applied.error());
        }
        applied_theme = *applied;
        theme_changed = true;
    } else {
        auto window_theme_path = std::string(window.getPath()) + "/style/theme";
        auto stored_theme = space.read<std::string, std::string>(window_theme_path);
        if (stored_theme) {
            if (!stored_theme->empty()) {
                applied_theme = SP::UI::Declarative::ThemeConfig::SanitizeName(*stored_theme);
            }
        } else {
            auto const code = stored_theme.error().code;
            if (code != SP::Error::Code::NoSuchPath && code != SP::Error::Code::NoObjectFound) {
                return std::unexpected(stored_theme.error());
            }
        }
        if (!applied_theme) {
            auto system_theme = SP::UI::Declarative::ThemeConfig::LoadSystemActive(space);
            if (!system_theme) {
                return std::unexpected(system_theme.error());
            }
            applied_theme = *system_theme;
        }
    }

    std::optional<std::uint64_t> forced_revision;
    bool force_publish = options.force_publish || theme_changed;
    bool mark_dirty_for_publish = options.mark_dirty_before_publish || theme_changed;
    if (force_publish) {
        if (mark_dirty_for_publish) {
            auto mark_dirty = SP::UI::Declarative::SceneLifecycle::MarkDirty(space,
                                                                             scene,
                                                                             SP::UI::Runtime::Scene::DirtyKind::All);
            if (!mark_dirty) {
                return std::unexpected(mark_dirty.error());
            }
        }
        auto publish_timeout = options.publish_timeout.count() > 0 ? options.publish_timeout
                                                                   : std::chrono::milliseconds{2000};
        SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
        publish_options.wait_timeout = publish_timeout;
        publish_options.min_revision = readiness_result->scene_revision;
        auto forced = SP::UI::Declarative::SceneLifecycle::ForcePublish(space,
                                                                        scene,
                                                                        publish_options);
        if (!forced) {
            return std::unexpected(forced.error());
        }
        forced_revision = *forced;
    }

    auto should_wait_revision = force_publish || readiness.wait_for_revision;
    if (should_wait_revision) {
        auto revision_timeout = readiness.revision_timeout.count() > 0 ? readiness.revision_timeout
                                                                      : options.readiness_timeout;
        std::optional<std::uint64_t> wait_floor;
        if (forced_revision && *forced_revision > 0) {
            wait_floor = *forced_revision - 1;
        } else if (readiness_result->scene_revision && *readiness_result->scene_revision > 0) {
            wait_floor = *readiness_result->scene_revision - 1;
        }
        auto ready_revision = SP::UI::Declarative::WaitForDeclarativeSceneRevision(space,
                                                                                  scene,
                                                                                  revision_timeout,
                                                                                  wait_floor);
        if (!ready_revision) {
            return std::unexpected(ready_revision.error());
        }
    }

    auto dimensions = derive_surface_dimensions(space, window, *view_name);
    if (!dimensions) {
        return std::unexpected(dimensions.error());
    }
    auto width = options.width.value_or(dimensions->first);
    auto height = options.height.value_or(dimensions->second);
    if (width <= 0 || height <= 0) {
        return std::unexpected(make_invalid_argument_error("screenshot dimensions must be positive"));
    }

    if (options.enable_capture_framebuffer) {
        auto capture_flag = enable_capture_framebuffer(space, window, *view_name, true);
        if (!capture_flag) {
            return std::unexpected(capture_flag.error());
        }
    }

    bool require_present = options.require_present;
    if (!require_present && options.baseline_png && !options.force_software) {
        require_present = true;
    }

    auto slot_paths = MakeScreenshotSlotPaths(window, *view_name);
    struct SlotArmedGuard {
        SP::PathSpace* space = nullptr;
        std::string path;
        ~SlotArmedGuard() {
            if (space && !path.empty()) {
                while (true) {
                    auto removed = space->take<bool>(path, SP::Pop{});
                    if (!removed) {
                        auto const& error = removed.error();
                        if (error.code == SP::Error::Code::NoSuchPath
                            || error.code == SP::Error::Code::NoObjectFound
                            || error.code == SP::Error::Code::InvalidType) {
                            break;
                        }
                        break;
                    }
                }
                (void)space->insert(path, false);
            }
        }
    } armed_guard{&space, slot_paths.armed};

    SlotEphemeralData ephemeral{};
    ephemeral.baseline_metadata = options.baseline_metadata;
    ephemeral.postprocess_png = options.postprocess_png;
    ephemeral.verify_output_matches_framebuffer = options.verify_output_matches_framebuffer;
    ephemeral.verify_max_mean_error = options.verify_max_mean_error;
    RegisterSlotEphemeral(slot_paths.base, std::move(ephemeral));

    ScreenshotSlotRequest slot_request{};
    slot_request.output_png = *options.output_png;
    slot_request.baseline_png = options.baseline_png;
    slot_request.diff_png = options.diff_png;
    slot_request.metrics_json = options.metrics_json;
    slot_request.capture_mode = options.capture_mode.empty() ? std::string{"next_present"} : options.capture_mode;
    slot_request.frame_index = options.capture_frame_index;
    if (options.capture_deadline) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        slot_request.deadline_ns =
            static_cast<std::uint64_t>((now + *options.capture_deadline).count());
    }
    slot_request.width = width;
    slot_request.height = height;
    slot_request.force_software = options.force_software;
    slot_request.allow_software_fallback = options.allow_software_fallback;
    slot_request.present_when_force_software = options.present_when_force_software;
    slot_request.require_present = require_present;
    slot_request.verify_output_matches_framebuffer = options.verify_output_matches_framebuffer;
    slot_request.max_mean_error = options.max_mean_error.value_or(0.0015);
    slot_request.verify_max_mean_error = options.verify_max_mean_error;

    auto token = AcquireScreenshotToken(space, slot_paths.token, options.token_timeout);
    if (!token) {
        return std::unexpected(token.error());
    }

    auto write_request = WriteScreenshotSlotRequest(space, slot_paths, slot_request);
    if (!write_request) {
        return std::unexpected(write_request.error());
    }

    auto handles = build_present_handles_for_window(space, window, *view_name);
    if (!handles) {
        return std::unexpected(handles.error());
    }

    std::optional<CachedFramebuffer> cached_framebuffer;
    if (!options.present_before_capture) {
        auto cached = read_cached_framebuffer(space, *handles, width, height);
        if (!cached) {
            return std::unexpected(cached.error());
        }
        cached_framebuffer = std::move(*cached);
        if (!cached_framebuffer) {
            auto message = std::string{"no framebuffer available for screenshot"};
            ScreenshotResult empty{};
            empty.artifact = *options.output_png;
            auto backend_kind = std::string{"unknown"};
            if (auto metrics = SP::UI::Runtime::Diagnostics::ReadTargetMetrics(space,
                                                                               SP::App::ConcretePathView{handles->target.getPath()})) {
                if (!metrics->backend_kind.empty()) {
                    backend_kind = metrics->backend_kind;
                }
            }
            if (auto write_error = WriteScreenshotSlotResult(space,
                                                             slot_paths,
                                                             empty,
                                                             "error",
                                                             backend_kind,
                                                             message);
                !write_error) {
                return std::unexpected(write_error.error());
            }
            return std::unexpected(SP::Error{SP::Error::Code::NoObjectFound, std::move(message)});
        }
    }

    if (cached_framebuffer) {
        ScreenshotRequest capture_request{
            .space = space,
            .window_path = window,
            .view_name = *view_name,
            .width = width,
            .height = height,
            .output_png = *options.output_png,
            .baseline_png = options.baseline_png,
            .diff_png = options.diff_png,
            .metrics_json = options.metrics_json,
            .max_mean_error = options.max_mean_error.value_or(0.0015),
            .require_present = options.require_present,
            .present_timeout = options.present_timeout,
            .baseline_metadata = options.baseline_metadata,
            .force_software = options.force_software,
            .allow_software_fallback = options.allow_software_fallback,
            .present_when_force_software = options.present_when_force_software,
            .provided_framebuffer = std::span<std::uint8_t>(cached_framebuffer->pixels.data(),
                                                            cached_framebuffer->pixels.size()),
            .provided_framebuffer_is_hardware = cached_framebuffer->hardware_capture,
            .verify_output_matches_framebuffer = options.verify_output_matches_framebuffer,
            .verify_max_mean_error = options.verify_max_mean_error,
        };
        if (applied_theme) {
            capture_request.theme_override = applied_theme;
        }
        if (!capture_request.verify_max_mean_error && capture_request.verify_output_matches_framebuffer) {
            capture_request.verify_max_mean_error = options.max_mean_error.value_or(0.0);
        }
        if (auto ephemeral = ConsumeSlotEphemeral(slot_paths.base)) {
            capture_request.baseline_metadata = ephemeral->baseline_metadata;
            capture_request.postprocess_png = std::move(ephemeral->postprocess_png);
            capture_request.verify_output_matches_framebuffer = ephemeral->verify_output_matches_framebuffer;
            if (ephemeral->verify_max_mean_error) {
                capture_request.verify_max_mean_error = ephemeral->verify_max_mean_error;
            }
        }

        auto capture_result = ScreenshotService::Capture(capture_request);
        if (capture_result) {
            auto write = WriteScreenshotSlotResult(space,
                                                   slot_paths,
                                                   *capture_result,
                                                   capture_result->status,
                                                   cached_framebuffer->backend_kind,
                                                   std::nullopt);
            if (!write) {
                return std::unexpected(write.error());
            }
            return capture_result;
        }

        ScreenshotResult empty{};
        empty.artifact = *options.output_png;
        auto write_error = WriteScreenshotSlotResult(space,
                                                     slot_paths,
                                                     empty,
                                                     "error",
                                                     cached_framebuffer->backend_kind,
                                                     SP::describeError(capture_result.error()));
        if (!write_error) {
            return std::unexpected(write_error.error());
        }
        return std::unexpected(capture_result.error());
    }

    std::optional<SP::UI::Declarative::PresentFrame> present_frame;
    if (options.present_before_capture) {
        auto const present_start = std::chrono::steady_clock::now();
        auto present_frame_result = SP::UI::Declarative::PresentWindowFrame(space, *handles);
        auto const present_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - present_start);
        if (!present_frame_result) {
            if (const char* debug_present = std::getenv("PATHSPACE_SCREENSHOT_DEBUG_PRESENT")) {
                (void)debug_present;
                std::fprintf(stderr,
                             "CaptureDeclarative: present failed present_ms=%lld error=%s\n",
                             static_cast<long long>(present_ms.count()),
                             describeError(present_frame_result.error()).c_str());
            }
            return std::unexpected(present_frame_result.error());
        }
        if (const char* debug_present = std::getenv("PATHSPACE_SCREENSHOT_DEBUG_PRESENT")) {
            (void)debug_present;
            std::fprintf(stderr,
                         "CaptureDeclarative: present stats drawables=%llu skipped=%d backend=%s framebuffer=%zu bytes present_ms=%lld\n",
                         static_cast<unsigned long long>(present_frame_result->stats.drawable_count),
                         present_frame_result->stats.skipped ? 1 : 0,
                         present_frame_result->stats.backend_kind.c_str(),
                         present_frame_result->framebuffer.size(),
                         static_cast<long long>(present_ms.count()));
        }
        if (require_present && present_frame_result->stats.drawable_count == 0
            && present_frame_result->framebuffer.empty()) {
            return std::unexpected(make_invalid_argument_error("present produced no drawables for screenshot"));
        }
        present_frame = std::move(*present_frame_result);
    }

    auto slot_result = WaitForScreenshotSlotResult(space, slot_paths, options.slot_timeout);
    if (!slot_result) {
        if (slot_result.error().code == SP::Error::Code::Timeout) {
            (void)WriteScreenshotSlotTimeout(space,
                                             slot_paths,
                                             std::string_view{"unknown"},
                                             std::string_view{"screenshot slot wait timed out"});
            if (present_frame) {
                ScreenshotRequest fallback{
                    .space = space,
                    .window_path = window,
                    .view_name = *view_name,
                    .width = width,
                    .height = height,
                    .output_png = *options.output_png,
                    .baseline_png = options.baseline_png,
                    .diff_png = options.diff_png,
                    .metrics_json = options.metrics_json,
                    .max_mean_error = options.max_mean_error.value_or(0.0015),
                    .require_present = require_present,
                    .present_timeout = options.present_timeout,
                    .baseline_metadata = options.baseline_metadata,
                    .force_software = options.force_software,
                    .allow_software_fallback = options.allow_software_fallback,
                    .present_when_force_software = options.present_when_force_software,
                    .verify_output_matches_framebuffer = options.verify_output_matches_framebuffer,
                    .verify_max_mean_error = options.verify_max_mean_error,
                    .postprocess_png = options.postprocess_png,
                };
                if (present_frame && !present_frame->framebuffer.empty()) {
                    fallback.provided_framebuffer = std::span<std::uint8_t>(present_frame->framebuffer.data(),
                                                                            present_frame->framebuffer.size());
                    fallback.provided_framebuffer_is_hardware = !options.force_software
                        && present_frame->stats.backend_kind != "software2d";
                    if (!fallback.verify_max_mean_error) {
                        fallback.verify_max_mean_error = options.max_mean_error.value_or(0.0);
                    }
                }
                if (applied_theme) {
                    fallback.theme_override = applied_theme;
                }
                auto capture_fallback = ScreenshotService::Capture(fallback);
                if (capture_fallback) {
                    (void)WriteScreenshotSlotResult(space,
                                                    slot_paths,
                                                    *capture_fallback,
                                                    capture_fallback->status,
                                                    present_frame->stats.backend_kind,
                                                    std::nullopt);
                    return capture_fallback;
                }
            }
        }
        if (screenshot_trace_enabled()) {
            auto message = std::string{"CaptureDeclarative: slot wait failed "} + describeError(slot_result.error());
            screenshot_trace(message.c_str());
        }
        return std::unexpected(slot_result.error());
    }

    ScreenshotResult result{};
    result.artifact = slot_result->artifact;
    result.mean_error = slot_result->mean_error;
    result.hardware_capture = slot_result->backend.value_or("") != "software2d";
    result.matched_baseline = slot_result->status == "match";
    result.status = slot_result->status;
    return result;
}

auto CaptureDeclarativeSimple(SP::PathSpace& space,
                              SP::UI::ScenePath const& scene,
                              SP::UI::WindowPath const& window,
                              std::filesystem::path const& output_png,
                              std::optional<int> width,
                              std::optional<int> height)
    -> void {
    (void)scene; // scene path retained for parity with advanced helper signature.

    auto view_name = resolve_view_name(space, window, std::nullopt);
    if (!view_name) {
        if (screenshot_trace_enabled()) {
            auto traced = std::string{"CaptureDeclarativeSimple view resolution failed: "}
                + SP::describeError(view_name.error());
            screenshot_trace(traced.c_str());
        }
        return;
    }

    auto slot_paths = MakeScreenshotSlotPaths(window, *view_name);

    auto dimensions = derive_surface_dimensions(space, window, *view_name);
    if (!dimensions) {
        write_slot_error(space, slot_paths, SP::describeError(dimensions.error()));
        return;
    }
    auto resolved_width = width.value_or(dimensions->first);
    auto resolved_height = height.value_or(dimensions->second);
    if (resolved_width <= 0 || resolved_height <= 0) {
        write_slot_error(space, slot_paths, "screenshot dimensions must be positive");
        return;
    }

    auto capture_flag = enable_capture_framebuffer(space, window, *view_name, true);
    if (!capture_flag) {
        write_slot_error(space, slot_paths, SP::describeError(capture_flag.error()));
        return;
    }

    ScreenshotSlotRequest slot_request{};
    slot_request.output_png = output_png;
    slot_request.capture_mode = "next_present";
    slot_request.width = resolved_width;
    slot_request.height = resolved_height;
    slot_request.force_software = false;
    slot_request.allow_software_fallback = true;
    slot_request.present_when_force_software = false;
    slot_request.require_present = true;
    slot_request.verify_output_matches_framebuffer = true;
    slot_request.max_mean_error = 0.0015;

    auto token = AcquireScreenshotToken(space, slot_paths.token, std::chrono::milliseconds{500});
    if (!token) {
        if (screenshot_trace_enabled()) {
            auto traced = std::string{"CaptureDeclarativeSimple token contention: "}
                + SP::describeError(token.error());
            screenshot_trace(traced.c_str());
        }
        return;
    }

    SlotEphemeralData ephemeral{};
    ephemeral.baseline_metadata = {};
    ephemeral.verify_output_matches_framebuffer = true;
    RegisterSlotEphemeral(slot_paths.base, std::move(ephemeral));

    auto write_request = WriteScreenshotSlotRequest(space, slot_paths, slot_request);
    if (!write_request) {
        write_slot_error(space, slot_paths, SP::describeError(write_request.error()));
        return;
    }

    token->release();
}

} // namespace SP::UI::Screenshot
