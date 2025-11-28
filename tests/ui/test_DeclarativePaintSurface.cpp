#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetDetail.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <thread>

namespace {

using WidgetAction = SP::UI::Declarative::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Runtime::Widgets::Bindings::WidgetOpKind;
using DirtyRectHint = SP::UI::Runtime::DirtyRectHint;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;
namespace BuilderDetail = SP::UI::Declarative::Detail;

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
    auto bucket = SP::UI::Declarative::BuildWidgetBucket(space, *descriptor);
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

TEST_CASE("Paint stroke history increments version for each mutation") {
    SP::PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "paint_surface_version_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "version_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::PaintSurface::Args args{};
    auto widget = SP::UI::Declarative::PaintSurface::Create(space,
                                                            window_view,
                                                            "version_canvas",
                                                            args);
    REQUIRE(widget);

    auto widget_path = widget->getPath();
    auto version_path = std::string(widget_path) + "/state/history/42/version";
    auto read_version = [&]() -> std::uint64_t {
        auto value = space.read<std::uint64_t, std::string>(version_path);
        if (!value) {
            return 0;
        }
        return *value;
    };

    auto begin = make_action(widget_path, WidgetOpKind::PaintStrokeBegin, 4.0f, 4.0f);
    REQUIRE(PaintRuntime::HandleAction(space, begin));
    CHECK_EQ(read_version(), 1);

    auto update = make_action(widget_path, WidgetOpKind::PaintStrokeUpdate, 8.0f, 10.0f);
    REQUIRE(PaintRuntime::HandleAction(space, update));
    CHECK_EQ(read_version(), 2);

    auto commit = make_action(widget_path, WidgetOpKind::PaintStrokeCommit, 16.0f, 18.0f);
    REQUIRE(PaintRuntime::HandleAction(space, commit));
    CHECK_EQ(read_version(), 3);

    auto points = PaintRuntime::ReadStrokePointsConsistent(space, widget_path, 42);
    REQUIRE(points);
    CHECK_EQ(points->size(), 3);
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

TEST_CASE("Paint surface layout resizing updates metrics and viewport") {
    SP::PathSpace space;
    SP::System::LaunchOptions launch_options{};
    launch_options.start_input_runtime = false;
    launch_options.start_io_pump = false;
    launch_options.start_io_telemetry_control = false;
    launch_options.start_widget_event_trellis = false;
    launch_options.start_paint_gpu_uploader = false;
    REQUIRE(SP::System::LaunchStandard(space, launch_options));
    RuntimeGuard runtime_guard{space};

    auto app_root = SP::App::Create(space, "paint_surface_resize_app");
    REQUIRE(app_root);

    SP::Window::CreateOptions window_options;
    window_options.name = "resize_window";
    auto window = SP::Window::Create(space, *app_root, window_options);
    REQUIRE(window);

    auto scene = SP::Scene::Create(space, *app_root, window->path, {});
    REQUIRE(scene);

    auto window_view_path = std::string(window->path.getPath()) + "/views/" + window->view_name;
    auto window_view = SP::App::ConcretePathView{window_view_path};

    SP::UI::Declarative::PaintSurface::Args args{};
    args.gpu_enabled = true;
    auto widget = SP::UI::Declarative::PaintSurface::Create(space,
                                                            window_view,
                                                            "resizable_canvas",
                                                            args);
    REQUIRE(widget);

    auto widget_path = widget->getPath();
    auto layout_path = std::string(widget_path) + "/layout/computed/size";
    auto dpi_path = std::string(scene->path.getPath()) + "/structure/window/"
        + window_options.name + "/metrics/dpi";

    auto set_layout = [&](float width, float height) {
        std::array<float, 2> size{width, height};
        REQUIRE(BuilderDetail::replace_single(space, layout_path, size));
    };
    auto set_dpi = [&](double dpi) {
        REQUIRE(BuilderDetail::replace_single(space, dpi_path, dpi));
    };

    auto stroke_once = [&]() {
        auto begin = make_action(widget_path, WidgetOpKind::PaintStrokeBegin, 4.0f, 5.0f);
        REQUIRE(PaintRuntime::HandleAction(space, begin));
        auto commit = make_action(widget_path, WidgetOpKind::PaintStrokeCommit, 10.0f, 12.0f);
        REQUIRE(PaintRuntime::HandleAction(space, commit));
    };

    set_dpi(1.0);
    set_layout(48.0f, 32.0f);
    auto initial_sync = PaintRuntime::ApplyLayoutSize(space, widget_path);
    REQUIRE(initial_sync);
    CHECK(*initial_sync);
    stroke_once();

    auto metrics = PaintRuntime::ReadBufferMetrics(space, widget_path);
    REQUIRE(metrics);
    CHECK_EQ(metrics->width, 48);
    CHECK_EQ(metrics->height, 32);

    auto viewport_path = std::string(widget_path) + "/render/buffer/viewport";
    auto viewport = space.read<SP::UI::Declarative::PaintBufferViewport, std::string>(viewport_path);
    REQUIRE(viewport);
    CHECK_EQ(viewport->max_x, doctest::Approx(48.0f));
    CHECK_EQ(viewport->max_y, doctest::Approx(32.0f));

    set_dpi(1.25);
    set_layout(80.0f, 64.0f);
    auto expanded = PaintRuntime::ApplyLayoutSize(space, widget_path);
    REQUIRE(expanded);
    CHECK(*expanded);

    metrics = PaintRuntime::ReadBufferMetrics(space, widget_path);
    REQUIRE(metrics);
    CHECK_EQ(metrics->width, 100);
    CHECK_EQ(metrics->height, 80);

    viewport = space.read<SP::UI::Declarative::PaintBufferViewport, std::string>(viewport_path);
    REQUIRE(viewport);
    CHECK_EQ(viewport->max_x, doctest::Approx(100.0f));
    CHECK_EQ(viewport->max_y, doctest::Approx(80.0f));

    auto redundant = PaintRuntime::ApplyLayoutSize(space, widget_path);
    REQUIRE(redundant);
    CHECK_FALSE(*redundant);

    set_dpi(1.0);
    set_layout(32.0f, 24.0f);
    auto shrink = PaintRuntime::ApplyLayoutSize(space, widget_path);
    REQUIRE(shrink);
    CHECK(*shrink);

    metrics = PaintRuntime::ReadBufferMetrics(space, widget_path);
    REQUIRE(metrics);
    CHECK_EQ(metrics->width, 32);
    CHECK_EQ(metrics->height, 24);

    viewport = space.read<SP::UI::Declarative::PaintBufferViewport, std::string>(viewport_path);
    REQUIRE(viewport);
    CHECK_EQ(viewport->max_x, doctest::Approx(32.0f));
    CHECK_EQ(viewport->max_y, doctest::Approx(24.0f));

    auto pending = space.read<std::vector<DirtyRectHint>, std::string>(widget_path + "/render/buffer/pendingDirty");
    REQUIRE(pending);
    REQUIRE_FALSE(pending->empty());
    auto const& last_hint = pending->back();
    CHECK_EQ(last_hint.min_x, doctest::Approx(0.0f));
    CHECK_EQ(last_hint.min_y, doctest::Approx(0.0f));
    CHECK_EQ(last_hint.max_x, doctest::Approx(32.0f));
    CHECK_EQ(last_hint.max_y, doctest::Approx(24.0f));

    auto gpu_state = space.read<std::string, std::string>(widget_path + "/render/gpu/state");
    REQUIRE(gpu_state);
    CHECK_EQ(*gpu_state, "DirtyFull");

    auto records = PaintRuntime::LoadStrokeRecords(space, widget_path);
    REQUIRE(records);
    REQUIRE_EQ(records->size(), 1);

    auto descriptor = SP::UI::Declarative::LoadWidgetDescriptor(space, *widget);
    REQUIRE(descriptor);
    auto bucket = SP::UI::Declarative::BuildWidgetBucket(space, *descriptor);
    REQUIRE(bucket);
    CHECK_EQ(bucket->stroke_points.size(), records->at(0).points.size());
}
