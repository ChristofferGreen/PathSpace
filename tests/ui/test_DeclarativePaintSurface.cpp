#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace {

using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind;
using DirtyRectHint = SP::UI::Builders::DirtyRectHint;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

struct RuntimeGuard {
    explicit RuntimeGuard(SP::PathSpace& s) : space(s) {}
    ~RuntimeGuard() { SP::System::ShutdownDeclarativeRuntime(space); }
    SP::PathSpace& space;
};

auto make_action(std::string const& widget_path,
                 WidgetOpKind kind,
                 float x,
                 float y) -> WidgetAction {
    WidgetAction action{};
    action.widget_path = widget_path;
    action.kind = kind;
    action.target_id = "paint_surface/stroke/42";
    action.pointer.has_local = true;
    action.pointer.local_x = x;
    action.pointer.local_y = y;
    return action;
}

auto flatten_stroke_points(std::vector<SP::UI::Declarative::PaintStrokeRecord> const& records)
    -> std::vector<SP::UI::Scene::StrokePoint> {
    std::vector<SP::UI::Scene::StrokePoint> points;
    for (auto const& stroke : records) {
        for (auto const& point : stroke.points) {
            SP::UI::Scene::StrokePoint flattened{};
            flattened.x = point.x;
            flattened.y = point.y;
            points.push_back(flattened);
        }
    }
    return points;
}

auto wait_for_gpu_ready(SP::PathSpace& space,
                        std::string const& widget_path,
                        std::chrono::milliseconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto state_path = widget_path + "/render/gpu/state";
    while (std::chrono::steady_clock::now() < deadline) {
        auto state = space.read<std::string, std::string>(state_path);
        if (state && *state == "Ready") {
            return true;
        }
        if (state && *state == "Error") {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("Declarative paint surface records strokes and builds stroke buckets") {
    SP::PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "paint_surface_test_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "main_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::PaintSurface::Args args{};
    args.brush_size = 10.0f;
    args.buffer_width = 128;
    args.buffer_height = 96;
    auto widget = SP::UI::Declarative::PaintSurface::Create(space,
                                                            window_view,
                                                            "canvas",
                                                            args);
    REQUIRE(widget);

    auto widget_path = widget->getPath();

    auto begin = make_action(widget_path, WidgetOpKind::PaintStrokeBegin, 5.0f, 6.0f);
    REQUIRE(PaintRuntime::HandleAction(space, begin));

    auto update = make_action(widget_path, WidgetOpKind::PaintStrokeUpdate, 32.0f, 40.0f);
    REQUIRE(PaintRuntime::HandleAction(space, update));

    auto commit = make_action(widget_path, WidgetOpKind::PaintStrokeCommit, 64.0f, 80.0f);
    REQUIRE(PaintRuntime::HandleAction(space, commit));

    auto records = PaintRuntime::LoadStrokeRecords(space, widget_path);
    REQUIRE(records);
    REQUIRE(records->size() == 1);
    CHECK_EQ(records->at(0).points.size(), 3);
    auto flattened = flatten_stroke_points(*records);

    auto descriptor = SP::UI::Declarative::LoadWidgetDescriptor(space, *widget);
    REQUIRE(descriptor);
    auto bucket = SP::UI::Declarative::BuildWidgetBucket(*descriptor);
    REQUIRE(bucket);
    REQUIRE_FALSE(bucket->command_kinds.empty());
    auto stroke_kind = static_cast<std::uint32_t>(SP::UI::Scene::DrawCommandKind::Stroke);
    CHECK(std::find(bucket->command_kinds.begin(), bucket->command_kinds.end(), stroke_kind)
          != bucket->command_kinds.end());
    CHECK_EQ(bucket->stroke_points.size(), flattened.size());
    for (std::size_t i = 0; i < flattened.size(); ++i) {
        CHECK(bucket->stroke_points[i].x == doctest::Approx(flattened[i].x));
        CHECK(bucket->stroke_points[i].y == doctest::Approx(flattened[i].y));
    }
}

TEST_CASE("Declarative paint surface GPU uploader stages texture payload") {
    using namespace std::chrono_literals;
    SP::PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    launch_options.start_widget_event_trellis = false;
    launch_options.start_paint_gpu_uploader = true;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "paint_surface_gpu_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "gpu_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::PaintSurface::Args args{};
    args.gpu_enabled = true;
    args.buffer_width = 96;
    args.buffer_height = 64;
    auto widget = SP::UI::Declarative::PaintSurface::Create(space,
                                                            window_view,
                                                            "gpu_canvas",
                                                            args);
    REQUIRE(widget);

    auto widget_path = widget->getPath();
    auto begin = make_action(widget_path, WidgetOpKind::PaintStrokeBegin, 8.0f, 12.0f);
    REQUIRE(PaintRuntime::HandleAction(space, begin));
    auto update = make_action(widget_path, WidgetOpKind::PaintStrokeUpdate, 40.0f, 30.0f);
    REQUIRE(PaintRuntime::HandleAction(space, update));
    auto commit = make_action(widget_path, WidgetOpKind::PaintStrokeCommit, 72.0f, 48.0f);
    REQUIRE(PaintRuntime::HandleAction(space, commit));

    CHECK(wait_for_gpu_ready(space, widget_path, 3s));

    auto texture_path = widget_path + "/assets/texture";
    auto texture = space.read<SP::UI::Declarative::PaintTexturePayload, std::string>(texture_path);
    REQUIRE(texture);
    CHECK(texture->width == args.buffer_width);
    CHECK(texture->height == args.buffer_height);
    CHECK_FALSE(texture->pixels.empty());

    auto stats_path = widget_path + "/render/gpu/stats";
    auto stats = space.read<SP::UI::Declarative::PaintGpuStats, std::string>(stats_path);
    REQUIRE(stats);
    CHECK(stats->uploads_total >= 1);
    CHECK(stats->last_revision == texture->revision);

    auto pending_path = widget_path + "/render/buffer/pendingDirty";
    auto pending = space.read<std::vector<DirtyRectHint>, std::string>(pending_path);
    REQUIRE(pending);
    CHECK(pending->empty());
}
