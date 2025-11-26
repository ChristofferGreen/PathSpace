#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathTypes.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace SP::UI::Declarative::SceneLifecycle {

struct Options {
    std::chrono::milliseconds trellis_wait = std::chrono::milliseconds{5};
    std::chrono::milliseconds publish_throttle = std::chrono::milliseconds{0};
};

struct ForcePublishOptions {
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds{1500};
    std::optional<std::uint64_t> min_revision;
};

struct ManualPumpOptions {
    std::chrono::milliseconds wait_timeout = std::chrono::milliseconds{0};
    bool mark_all_widgets_dirty = true;
};

struct ManualPumpResult {
    std::uint64_t widgets_processed = 0;
    std::uint64_t buckets_ready = 0;
};

[[nodiscard]] auto Start(PathSpace& space,
                         SP::App::AppRootPathView app_root,
                         SP::UI::ScenePath const& scene_path,
                         SP::UI::WindowPath const& window_path,
                         std::string_view view_name,
                         Options const& options = {}) -> SP::Expected<void>;

[[nodiscard]] auto Stop(PathSpace& space,
                        SP::UI::ScenePath const& scene_path) -> SP::Expected<void>;

[[nodiscard]] auto ForcePublish(PathSpace& space,
                                SP::UI::ScenePath const& scene_path,
                                ForcePublishOptions const& options = {}) -> SP::Expected<std::uint64_t>;

[[nodiscard]] auto PumpSceneOnce(PathSpace& space,
                                 SP::UI::ScenePath const& scene_path,
                                 ManualPumpOptions const& options = {}) -> SP::Expected<ManualPumpResult>;

auto InvalidateThemes(PathSpace& space,
                      SP::App::AppRootPathView app_root) -> void;

auto StopAll(PathSpace& space) -> void;

} // namespace SP::UI::Declarative::SceneLifecycle
