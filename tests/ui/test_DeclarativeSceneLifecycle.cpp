#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace SP;

namespace {

auto wait_for_metric(PathSpace& space,
                     std::string const& metric_path,
                     std::uint64_t target,
                     std::chrono::milliseconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto value = space.read<std::uint64_t, std::string>(metric_path);
        if (value && *value >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

} // namespace

TEST_CASE("Scene lifecycle rebuilds declarative widget buckets") {
    PathSpace space;

    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    auto launch = SP::System::LaunchStandard(space, launch_options);
    REQUIRE(launch);

    auto app_root = SP::App::Create(space, "scene_lifecycle_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "main_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_options;
    scene_options.name = "main_scene";
    auto scene = SP::Scene::Create(space, *app_root, window->path, scene_options);
    REQUIRE(scene);

    auto widget = SP::UI::Declarative::Button::Create(space,
                                                      SP::App::ConcretePathView{window->path.getPath()},
                                                      "watch_me",
                                                      {});
    REQUIRE(widget);

    auto widget_relative = std::string(widget->getPath()).substr(app_root->getPath().size());
    if (widget_relative.empty() || widget_relative.front() != '/') {
        widget_relative.insert(widget_relative.begin(), '/');
    }
    auto bucket_path = std::string(scene->path.getPath()) + "/structure/widgets" + widget_relative + "/render/bucket";
    auto metrics_path = std::string(scene->path.getPath()) + "/runtime/lifecycle/metrics/events_processed_total";
    auto initial_metric = space.read<std::uint64_t, std::string>(metrics_path).value_or(0);

    CHECK(wait_for_metric(space, metrics_path, initial_metric + 1, std::chrono::milliseconds{1500}));
    auto first_bucket = space.read<SP::UI::Scene::DrawableBucketSnapshot, std::string>(bucket_path);
    REQUIRE(first_bucket);

    auto updated = SP::UI::Declarative::Button::SetLabel(space, *widget, "updated label");
    REQUIRE(updated);
    CHECK(wait_for_metric(space, metrics_path, initial_metric + 2, std::chrono::milliseconds{1500}));
    auto second_bucket = space.read<SP::UI::Scene::DrawableBucketSnapshot, std::string>(bucket_path);
    REQUIRE(second_bucket);
    CHECK(second_bucket->drawable_ids.size() == first_bucket->drawable_ids.size());

    auto shutdown = SP::Scene::Shutdown(space, scene->path);
    REQUIRE(shutdown);
    SP::System::ShutdownDeclarativeRuntime(space);
}
