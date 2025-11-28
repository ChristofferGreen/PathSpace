#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Helpers.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>

namespace {

using namespace SP;
using namespace SP::UI;

struct SceneHelpersFixture {
    PathSpace space;
    AppRootPath app_root{ "/system/applications/test_app" };
    auto root_view() const -> SP::App::AppRootPathView { return SP::App::AppRootPathView{app_root.getPath()}; }
};

} // namespace

TEST_SUITE("UIHelpersAndBuilders") {

TEST_CASE("Scene::Create returns canonical scene path") {
    SceneHelpersFixture fx;
    SceneParams params{
        .name = "main",
        .description = "Main scene",
    };

    auto helperResult = Scene::Create(fx.space, fx.app_root, params);
    REQUIRE(helperResult.has_value());
    CHECK(helperResult->getPath() == "/system/applications/test_app/scenes/main");

    auto builderResult = SP::UI::Runtime::Scene::Create(fx.space, fx.root_view(), params);
    REQUIRE(builderResult.has_value());
    CHECK(builderResult->getPath() == helperResult->getPath());
}

TEST_CASE("Scene::Create rejects invalid scene name") {
    SceneHelpersFixture fx;
    SceneParams params{
        .name = "../oops",
        .description = "Bad scene",
    };

    auto helperResult = Scene::Create(fx.space, fx.app_root, params);
    CHECK_FALSE(helperResult.has_value());

    auto builderResult = SP::UI::Runtime::Scene::Create(fx.space, fx.root_view(), params);
    CHECK_FALSE(builderResult.has_value());
}

TEST_CASE("Renderer::ResolveTargetBase builds relative target path") {
    SceneHelpersFixture fx;
    RendererParams rendererParams{
        .name = "2d",
        .kind = RendererKind::Software2D,
        .description = "Renderer",
    };
    auto helperRenderer = Renderer::Create(fx.space, fx.app_root, rendererParams);
    REQUIRE(helperRenderer.has_value());
    auto builderRenderer = SP::UI::Runtime::Renderer::Create(fx.space, fx.root_view(), rendererParams);
    REQUIRE(builderRenderer.has_value());
    CHECK(builderRenderer->getPath() == helperRenderer->getPath());

    auto helperTarget = Renderer::ResolveTargetBase(fx.space, fx.app_root, *helperRenderer, "targets/surfaces/editor/settings");
    REQUIRE(helperTarget.has_value());
    auto builderTarget = SP::UI::Runtime::Renderer::ResolveTargetBase(fx.space, fx.root_view(), *helperRenderer, "targets/surfaces/editor/settings");
    REQUIRE(builderTarget.has_value());
    CHECK(builderTarget->getPath() == helperTarget->getPath());
}

TEST_CASE("Surface::SetScene requires shared app root") {
    SceneHelpersFixture fx;
    SceneParams sceneParams{ .name = "main", .description = "Scene" };
    auto scenePath = Scene::Create(fx.space, fx.app_root, sceneParams);
    REQUIRE(scenePath.has_value());

    SurfaceParams surfaceParams{
        .name = "editor",
        .desc = {},
        .renderer = "renderers/2d"
    };
    auto surfacePath = Surface::Create(fx.space, fx.app_root, surfaceParams);
    REQUIRE(surfacePath.has_value());

    auto helperOk = Surface::SetScene(fx.space, *surfacePath, *scenePath);
    CHECK(helperOk.has_value());

    SurfacePath foreignSurface{ "/system/applications/other_app/surfaces/editor" };
    auto helperMismatch = Surface::SetScene(fx.space, foreignSurface, *scenePath);
    CHECK_FALSE(helperMismatch.has_value());
    CHECK(helperMismatch.error().code == SP::Error::Code::InvalidPath);
}

TEST_CASE("Window::Create returns canonical path") {
    SceneHelpersFixture fx;
    WindowParams params{
        .name = "MainWindow",
        .title = "Main",
        .width = 800,
        .height = 600,
        .scale = 1.0f,
        .background = "#000000",
    };

    auto helperWindow = Window::Create(fx.space, fx.app_root, params);
    REQUIRE(helperWindow.has_value());
    CHECK(helperWindow->getPath() == "/system/applications/test_app/windows/MainWindow");

    auto builderWindow = SP::UI::Runtime::Window::Create(fx.space, fx.root_view(), params);
    REQUIRE(builderWindow.has_value());
    CHECK(builderWindow->getPath() == helperWindow->getPath());
}

}
