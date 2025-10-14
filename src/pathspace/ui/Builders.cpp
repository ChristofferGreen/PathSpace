#include <pathspace/ui/Builders.hpp>

#include <algorithm>

namespace SP::UI::Builders {

namespace {

auto make_error(std::string message, SP::Error::Code code = SP::Error::Code::InvalidPath) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

auto ensure_non_empty(std::string_view value, std::string_view what) -> SP::Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(std::string(what) + " must not be empty"));
    }
    return {};
}

auto combine_relative(AppRootPathView root, std::string relative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, relative);
}

auto relative_to_root(AppRootPathView root, ConcretePathView absolute) -> SP::Expected<std::string> {
    auto status = SP::App::ensure_within_app(root, absolute);
    if (!status) {
        return std::unexpected(status.error());
    }
    auto const& rootStr = root.getPath();
    auto const& absStr  = absolute.getPath();
    if (absStr.size() == rootStr.size()) {
        return std::string{};
    }
    return std::string(absStr.substr(rootStr.size() + 1));
}

auto ensure_contains_segment(ConcretePathView path, std::string_view segment) -> SP::Expected<void> {
    if (path.getPath().find(segment) == std::string::npos) {
        return std::unexpected(make_error("path '" + std::string(path.getPath()) + "' missing segment '" + std::string(segment) + "'"));
    }
    return {};
}

auto derive_app_root_for(ConcretePathView absolute) -> SP::Expected<AppRootPath> {
    return SP::App::derive_app_root(absolute);
}

} // namespace

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, maybeRelative);
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath> {
    return SP::App::derive_target_base(root, rendererPath, targetPath);
}

