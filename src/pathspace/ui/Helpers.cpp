#include "Helpers.hpp"

#include <pathspace/ui/Builders.hpp>

namespace SP::UI {

namespace {

auto root_view(AppRootPath const& root) -> Builders::AppRootPathView {
    return Builders::AppRootPathView{root.getPath()};
}

auto path_view(ConcretePath const& path) -> Builders::ConcretePathView {
    return Builders::ConcretePathView{path.getPath()};
}

} // namespace

namespace Scene {

auto Create(PathSpace& space, AppRootPath const& appRoot, SceneParams const& params) -> SP::Expected<ScenePath> {
    return Builders::Scene::Create(space, root_view(appRoot), params);
}

auto EnsureAuthoringRoot(PathSpace& space, ScenePath const& scenePath) -> SP::Expected<void> {
    return Builders::Scene::EnsureAuthoringRoot(space, scenePath);
}

auto PublishRevision(PathSpace& space,
                     ScenePath const& scenePath,
                     SceneRevisionDesc const& revision,
                     std::span<std::byte const> drawableBucket,
                     std::span<std::byte const> metadata) -> SP::Expected<void> {
    return Builders::Scene::PublishRevision(space, scenePath, revision, drawableBucket, metadata);
}

auto ReadCurrentRevision(PathSpace const& space, ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc> {
    return Builders::Scene::ReadCurrentRevision(space, scenePath);
}

auto WaitUntilReady(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<void> {
    return Builders::Scene::WaitUntilReady(space, scenePath, timeout);
}

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath> {
    return Builders::Renderer::Create(space, root_view(appRoot), params);
}

auto ResolveTargetBase(PathSpace const& space,
                       AppRootPath const& appRoot,
                       RendererPath const& rendererPath,
                       std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    return Builders::Renderer::ResolveTargetBase(space, root_view(appRoot), rendererPath, targetSpec);
}

auto UpdateSettings(PathSpace& space,
                    ConcretePath const& targetPath,
                    RenderSettings const& settings) -> SP::Expected<void> {
    return Builders::Renderer::UpdateSettings(space, path_view(targetPath), settings);
}

auto ReadSettings(PathSpace const& space,
                  ConcretePath const& targetPath) -> SP::Expected<RenderSettings> {
    return Builders::Renderer::ReadSettings(space, path_view(targetPath));
}

auto TriggerRender(PathSpace& space,
                   ConcretePath const& targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    return Builders::Renderer::TriggerRender(space, path_view(targetPath), settings);
}

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    return Builders::Surface::Create(space, root_view(appRoot), params);
}

auto SetScene(PathSpace& space,
              SurfacePath const& surfacePath,
              ScenePath const& scenePath) -> SP::Expected<void> {
    return Builders::Surface::SetScene(space, surfacePath, scenePath);
}

auto RenderOnce(PathSpace& space,
                SurfacePath const& surfacePath,
                std::optional<RenderSettings> settingsOverride) -> SP::Expected<SP::FutureAny> {
    return Builders::Surface::RenderOnce(space, surfacePath, settingsOverride);
}

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            WindowParams const& params) -> SP::Expected<WindowPath> {
    return Builders::Window::Create(space, root_view(appRoot), params);
}

auto AttachSurface(PathSpace& space,
                   WindowPath const& windowPath,
                   std::string_view viewName,
                   SurfacePath const& surfacePath) -> SP::Expected<void> {
    return Builders::Window::AttachSurface(space, windowPath, viewName, surfacePath);
}

auto AttachHtmlTarget(PathSpace& space,
                      WindowPath const& windowPath,
                      std::string_view viewName,
                      HtmlTargetPath const& targetPath) -> SP::Expected<void> {
    return Builders::Window::AttachHtmlTarget(space, windowPath, viewName, targetPath);
}

auto Present(PathSpace& space,
             WindowPath const& windowPath,
             std::string_view viewName) -> SP::Expected<Builders::Window::WindowPresentResult> {
    return Builders::Window::Present(space, windowPath, viewName);
}

} // namespace Window

namespace Diagnostics {

auto ReadTargetMetrics(PathSpace const& space,
                       ConcretePath const& targetPath) -> SP::Expected<TargetMetrics> {
    return Builders::Diagnostics::ReadTargetMetrics(space, path_view(targetPath));
}

auto ClearTargetError(PathSpace& space,
                      ConcretePath const& targetPath) -> SP::Expected<void> {
    return Builders::Diagnostics::ClearTargetError(space, path_view(targetPath));
}

} // namespace Diagnostics

} // namespace SP::UI
