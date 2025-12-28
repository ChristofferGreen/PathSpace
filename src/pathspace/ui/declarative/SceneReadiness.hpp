#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Declarative {

struct DeclarativeReadinessOptions {
    std::chrono::milliseconds widget_timeout{std::chrono::milliseconds(5000)};
    std::chrono::milliseconds revision_timeout{std::chrono::milliseconds(3000)};
    // Renderer snapshots now live outside PathSpace; default waits skip structure/bucket mirrors.
    bool wait_for_structure = false;
    bool wait_for_buckets = false;
    bool wait_for_revision = true;
    bool wait_for_runtime_metrics = false;
    std::chrono::milliseconds runtime_metrics_timeout{std::chrono::milliseconds(2000)};
    std::optional<std::uint64_t> min_revision;
    bool ensure_scene_window_mirror = false;
    std::optional<std::string> scene_window_component_override;
    std::optional<std::string> scene_view_override;
    bool force_scene_publish = false;
    bool pump_scene_before_force_publish = true;
    SP::UI::Declarative::SceneLifecycle::ManualPumpOptions scene_pump_options{};
};

struct DeclarativeReadinessResult {
    std::size_t widget_count = 0;
    std::optional<std::uint64_t> scene_revision;
};

auto MakeWindowViewPath(SP::UI::WindowPath const& window,
                        std::string const& view_name) -> std::string;
auto WindowComponentName(std::string const& window_path) -> std::string;
auto AppRootFromWindow(SP::UI::WindowPath const& window) -> std::string;
auto MakeSceneWidgetsRootComponents(SP::UI::ScenePath const& scene,
                                    std::string_view window_component,
                                    std::string_view view_name) -> std::string;
auto MakeSceneWidgetsRoot(SP::UI::ScenePath const& scene,
                          SP::UI::WindowPath const& window,
                          std::string const& view_name) -> std::string;

auto CountWindowWidgets(SP::PathSpace& space,
                        SP::UI::WindowPath const& window,
                        std::string const& view_name) -> std::size_t;

auto WaitForRuntimeMetricsReady(SP::PathSpace& space,
                                std::chrono::milliseconds timeout) -> SP::Expected<void>;

auto WaitForDeclarativeSceneWidgets(SP::PathSpace& space,
                                    std::string const& widgets_root,
                                    std::size_t expected_widgets,
                                    std::chrono::milliseconds timeout) -> SP::Expected<void>;

auto WaitForDeclarativeWidgetBuckets(SP::PathSpace& space,
                                     SP::UI::ScenePath const& scene,
                                     std::size_t expected_widgets,
                                     std::chrono::milliseconds timeout) -> SP::Expected<void>;

auto WaitForDeclarativeSceneRevision(SP::PathSpace& space,
                                     SP::UI::ScenePath const& scene,
                                     std::chrono::milliseconds timeout,
                                     std::optional<std::uint64_t> min_revision = std::nullopt)
    -> SP::Expected<std::uint64_t>;

auto EnsureDeclarativeSceneReady(SP::PathSpace& space,
                                 SP::UI::ScenePath const& scene,
                                 SP::UI::WindowPath const& window,
                                 std::string const& view_name,
                                 DeclarativeReadinessOptions const& options = {})
    -> SP::Expected<DeclarativeReadinessResult>;

} // namespace SP::UI::Declarative
