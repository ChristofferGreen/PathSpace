#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>

#include <chrono>
#include <string>

namespace SP::UI::Declarative::SceneLifecycle {

struct Options {
    std::chrono::milliseconds trellis_wait = std::chrono::milliseconds{5};
};

[[nodiscard]] auto Start(PathSpace& space,
                         SP::App::AppRootPathView app_root,
                         SP::UI::Builders::ScenePath const& scene_path,
                         SP::UI::Builders::WindowPath const& window_path,
                         std::string_view view_name,
                         Options const& options = {}) -> SP::Expected<void>;

[[nodiscard]] auto Stop(PathSpace& space,
                        SP::UI::Builders::ScenePath const& scene_path) -> SP::Expected<void>;

} // namespace SP::UI::Declarative::SceneLifecycle
