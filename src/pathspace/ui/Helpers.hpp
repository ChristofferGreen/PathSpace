#pragma once

#include "PathSpace.hpp"

#include "path/GlobPath.hpp"

#include <pathspace/ui/Builders.hpp>

namespace SP::UI {

using ConcretePath = SP::ConcretePathString;
using ConcretePathView = SP::ConcretePathStringView;
using GlobPath = SP::GlobPathString;
using GlobPathView = SP::GlobPathStringView;

using AppRootPath = Builders::AppRootPath;
using ScenePath = Builders::ScenePath;
using RendererPath = Builders::RendererPath;
using SurfacePath = Builders::SurfacePath;
using WindowPath = Builders::WindowPath;
using HtmlTargetPath = Builders::HtmlTargetPath;

using SceneParams = Builders::SceneParams;
using SceneRevisionDesc = Builders::SceneRevisionDesc;
using RendererParams = Builders::RendererParams;
using RendererKind = Builders::RendererKind;
using SurfaceDesc = Builders::SurfaceDesc;
using SurfaceParams = Builders::SurfaceParams;
using WindowParams = Builders::WindowParams;
using RenderSettings = Builders::RenderSettings;
using WindowPresentResult = Builders::Window::WindowPresentResult;

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
            RendererParams const& params,
            RendererKind kind) -> SP::Expected<RendererPath>;

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

using TargetMetrics = Builders::Diagnostics::TargetMetrics;

auto ReadTargetMetrics(PathSpace const& space,
                       ConcretePath const& targetPath) -> SP::Expected<TargetMetrics>;

auto ClearTargetError(PathSpace& space,
                      ConcretePath const& targetPath) -> SP::Expected<void>;

} // namespace Diagnostics

} // namespace SP::UI
