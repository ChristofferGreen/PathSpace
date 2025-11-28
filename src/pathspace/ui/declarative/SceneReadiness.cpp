#include <pathspace/ui/declarative/SceneReadiness.hpp>

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/runtime/IOPump.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

auto manual_pump_metrics_requested() -> bool {
    if (const char* env = std::getenv("PATHSPACE_RECORD_MANUAL_PUMPS")) {
        if (env[0] == '0' && env[1] == '\0') {
            return false;
        }
        return true;
    }
    return false;
}

auto manual_pump_metrics_file_path() -> std::optional<std::string> {
    if (const char* dir = std::getenv("PATHSPACE_TEST_ARTIFACT_DIR")) {
        if (*dir == '\0') {
            return std::nullopt;
        }
        std::string path{dir};
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path.append("manual_pump_metrics.jsonl");
        return path;
    }
    return std::nullopt;
}

auto json_escape(std::string_view value) -> std::string {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (char ch : value) {
        switch (ch) {
        case '"':
            escaped.append("\\\"");
            break;
        case '\\':
            escaped.append("\\\\");
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

auto app_component_from_window_path(std::string const& window_path) -> std::optional<std::string> {
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

auto read_manual_metric(SP::PathSpace& space,
                        std::string const& base,
                        std::string const& leaf) -> std::optional<std::uint64_t> {
    auto value = space.read<std::uint64_t, std::string>(base + "/" + leaf);
    if (value) {
        return *value;
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoSuchPath
        || error.code == SP::Error::Code::NoObjectFound) {
        return std::nullopt;
    }
    return std::nullopt;
}

auto now_timestamp_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto readiness_skip_requested() -> bool {
    return std::getenv("PATHSPACE_SKIP_UI_READY_WAIT") != nullptr;
}

} // namespace

namespace SP::UI::Declarative {

auto MakeWindowViewPath(SP::UI::WindowPath const& window,
                        std::string const& view_name) -> std::string {
    std::string view_path = std::string(window.getPath());
    view_path.append("/views/");
    view_path.append(view_name);
    return view_path;
}

auto WindowComponentName(std::string const& window_path) -> std::string {
    auto slash = window_path.find_last_of('/');
    if (slash == std::string::npos) {
        return window_path;
    }
    return window_path.substr(slash + 1);
}

auto AppRootFromWindow(SP::UI::WindowPath const& window) -> std::string {
    auto full = std::string(window.getPath());
    auto windows_pos = full.find("/windows/");
    if (windows_pos == std::string::npos) {
        return {};
    }
    return full.substr(0, windows_pos);
}

auto MakeSceneWidgetsRootComponents(SP::UI::ScenePath const& scene,
                                    std::string_view window_component,
                                    std::string_view view_name) -> std::string {
    std::string root = std::string(scene.getPath());
    root.append("/structure/widgets/windows/");
    root.append(window_component);
    root.append("/views/");
    root.append(view_name);
    root.append("/widgets");
    return root;
}

auto MakeSceneWidgetsRoot(SP::UI::ScenePath const& scene,
                          SP::UI::WindowPath const& window,
                          std::string const& view_name) -> std::string {
    auto window_component = WindowComponentName(std::string(window.getPath()));
    return MakeSceneWidgetsRootComponents(scene, window_component, view_name);
}

auto CountWindowWidgets(SP::PathSpace& space,
                        SP::UI::WindowPath const& window,
                        std::string const& view_name) -> std::size_t {
    auto widgets_root = MakeWindowViewPath(window, view_name) + "/widgets";
    auto children = space.listChildren(SP::ConcretePathStringView{widgets_root});
    return children.size();
}

auto WaitForRuntimeMetricVisible(SP::PathSpace& space,
                                 std::string const& metric_path,
                                 std::chrono::milliseconds timeout) -> SP::Expected<void> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = space.read<std::uint64_t, std::string>(metric_path);
        if (value) {
            return {};
        }
        auto const& error = value.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "runtime metric path did not appear: " + metric_path});
}

auto WaitForRuntimeMetricsReady(SP::PathSpace& space,
                                std::chrono::milliseconds timeout) -> SP::Expected<void> {
    constexpr std::string_view kInputMetric =
        "/system/widgets/runtime/input/metrics/widgets_processed_total";
    constexpr std::string_view kWidgetOpsMetric =
        "/system/widgets/runtime/events/metrics/widget_ops_total";
    if (auto status = WaitForRuntimeMetricVisible(space, std::string(kInputMetric), timeout);
        !status) {
        return status;
    }
    return WaitForRuntimeMetricVisible(space, std::string(kWidgetOpsMetric), timeout);
}

auto WaitForDeclarativeSceneWidgets(SP::PathSpace& space,
                                    std::string const& widgets_root,
                                    std::size_t expected_widgets,
                                    std::chrono::milliseconds timeout) -> SP::Expected<void> {
    if (expected_widgets == 0) {
        return {};
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto children = space.listChildren(SP::ConcretePathStringView{widgets_root});
        if (children.size() >= expected_widgets) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "scene widget structure did not publish"});
}

