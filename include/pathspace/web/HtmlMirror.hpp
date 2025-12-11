#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace SP::ServeHtml {

struct HtmlMirrorConfig {
    std::string renderer_name{"html"};
    std::string target_name{"web"};
    std::string view_name{"web"};
};

struct HtmlMirrorContext {
    SP::App::AppRootPath              app_root;
    SP::UI::Runtime::WindowPath       window;
    std::string                       view_name;
    SP::UI::Runtime::RendererPath     renderer;
    SP::UI::Runtime::HtmlTargetPath   target;
};

inline auto MakeAppRelativePath(std::string_view absolute, std::string_view app_root) -> std::string {
    if (absolute.size() < app_root.size() || absolute.compare(0, app_root.size(), app_root) != 0) {
        return std::string{absolute};
    }
    auto remainder = absolute.substr(app_root.size());
    if (!remainder.empty() && remainder.front() == '/') {
        remainder.remove_prefix(1);
    }
    return std::string{remainder};
}

inline auto CreateHtmlMirrorTargets(SP::PathSpace&                  space,
                                    SP::App::AppRootPath const&     app_root,
                                    SP::UI::Runtime::WindowPath const& window_path,
                                    SP::UI::Runtime::ScenePath const&  scene_path,
                                    HtmlMirrorConfig const&         config)
    -> SP::Expected<HtmlMirrorContext> {
    auto renderer_name = config.renderer_name.empty() ? std::string{"html"} : config.renderer_name;
    auto target_name   = config.target_name.empty() ? std::string{"web"} : config.target_name;
    auto view_name     = config.view_name.empty() ? std::string{"web"} : config.view_name;

    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    SP::UI::Runtime::RendererParams renderer_params{
        .name        = renderer_name,
        .kind        = SP::UI::Runtime::RendererKind::Software2D,
        .description = "HTML mirror renderer",
    };
    auto renderer_path = SP::UI::Runtime::Renderer::Create(space, app_root_view, renderer_params);
    if (!renderer_path) {
        return std::unexpected(renderer_path.error());
    }

    auto scene_relative = MakeAppRelativePath(scene_path.getPath(), app_root.getPath());
    if (scene_relative.empty()) {
        return std::unexpected(
            SP::Error{SP::Error::Code::InvalidError, "scene path not relative to app root"});
    }

    SP::UI::Runtime::HtmlTargetParams target_params{};
    target_params.name  = target_name;
    target_params.scene = std::move(scene_relative);

    auto html_target = SP::UI::Runtime::Renderer::CreateHtmlTarget(space,
                                                                   app_root_view,
                                                                   *renderer_path,
                                                                   target_params);
    if (!html_target) {
        return std::unexpected(html_target.error());
    }

    auto attached = SP::UI::Runtime::Window::AttachHtmlTarget(space,
                                                              window_path,
                                                              view_name,
                                                              *html_target);
    if (!attached) {
        return std::unexpected(attached.error());
    }

    return HtmlMirrorContext{
        .app_root = app_root,
        .window   = window_path,
        .view_name = std::move(view_name),
        .renderer = *renderer_path,
        .target   = *html_target,
    };
}

inline auto SetupHtmlMirror(SP::PathSpace&                  space,
                            SP::App::AppRootPath const&     app_root,
                            SP::UI::Runtime::WindowPath const& window_path,
                            SP::UI::Runtime::ScenePath const&  scene_path,
                            HtmlMirrorConfig const&         config)
    -> SP::Expected<HtmlMirrorContext> {
    return CreateHtmlMirrorTargets(space, app_root, window_path, scene_path, config);
}

inline auto PresentHtmlMirror(SP::PathSpace& space, HtmlMirrorContext const& context)
    -> SP::Expected<void> {
    auto present = SP::UI::Runtime::Window::Present(space, context.window, context.view_name);
    if (!present) {
        return std::unexpected(present.error());
    }
    return {};
}

} // namespace SP::ServeHtml
