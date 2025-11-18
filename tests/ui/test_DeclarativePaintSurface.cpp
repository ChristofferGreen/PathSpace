#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

namespace {

using WidgetAction = SP::UI::Builders::Widgets::Reducers::WidgetAction;
using WidgetOpKind = SP::UI::Builders::Widgets::Bindings::WidgetOpKind;
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

    SP::UI::Declarative::PaintSurface::Args args{};
    args.brush_size = 10.0f;
    args.buffer_width = 128;
    args.buffer_height = 96;
    auto widget = SP::UI::Declarative::PaintSurface::Create(space,
                                                            SP::App::ConcretePathView{window->path.getPath()},
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

    auto descriptor = SP::UI::Declarative::LoadWidgetDescriptor(space, *widget);
    REQUIRE(descriptor);
    auto bucket = SP::UI::Declarative::BuildWidgetBucket(*descriptor);
    REQUIRE(bucket);
    REQUIRE_FALSE(bucket->command_kinds.empty());
    CHECK_EQ(bucket->command_kinds.front(), static_cast<std::uint32_t>(SP::UI::Scene::DrawCommandKind::Stroke));
    CHECK_EQ(bucket->stroke_points.size(), 3);
}