auto WaitForDeclarativeWidgetBuckets(SP::PathSpace& space,
                                     SP::UI::ScenePath const& scene,
                                     std::size_t expected_widgets,
                                     std::chrono::milliseconds timeout) -> SP::Expected<void> {
    if (expected_widgets == 0) {
        return {};
    }
    auto metrics_base = std::string(scene.getPath()) + "/runtime/lifecycle/metrics";
    auto widgets_path = metrics_base + "/widgets_with_buckets";
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto buckets = space.read<std::uint64_t, std::string>(widgets_path);
        if (buckets && *buckets >= expected_widgets) {
            return {};
        }
        if (buckets && *buckets == 0 && expected_widgets == 0) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "widgets never published render buckets"});
}

auto WaitForDeclarativeSceneRevision(SP::PathSpace& space,
                                     SP::UI::ScenePath const& scene,
                                     std::chrono::milliseconds timeout,
                                     std::optional<std::uint64_t> min_revision)
    -> SP::Expected<std::uint64_t> {
    auto revision_path = std::string(scene.getPath()) + "/current_revision";
    auto format_revision = [](std::uint64_t revision) {
        std::ostringstream oss;
        oss << std::setw(16) << std::setfill('0') << revision;
        return oss.str();
    };
    std::optional<std::uint64_t> ready_revision;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto revision = space.read<std::uint64_t, std::string>(revision_path);
        if (revision) {
            if (*revision != 0
                && (!min_revision.has_value() || *revision > *min_revision)) {
                ready_revision = *revision;
                break;
            }
        } else {
            auto const& error = revision.error();
            if (error.code != SP::Error::Code::NoSuchPath
                && error.code != SP::Error::Code::NoObjectFound) {
                return std::unexpected(error);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ready_revision) {
        return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                         "scene revision did not publish"});
    }
    auto revision_str = format_revision(*ready_revision);
    auto bucket_path = std::string(scene.getPath()) + "/builds/" + revision_str + "/bucket/drawables.bin";
    auto bucket_deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < bucket_deadline) {
        auto drawables = space.read<std::vector<std::uint8_t>, std::string>(bucket_path);
        if (drawables) {
            return *ready_revision;
        }
        auto const& error = drawables.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::unexpected(SP::Error{SP::Error::Code::Timeout,
                                     "scene bucket did not publish"});
}

