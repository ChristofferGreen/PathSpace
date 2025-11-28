#include "Helpers.hpp"

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/Detail.hpp>

namespace SP::UI {

namespace {

auto root_view(AppRootPath const& root) -> Runtime::AppRootPathView {
    return Runtime::AppRootPathView{root.getPath()};
}

auto path_view(ConcretePath const& path) -> Runtime::ConcretePathView {
    return Runtime::ConcretePathView{path.getPath()};
}

auto relative_to_root(SP::App::AppRootPathView root,
                      ConcretePathView absolute) -> SP::Expected<std::string> {
    if (auto status = SP::App::ensure_within_app(root, absolute); !status) {
        return std::unexpected(status.error());
    }

    auto const root_path = root.getPath();
    auto const absolute_path = absolute.getPath();
    if (absolute_path.size() == root_path.size()) {
        return std::string{};
    }
    // ensure_absolute_path starts with root_path + '/'
    if (absolute_path[root_path.size()] != '/') {
        return std::unexpected(
            SP::UI::Declarative::Detail::make_error(
                "path '" + std::string(absolute_path) + "' is not nested under application root '"
                + std::string(root_path) + "'",
                SP::Error::Code::InvalidPath));
    }
    return std::string(absolute_path.substr(root_path.size() + 1));
}

} // namespace

namespace Scene {

auto Create(PathSpace& space, AppRootPath const& appRoot, SceneParams const& params) -> SP::Expected<ScenePath> {
    return Runtime::Scene::Create(space, root_view(appRoot), params);
}

auto EnsureAuthoringRoot(PathSpace& space, ScenePath const& scenePath) -> SP::Expected<void> {
    return Runtime::Scene::EnsureAuthoringRoot(space, scenePath);
}

auto PublishRevision(PathSpace& space,
                     ScenePath const& scenePath,
                     SceneRevisionDesc const& revision,
                     std::span<std::byte const> drawableBucket,
                     std::span<std::byte const> metadata) -> SP::Expected<void> {
    return Runtime::Scene::PublishRevision(space, scenePath, revision, drawableBucket, metadata);
}

auto ReadCurrentRevision(PathSpace const& space, ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc> {
    return Runtime::Scene::ReadCurrentRevision(space, scenePath);
}

auto WaitUntilReady(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return Runtime::Scene::WaitUntilReady(space, scenePath, timeout);
}

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath> {
    return Runtime::Renderer::Create(space, root_view(appRoot), params);
}

auto ResolveTargetBase(PathSpace const& space,
                       AppRootPath const& appRoot,
                       RendererPath const& rendererPath,
                       std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    return Runtime::Renderer::ResolveTargetBase(space, root_view(appRoot), rendererPath, targetSpec);
}

auto UpdateSettings(PathSpace& space,
                    ConcretePath const& targetPath,
                    RenderSettings const& settings) -> SP::Expected<void> {
    return Runtime::Renderer::UpdateSettings(space, path_view(targetPath), settings);
}

auto ReadSettings(PathSpace const& space,
                  ConcretePath const& targetPath) -> SP::Expected<RenderSettings> {
    return Runtime::Renderer::ReadSettings(space, path_view(targetPath));
}

auto TriggerRender(PathSpace& space,
                   ConcretePath const& targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    return Runtime::Renderer::TriggerRender(space, path_view(targetPath), settings);
}

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    return Runtime::Surface::Create(space, root_view(appRoot), params);
}

auto SetScene(PathSpace& space,
              SurfacePath const& surfacePath,
              ScenePath const& scenePath) -> SP::Expected<void> {
    namespace Detail = SP::UI::Declarative::Detail;

    auto surfaceRoot = Detail::derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    auto sceneRoot = Detail::derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }
    if (surfaceRoot->getPath() != sceneRoot->getPath()) {
        return std::unexpected(
            Detail::make_error("surface and scene belong to different applications",
                               SP::Error::Code::InvalidPath));
    }

    auto rootView = SP::App::AppRootPathView{surfaceRoot->getPath()};
    auto sceneRelative = relative_to_root(rootView, ConcretePathView{scenePath.getPath()});
    if (!sceneRelative) {
        return std::unexpected(sceneRelative.error());
    }

    auto sceneField = std::string(surfacePath.getPath()) + "/scene";
    if (auto status = Detail::replace_single<std::string>(space, sceneField, *sceneRelative); !status) {
        return status;
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = space.read<std::string, std::string>(targetField);
    if (!targetRelative) {
        if (targetRelative.error().code == SP::Error::Code::NoObjectFound) {
            return std::unexpected(
                Detail::make_error("surface missing target binding", SP::Error::Code::InvalidPath));
        }
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(rootView, *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto targetScenePath = std::string(targetAbsolute->getPath()) + "/scene";
    return Detail::replace_single<std::string>(space, targetScenePath, *sceneRelative);
}

auto RenderOnce(PathSpace& space,
                SurfacePath const& surfacePath,
                std::optional<RenderSettings> settingsOverride) -> SP::Expected<SP::FutureAny> {
    return Runtime::Surface::RenderOnce(space, surfacePath, settingsOverride);
}

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            WindowParams const& params) -> SP::Expected<WindowPath> {
    return Runtime::Window::Create(space, root_view(appRoot), params);
}

auto AttachSurface(PathSpace& space,
                   WindowPath const& windowPath,
                   std::string_view viewName,
                   SurfacePath const& surfacePath) -> SP::Expected<void> {
    return Runtime::Window::AttachSurface(space, windowPath, viewName, surfacePath);
}

auto AttachHtmlTarget(PathSpace& space,
                      WindowPath const& windowPath,
                      std::string_view viewName,
                      HtmlTargetPath const& targetPath) -> SP::Expected<void> {
    return Runtime::Window::AttachHtmlTarget(space, windowPath, viewName, targetPath);
}

auto Present(PathSpace& space,
             WindowPath const& windowPath,
             std::string_view viewName) -> SP::Expected<Runtime::Window::WindowPresentResult> {
    return Runtime::Window::Present(space, windowPath, viewName);
}

} // namespace Window

namespace Diagnostics {

auto ReadTargetMetrics(PathSpace const& space,
                       ConcretePath const& targetPath) -> SP::Expected<TargetMetrics> {
    return Runtime::Diagnostics::ReadTargetMetrics(space, path_view(targetPath));
}

auto ClearTargetError(PathSpace& space,
                      ConcretePath const& targetPath) -> SP::Expected<void> {
    return Runtime::Diagnostics::ClearTargetError(space, path_view(targetPath));
}

} // namespace Diagnostics

} // namespace SP::UI
