#pragma once

#include "PathSpace.hpp"

#include "path/GlobPath.hpp"

#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

namespace SP::UI {

using SceneParams = Runtime::SceneParams;
using SceneRevisionDesc = Runtime::SceneRevisionDesc;
using RendererParams = Runtime::RendererParams;
using RendererKind = Runtime::RendererKind;
using SurfaceDesc = Runtime::SurfaceDesc;
using SurfaceParams = Runtime::SurfaceParams;
using WindowParams = Runtime::WindowParams;
using RenderSettings = Runtime::RenderSettings;
using WindowPresentResult = Runtime::Window::WindowPresentResult;

namespace Scene {

auto Create(PathSpace& space, AppRootPath const& appRoot, SceneParams const& params) -> SP::Expected<ScenePath>;

auto EnsureAuthoringRoot(PathSpace& space, ScenePath const& scenePath) -> SP::Expected<void>;

auto PublishRevision(PathSpace& space,
                     ScenePath const& scenePath,
                     SceneRevisionDesc const& revision,
                     std::span<std::byte const> drawableBucket,
                     std::span<std::byte const> metadata) -> SP::Expected<void>;

auto ReadCurrentRevision(PathSpace const& space, ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc>;

auto WaitUntilReady(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<void>;

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            RendererParams const& params) -> SP::Expected<RendererPath>;

auto ResolveTargetBase(PathSpace const& space,
                       AppRootPath const& appRoot,
                       RendererPath const& rendererPath,
                       std::string_view targetSpec) -> SP::Expected<ConcretePath>;

auto UpdateSettings(PathSpace& space,
                    ConcretePath const& targetPath,
                    RenderSettings const& settings) -> SP::Expected<void>;

auto ReadSettings(PathSpace const& space,
                  ConcretePath const& targetPath) -> SP::Expected<RenderSettings>;

auto TriggerRender(PathSpace& space,
                   ConcretePath const& targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny>;

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            SurfaceParams const& params) -> SP::Expected<SurfacePath>;

auto SetScene(PathSpace& space,
              SurfacePath const& surfacePath,
              ScenePath const& scenePath) -> SP::Expected<void>;

auto RenderOnce(PathSpace& space,
                SurfacePath const& surfacePath,
                std::optional<RenderSettings> settingsOverride = std::nullopt) -> SP::Expected<SP::FutureAny>;

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
            AppRootPath const& appRoot,
            WindowParams const& params) -> SP::Expected<WindowPath>;

auto AttachSurface(PathSpace& space,
                   WindowPath const& windowPath,
                   std::string_view viewName,
                   SurfacePath const& surfacePath) -> SP::Expected<void>;

auto AttachHtmlTarget(PathSpace& space,
                      WindowPath const& windowPath,
                      std::string_view viewName,
                      HtmlTargetPath const& targetPath) -> SP::Expected<void>;

auto Present(PathSpace& space,
             WindowPath const& windowPath,
             std::string_view viewName) -> SP::Expected<WindowPresentResult>;

} // namespace Window

namespace Diagnostics {

using TargetMetrics = Runtime::Diagnostics::TargetMetrics;

auto ReadTargetMetrics(PathSpace const& space,
                       ConcretePath const& targetPath) -> SP::Expected<TargetMetrics>;

auto ClearTargetError(PathSpace& space,
                      ConcretePath const& targetPath) -> SP::Expected<void>;

} // namespace Diagnostics

} // namespace SP::UI
