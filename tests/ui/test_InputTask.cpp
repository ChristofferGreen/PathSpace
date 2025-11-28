#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <chrono>
#include <thread>

TEST_CASE("InputTask updates per-widget handler metrics") {
    using WidgetOp = SP::UI::Runtime::Widgets::Bindings::WidgetOp;
    using HandlerBinding = SP::UI::Declarative::HandlerBinding;
    using HandlerKind = SP::UI::Declarative::HandlerKind;

    PathSpace space;
    std::string widget_path = "/system/applications/test_app/widgets/test_button";

    // Insert a missing handler binding so dispatch records a missing metric.
    HandlerBinding binding{
        .registry_key = "missing#press#1",
        .kind = HandlerKind::ButtonPress,
    };
    REQUIRE(space.insert(widget_path + "/events/press/handler", binding).errors.empty());

    WidgetOp op{};
    op.kind = SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate;
    op.widget_path = widget_path;
    op.target_id = "button/background";
    REQUIRE(space.insert(widget_path + "/ops/inbox/queue", op).errors.empty());

    SP::UI::Declarative::InputTaskOptions options;
    options.poll_interval = std::chrono::milliseconds{1};
    auto started = SP::UI::Declarative::CreateInputTask(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    auto metric_path = widget_path + "/metrics/handlers/missing_total";
    bool observed = false;
    for (int attempt = 0; attempt < 100 && !observed; ++attempt) {
        auto value = space.read<std::uint64_t, std::string>(metric_path);
        if (value && *value >= 1u) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(observed);

    SP::UI::Declarative::ShutdownInputTask(space);
}
