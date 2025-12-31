#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/SceneReadiness.hpp>

#include <span>
#include <string>

namespace PathSpaceExamples {

using DeclarativeReadinessOptions = SP::UI::Declarative::DeclarativeReadinessOptions;
using DeclarativeReadinessResult = SP::UI::Declarative::DeclarativeReadinessResult;

inline std::string make_window_view_path(SP::UI::WindowPath const& window,
                                         std::string const& view_name) {
    return SP::UI::Declarative::MakeWindowViewPath(window, view_name);
}

inline std::string window_component_name(std::string const& window_path) {
    return SP::UI::Declarative::WindowComponentName(window_path);
}

inline auto ensure_declarative_scene_ready(SP::PathSpace& space,
                                           SP::UI::ScenePath const& scene,
                                           SP::UI::WindowPath const& window,
                                           std::string const& view_name,
                                           DeclarativeReadinessOptions const& options = {})
    -> SP::Expected<DeclarativeReadinessResult> {
    return SP::UI::Declarative::EnsureDeclarativeSceneReady(space, scene, window, view_name, options);
}

inline auto force_window_software_renderer(SP::PathSpace& space,
                                           SP::UI::WindowPath const& window,
                                           std::string const& view_name) -> SP::Expected<void> {
    // Hint the renderer to use the software backend; tolerate missing paths.
    auto renderer_path = std::string(window.getPath()) + "/views/" + view_name + "/config/renderer/default";
    auto set = space.insert(renderer_path, std::string{"software"});
    if (!set.errors.empty()) {
        return std::unexpected(set.errors.front());
    }
    return {};
}

// No-op shims for devices; PathSpace UI runtime covers device wiring elsewhere.
inline void ensure_device_push_config(SP::PathSpace&, std::string const&, std::string const&) {}
inline void subscribe_window_devices(SP::PathSpace&,
                                     SP::UI::WindowPath const&,
                                     std::span<const std::string>,
                                     std::span<const std::string>,
                                     std::span<const std::string>) {}

} // namespace PathSpaceExamples
