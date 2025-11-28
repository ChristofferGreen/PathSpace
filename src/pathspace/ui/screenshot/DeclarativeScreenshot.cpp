#include <pathspace/ui/screenshot/DeclarativeScreenshot.hpp>

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using SP::UI::Screenshot::DeclarativeScreenshotOptions;
using SP::UI::Screenshot::Hooks;

namespace {

auto make_invalid_argument_error(std::string message) -> SP::Error {
    return SP::Error{SP::Error::Code::InvalidType, std::move(message)};
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

} // namespace

namespace SP::UI::Screenshot {

auto CaptureDeclarative(SP::PathSpace& space,
                        SP::UI::ScenePath const& scene,
                        SP::UI::WindowPath const& window,
                        DeclarativeScreenshotOptions const& options)
    -> SP::Expected<ScreenshotResult> {
    auto view_name = resolve_view_name(space, window, options.view_name);
    if (!view_name) {
        return std::unexpected(view_name.error());
    }

    if (!options.output_png) {
        return std::unexpected(make_invalid_argument_error("Declarative screenshot requires an output_png"));
    }

    auto readiness = build_readiness_options(options, *view_name);
    auto readiness_result = SP::UI::Declarative::EnsureDeclarativeSceneReady(space,
                                                                             scene,
                                                                             window,
                                                                             *view_name,
                                                                             readiness);
    if (!readiness_result) {
        return std::unexpected(readiness_result.error());
    }

    std::optional<std::uint64_t> forced_revision;
    if (options.force_publish) {
        if (options.mark_dirty_before_publish) {
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

    auto should_wait_revision = options.force_publish || readiness.wait_for_revision;
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

    auto telemetry_namespace = options.telemetry_namespace;
    if (!telemetry_namespace || telemetry_namespace->empty()) {
        if (auto app_component = app_component_from_window(window.getPath())) {
            telemetry_namespace = *app_component;
        } else {
            telemetry_namespace = SP::UI::Declarative::WindowComponentName(std::string(window.getPath()));
        }
    }

    bool require_present = options.require_present;
    if (!require_present && options.baseline_png && !options.force_software) {
        require_present = true;
    }

    ScreenshotRequest request{
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
        .telemetry_namespace = *telemetry_namespace,
        .force_software = options.force_software,
        .allow_software_fallback = options.allow_software_fallback,
    };

    request.telemetry_root = options.telemetry_root.value_or("/diagnostics/ui/screenshot");
    if (options.hooks) {
        request.hooks = *options.hooks;
    }

    auto capture_result = ScreenshotService::Capture(request);
    if (!capture_result) {
        return capture_result;
    }
    return capture_result;
}

} // namespace SP::UI::Screenshot
