#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>

using namespace SP;

TEST_CASE("Declarative runtime wires app, window, and scene") {
    PathSpace space;

    auto launch = SP::System::LaunchStandard(space);
    REQUIRE(launch);
    CHECK_FALSE(launch->default_theme_path.empty());

    auto app_root = SP::App::Create(space, "hello_widgets");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.title = "Hello Widgets";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "main";
    scene_options.description = "Declarative main scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto relative_scene = std::string(scene->path.getPath()).substr(app_root->getPath().size() + 1);

    auto view_path = std::string(window->path.getPath()) + "/views/" + window->view_name + "/scene";
    auto stored_scene = space.read<std::string, std::string>(view_path);
    REQUIRE(stored_scene);
    CHECK(stored_scene.value() == relative_scene);

    auto structure_view = std::string(scene->path.getPath())
                          + "/structure/window/"
                          + "main_window/view";
    auto stored_view = space.read<std::string, std::string>(structure_view);
    REQUIRE(stored_view);
    CHECK(stored_view.value() == window->view_name);

    auto attached = space.read<bool, std::string>(std::string(scene->path.getPath()) + "/state/attached");
    REQUIRE(attached);
    CHECK(attached.value());
}