static auto read_scene_lifecycle_diagnostics(SP::PathSpace& space,
                                             SP::UI::ScenePath const& scene) -> std::string {
    auto metrics_base = std::string(scene.getPath()) + "/runtime/lifecycle/metrics";
    auto read_string = [&](std::string const& leaf) -> std::optional<std::string> {
        auto value = space.read<std::string, std::string>(metrics_base + "/" + leaf);
        if (!value) {
            auto const& error = value.error();
            if (error.code == SP::Error::Code::NoSuchPath
                || error.code == SP::Error::Code::NoObjectFound) {
                return std::nullopt;
            }
            return std::string{"<error reading " + leaf + ">"};
        }
        return *value;
    };
    auto read_uint = [&](std::string const& leaf) -> std::optional<std::uint64_t> {
        auto value = space.read<std::uint64_t, std::string>(metrics_base + "/" + leaf);
        if (!value) {
            auto const& error = value.error();
            if (error.code == SP::Error::Code::NoSuchPath
                || error.code == SP::Error::Code::NoObjectFound) {
                return std::nullopt;
            }
            return std::uint64_t{0};
        }
        return *value;
    };
    std::ostringstream oss;
    bool has_data = false;
    if (auto widgets = read_uint("widgets_with_buckets")) {
        oss << "widgets_with_buckets=" << *widgets;
        has_data = true;
    }
    if (auto descriptor = read_string("last_descriptor_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_descriptor_error=" << *descriptor;
        has_data = true;
    }
    if (auto bucket = read_string("last_bucket_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_bucket_error=" << *bucket;
        has_data = true;
    }
    if (auto last_error = read_string("last_error")) {
        if (has_data) {
            oss << ' ';
        }
        oss << "last_error=" << *last_error;
        has_data = true;
    }
    if (!has_data) {
        return {};
    }
    return oss.str();
}

static auto record_manual_pump_metrics(SP::PathSpace& space,
                                       SP::UI::WindowPath const& window,
                                       std::string const& view_name) -> void {
    if (!manual_pump_metrics_requested()) {
        return;
    }
    auto output_path = manual_pump_metrics_file_path();
    if (!output_path) {
        return;
    }
    auto app_component = app_component_from_window_path(window.getPath());
    if (!app_component) {
        return;
    }
    auto window_token = SP::Runtime::MakeRuntimeWindowToken(window.getPath());
    std::string window_metrics_base = std::string("/system/widgets/runtime/input/windows/") + window_token + "/metrics";
    std::string app_metrics_base = std::string("/system/widgets/runtime/input/apps/") + *app_component + "/metrics";

    auto collect_metrics = [&](std::string const& base) {
        std::vector<std::pair<std::string, std::uint64_t>> metrics;
        auto append_metric = [&](char const* leaf) {
            if (auto value = read_manual_metric(space, base, leaf)) {
                metrics.emplace_back(leaf, *value);
            }
        };
        append_metric("widgets_processed_total");
        append_metric("actions_published_total");
        append_metric("manual_pumps_total");
        append_metric("last_manual_pump_ns");
        return metrics;
    };

    auto window_metrics = collect_metrics(window_metrics_base);
    auto app_metrics = collect_metrics(app_metrics_base);
    if (window_metrics.empty() && app_metrics.empty()) {
        return;
    }

    std::ostringstream payload;
    payload << "{\"timestamp_ns\":" << now_timestamp_ns();
    payload << ",\"window_path\":\"" << json_escape(window.getPath()) << '\"';
    payload << ",\"window_token\":\"" << json_escape(window_token) << '\"';
    payload << ",\"view\":\"" << json_escape(view_name) << '\"';
    payload << ",\"app_component\":\"" << json_escape(*app_component) << '\"';

    auto write_metric_object = [&](char const* key,
                                   std::vector<std::pair<std::string, std::uint64_t>> const& metrics) {
        if (metrics.empty()) {
            return;
        }
        payload << ",\"" << key << "\":{";
        bool first = true;
        for (auto const& entry : metrics) {
            if (!first) {
                payload << ',';
            }
            first = false;
            payload << '\"' << entry.first << "\":" << entry.second;
        }
        payload << '}';
    };

    write_metric_object("window_metrics", window_metrics);
    write_metric_object("app_metrics", app_metrics);
    payload << "}\n";

    std::ofstream stream(*output_path, std::ios::app);
    if (!stream) {
        return;
    }
    stream << payload.str();
}

static auto force_scene_publish_with_retry(SP::PathSpace& space,
                                           SP::UI::ScenePath const& scene,
                                           std::chrono::milliseconds widget_timeout,
                                           std::chrono::milliseconds publish_timeout,
                                           std::optional<std::uint64_t> min_revision,
                                           DeclarativeReadinessOptions const& readiness_options)
    -> SP::Expected<std::uint64_t> {
    auto deadline = std::chrono::steady_clock::now() + widget_timeout;
    SP::Error last_error{SP::Error::Code::Timeout, "scene force publish timed out"};
    SP::UI::Declarative::SceneLifecycle::ForcePublishOptions publish_options{};
    publish_options.wait_timeout = publish_timeout;
    publish_options.min_revision = min_revision;
    auto make_pump_options = [&]() {
        auto pump_options = readiness_options.scene_pump_options;
        if (pump_options.wait_timeout.count() <= 0) {
            pump_options.wait_timeout = widget_timeout;
        }
        return pump_options;
    };
    auto pump_if_needed = [&]() -> SP::Expected<void> {
        if (!readiness_options.pump_scene_before_force_publish) {
            return {};
        }
        auto pump_options = make_pump_options();
        auto pump_result = SP::UI::Declarative::SceneLifecycle::PumpSceneOnce(space, scene, pump_options);
        if (!pump_result) {
            auto const& error = pump_result.error();
            if (error.code == SP::Error::Code::Timeout) {
                return std::unexpected(error);
            }
            return std::unexpected(error);
        }
        return {};
    };
    if (readiness_options.pump_scene_before_force_publish) {
        auto pump_status = pump_if_needed();
        if (!pump_status) {
            last_error = pump_status.error();
        }
    }
    while (std::chrono::steady_clock::now() < deadline) {
        auto forced = SP::UI::Declarative::SceneLifecycle::ForcePublish(space, scene, publish_options);
        if (forced) {
            return forced;
        }
        last_error = forced.error();
        if (last_error.code == SP::Error::Code::NoObjectFound
            && readiness_options.pump_scene_before_force_publish) {
            auto pump_status = pump_if_needed();
            if (!pump_status) {
                last_error = pump_status.error();
                if (last_error.code != SP::Error::Code::Timeout
                    && last_error.code != SP::Error::Code::NoObjectFound) {
                    return std::unexpected(last_error);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
            continue;
        }
        if (last_error.code != SP::Error::Code::NoObjectFound
            && last_error.code != SP::Error::Code::Timeout) {
            auto diag = read_scene_lifecycle_diagnostics(space, scene);
            if (!diag.empty()) {
                if (last_error.message) {
                    last_error.message = *last_error.message + "; " + diag;
                } else {
                    last_error.message = diag;
                }
            }
            return std::unexpected(last_error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    auto diag = read_scene_lifecycle_diagnostics(space, scene);
    if (!diag.empty()) {
        if (last_error.message) {
            last_error.message = *last_error.message + "; " + diag;
        } else {
            last_error.message = diag;
        }
    }
    return std::unexpected(last_error);
}

auto EnsureDeclarativeSceneReady(SP::PathSpace& space,
                                 SP::UI::ScenePath const& scene,
                                 SP::UI::WindowPath const& window,
                                 std::string const& view_name,
                                 DeclarativeReadinessOptions const& options)
    -> SP::Expected<DeclarativeReadinessResult> {
    DeclarativeReadinessResult result{};
    result.widget_count = CountWindowWidgets(space, window, view_name);
    auto scene_window_component = options.scene_window_component_override
        ? *options.scene_window_component_override
        : WindowComponentName(std::string(window.getPath()));
    auto scene_view_name = options.scene_view_override
        ? *options.scene_view_override
        : view_name;
    if (options.wait_for_runtime_metrics) {
        auto metrics_ready = WaitForRuntimeMetricsReady(space, options.runtime_metrics_timeout);
        if (!metrics_ready) {
            return std::unexpected(metrics_ready.error());
        }
    }
    if (readiness_skip_requested()) {
        return result;
    }
    if (result.widget_count == 0) {
        return result;
    }
    std::optional<std::uint64_t> publish_revision;
    if (options.force_scene_publish) {
        auto forced = force_scene_publish_with_retry(space,
                                                     scene,
                                                     options.widget_timeout,
                                                     options.revision_timeout,
                                                     options.min_revision,
                                                     options);
        if (!forced) {
            return std::unexpected(forced.error());
        }
        publish_revision = *forced;
    }
    if (options.wait_for_buckets && !options.force_scene_publish) {
        auto status = WaitForDeclarativeWidgetBuckets(space,
                                                      scene,
                                                      result.widget_count,
                                                      options.widget_timeout);
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    if (options.wait_for_revision) {
        if (publish_revision) {
            result.scene_revision = *publish_revision;
        } else {
            auto revision = WaitForDeclarativeSceneRevision(space,
                                                            scene,
                                                            options.revision_timeout,
                                                            options.min_revision);
            if (!revision) {
                return std::unexpected(revision.error());
            }
            result.scene_revision = *revision;
        }
    }
    if (options.wait_for_structure && !options.force_scene_publish) {
        auto scene_widgets_root = MakeSceneWidgetsRootComponents(scene,
                                                                 scene_window_component,
                                                                 scene_view_name);
        auto status = WaitForDeclarativeSceneWidgets(space,
                                                     scene_widgets_root,
                                                     result.widget_count,
                                                     options.widget_timeout);
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    record_manual_pump_metrics(space, window, view_name);
    return result;
}

} // namespace SP::UI::Declarative