namespace Scene {

auto Create(PathSpace& /*space*/,
             AppRootPathView appRoot,
             SceneParams const& params) -> SP::Expected<ScenePath> {
    if (auto status = ensure_non_empty(params.name, "scene name"); !status) {
        return std::unexpected(status.error());
    }
    auto relative = std::string("scenes/") + params.name;
    auto resolved = combine_relative(appRoot, std::move(relative));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return ScenePath{resolved->getPath()};
}

auto EnsureAuthoringRoot(PathSpace& /*space*/,
                          ScenePath const& scenePath) -> SP::Expected<void> {
    if (!scenePath.isValid()) {
        return std::unexpected(make_error("scene path is not a valid concrete path"));
    }
    if (auto status = ensure_contains_segment(scenePath, "/scenes/"); !status) {
        return std::unexpected(status.error());
    }
    auto root = derive_app_root_for(scenePath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return {};
}

auto PublishRevision(PathSpace& space,
                      ScenePath const& scenePath,
                      SceneRevisionDesc const& /*revision*/,
                      std::span<std::byte const> /*drawableBucket*/,
                      std::span<std::byte const> /*metadata*/) -> SP::Expected<void> {
    return EnsureAuthoringRoot(space, scenePath);
}

auto ReadCurrentRevision(PathSpace const& /*space*/,
                          ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc> {
    if (!scenePath.isValid()) {
        return std::unexpected(make_error("scene path is not a valid concrete path"));
    }
    SceneRevisionDesc desc{};
    desc.author = "unknown";
    return desc;
}

auto WaitUntilReady(PathSpace& space,
                     ScenePath const& scenePath,
                     std::chrono::milliseconds /*timeout*/) -> SP::Expected<void> {
    return EnsureAuthoringRoot(space, scenePath);
}

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& /*space*/,
             AppRootPathView appRoot,
             RendererParams const& params,
             RendererKind /*kind*/) -> SP::Expected<RendererPath> {
    if (auto status = ensure_non_empty(params.name, "renderer name"); !status) {
        return std::unexpected(status.error());
    }
    auto relative = std::string("renderers/") + params.name;
    auto resolved = combine_relative(appRoot, std::move(relative));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return RendererPath{resolved->getPath()};
}

auto ResolveTargetBase(PathSpace const& /*space*/,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    if (auto status = ensure_non_empty(targetSpec, "target spec"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = SP::App::ensure_within_app(appRoot, rendererPath); !status) {
        return std::unexpected(status.error());
    }

    if (!targetSpec.empty() && targetSpec.front() == '/') {
        auto resolved = combine_relative(appRoot, std::string(targetSpec));
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        return *resolved;
    }

    auto rendererRelative = relative_to_root(appRoot, rendererPath);
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string combined = *rendererRelative;
    if (!combined.empty()) {
        combined.push_back('/');
    }
    combined.append(targetSpec);

    auto resolved = combine_relative(appRoot, std::move(combined));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return *resolved;
}

auto UpdateSettings(PathSpace& /*space*/,
                     ConcretePathView targetPath,
                     RenderSettings const& /*settings*/) -> SP::Expected<void> {
    auto root = derive_app_root_for(targetPath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return {};
}

auto ReadSettings(PathSpace const& /*space*/,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings> {
    auto root = derive_app_root_for(targetPath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return RenderSettings{};
}

auto TriggerRender(PathSpace& /*space*/,
                    ConcretePathView targetPath) -> SP::Expected<SP::FutureAny> {
    auto root = derive_app_root_for(targetPath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return std::unexpected(make_error("render execution helpers not available",
                                      SP::Error::Code::UnknownError));
}

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& /*space*/,
             AppRootPathView appRoot,
             SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    if (auto status = ensure_non_empty(params.name, "surface name"); !status) {
        return std::unexpected(status.error());
    }
    auto relative = std::string("surfaces/") + params.name;
    auto resolved = combine_relative(appRoot, std::move(relative));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return SurfacePath{resolved->getPath()};
}

auto SetScene(PathSpace& /*space*/,
               SurfacePath const& surfacePath,
               ScenePath const& scenePath) -> SP::Expected<void> {
    auto surfaceRoot = derive_app_root_for(surfacePath);
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    auto sceneRoot = derive_app_root_for(scenePath);
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }
    if (surfaceRoot->getPath() != sceneRoot->getPath()) {
        return std::unexpected(make_error("surface and scene belong to different applications"));
    }
    return {};
}

auto RenderOnce(PathSpace& /*space*/,
                 SurfacePath const& surfacePath,
                 std::optional<RenderSettings> /*settingsOverride*/) -> SP::Expected<SP::FutureAny> {
    auto root = derive_app_root_for(surfacePath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return std::unexpected(make_error("render helpers not implemented", SP::Error::Code::UnknownError));
}

} // namespace Surface

namespace Window {

auto Create(PathSpace& /*space*/,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath> {
    if (auto status = ensure_non_empty(params.name, "window name"); !status) {
        return std::unexpected(status.error());
    }
    auto relative = std::string("windows/") + params.name;
    auto resolved = combine_relative(appRoot, std::move(relative));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return WindowPath{resolved->getPath()};
}

auto AttachSurface(PathSpace& /*space*/,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void> {
    if (auto status = ensure_non_empty(viewName, "view name"); !status) {
        return std::unexpected(status.error());
    }
    auto windowRoot = derive_app_root_for(windowPath);
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }
    auto surfaceRoot = derive_app_root_for(surfacePath);
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    if (windowRoot->getPath() != surfaceRoot->getPath()) {
        return std::unexpected(make_error("window and surface belong to different applications"));
    }
    return {};
}

auto Present(PathSpace& /*space*/,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<void> {
    if (auto status = ensure_non_empty(viewName, "view name"); !status) {
        return std::unexpected(status.error());
    }
    auto windowRoot = derive_app_root_for(windowPath);
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }
    return {};
}

} // namespace Window

namespace Diagnostics {

auto ReadTargetMetrics(PathSpace const& /*space*/,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics> {
    auto root = derive_app_root_for(targetPath);
    if (!root) {
        return std::unexpected(root.error());
    }
    TargetMetrics metrics{};
    metrics.last_error.clear();
    return metrics;
}

auto ClearTargetError(PathSpace& /*space*/,
                       ConcretePathView targetPath) -> SP::Expected<void> {
    auto root = derive_app_root_for(targetPath);
    if (!root) {
        return std::unexpected(root.error());
    }
    return {};
}

} // namespace Diagnostics

} // namespace SP::UI::Builders
