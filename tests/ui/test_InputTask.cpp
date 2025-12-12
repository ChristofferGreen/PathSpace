#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/WidgetMailbox.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <chrono>
#include <thread>

TEST_CASE("InputTask updates per-widget handler metrics") {
    using HandlerBinding = SP::UI::Declarative::HandlerBinding;
    using HandlerKind = SP::UI::Declarative::HandlerKind;

    PathSpace space;
    std::string widget_path = "/system/applications/test_app/widgets/test_button";

    // Insert a missing handler binding so dispatch records a missing metric.
    HandlerBinding binding{
        .registry_key = "missing#press#1",
        .kind = HandlerKind::ButtonPress,
    };
    REQUIRE(space.insert(SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, "/events/press/handler"), binding).errors.empty());
    REQUIRE(space.insert(SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, "/meta/kind"),
                         std::string{"button"})
                .errors.empty());
    REQUIRE(space.insert(SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, "/capsule/mailbox/subscriptions"),
                         std::vector<std::string>{"activate"})
                .errors.empty());

    SP::UI::Declarative::WidgetMailboxEvent mailbox_event{};
    mailbox_event.topic = "activate";
    mailbox_event.kind = SP::UI::Runtime::Widgets::Bindings::WidgetOpKind::Activate;
    mailbox_event.widget_path = widget_path;
    mailbox_event.target_id = "button/background";
    mailbox_event.pointer = SP::UI::Runtime::Widgets::Bindings::PointerInfo::Make(0.0f, 0.0f)
                               .WithInside(true)
                               .WithPrimary(true);
    mailbox_event.sequence = 1;
    mailbox_event.timestamp_ns = 1;
    REQUIRE(space.insert(SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path,
                                                                  "/capsule/mailbox/events/activate/queue"),
                         mailbox_event)
                .errors.empty());

    SP::UI::Declarative::InputTaskOptions options;
    options.poll_interval = std::chrono::milliseconds{1};
    auto started = SP::UI::Declarative::CreateInputTask(space, options);
    REQUIRE(started);
    REQUIRE(*started);

    auto metric_path = SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, "/metrics/handlers/missing_total");
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
