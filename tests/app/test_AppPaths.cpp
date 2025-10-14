#include "ext/doctest.h"

#include <pathspace/app/AppPaths.hpp>

using namespace SP;
using namespace SP::App;

namespace {

auto to_view(AppRootPath const& root) -> AppRootPathView {
    return AppRootPathView{root.getPath()};
}

} // namespace

TEST_SUITE("AppPaths") {

TEST_CASE("normalize_app_root canonicalizes and strips trailing slash") {
    auto normalized = normalize_app_root(AppRootPathView{"/system/applications/notepad/"});
    REQUIRE(normalized.has_value());
    CHECK(normalized->getPath() == "/system/applications/notepad");
}

TEST_CASE("normalize_app_root rejects invalid roots") {
    auto normalized = normalize_app_root(AppRootPathView{"system/app"});
    CHECK_FALSE(normalized.has_value());
}

TEST_CASE("resolve_app_relative joins relative paths under root") {
    auto root = normalize_app_root(AppRootPathView{"/system/applications/sketch"});
    REQUIRE(root.has_value());

    auto resolved = resolve_app_relative(to_view(*root), "scenes/main");
    REQUIRE(resolved.has_value());
    CHECK(resolved->getPath() == "/system/applications/sketch/scenes/main");
}

TEST_CASE("resolve_app_relative rejects absolute paths outside the app") {
    auto root = normalize_app_root(AppRootPathView{"/system/applications/sketch"});
    REQUIRE(root.has_value());

    auto resolved = resolve_app_relative(to_view(*root), "/system/applications/other/scenes/main");
    CHECK_FALSE(resolved.has_value());
}

TEST_CASE("derive_target_base extracts renderer target base") {
    auto root = normalize_app_root(AppRootPathView{"/system/applications/notepad"});
    REQUIRE(root.has_value());

    SP::App::ConcretePath renderer{"/system/applications/notepad/renderers/2d"};
    SP::App::ConcretePath target{"/system/applications/notepad/renderers/2d/targets/surfaces/editor/settings"};
    SP::App::ConcretePathView rendererView{renderer.getPath()};
    SP::App::ConcretePathView targetView{target.getPath()};

    auto base = derive_target_base(to_view(*root), rendererView, targetView);
    REQUIRE(base.has_value());
    CHECK(base->getPath() == "/system/applications/notepad/renderers/2d/targets/surfaces/editor");
}

TEST_CASE("derive_app_root identifies root for system app") {
    SP::ConcretePathString scene{"/system/applications/notepad/scenes/main"};
    auto root = derive_app_root(SP::ConcretePathStringView{scene.getPath()});
    REQUIRE(root.has_value());
    CHECK(root->getPath() == "/system/applications/notepad");
}

TEST_CASE("derive_app_root identifies root for user app") {
    SP::ConcretePathString surface{"/users/alex/system/applications/sketch/surfaces/view"};
    auto root = derive_app_root(SP::ConcretePathStringView{surface.getPath()});
    REQUIRE(root.has_value());
    CHECK(root->getPath() == "/users/alex/system/applications/sketch");
}

TEST_CASE("derive_app_root fails when applications segment missing") {
    SP::ConcretePathString invalid{"/system/not-an-app-root/path"};
    auto root = derive_app_root(SP::ConcretePathStringView{invalid.getPath()});
    CHECK_FALSE(root.has_value());
}

} // TEST_SUITE
