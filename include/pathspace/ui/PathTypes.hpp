#pragma once

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/path/ConcretePath.hpp>

namespace SP::UI {

using AppRootPath = SP::App::AppRootPath;
using AppRootPathView = SP::App::AppRootPathView;

using ConcretePath = SP::ConcretePathString;
using ConcretePathView = SP::ConcretePathStringView;

using ScenePath = SP::ConcretePathString;
using ScenePathView = SP::ConcretePathStringView;

using RendererPath = SP::ConcretePathString;
using SurfacePath = SP::ConcretePathString;
using WindowPath = SP::ConcretePathString;
using WindowPathView = SP::ConcretePathStringView;
using HtmlTargetPath = SP::ConcretePathString;
using WidgetPath = SP::ConcretePathString;
using WidgetPathView = SP::ConcretePathStringView;

} // namespace SP::UI
