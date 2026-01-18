#include "third_party/doctest.h"

#if PATHSPACE_ENABLE_UI

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/declarative/HistoryBinding.hpp>

TEST_SUITE("ui.history.binding") {
TEST_CASE("HistoryBinding initializes metrics") {
    SP::PathSpace space;
    auto widget_path = std::string("/widgets/paint");

    auto metrics_root = SP::UI::Declarative::HistoryMetricsRoot(widget_path);
    CHECK(metrics_root == "/widgets/paint/space/metrics/history_binding");

    SP::UI::Declarative::InitializeHistoryMetrics(space, widget_path);

    auto state = space.read<std::string, std::string>(metrics_root + "/state");
    REQUIRE(state);
    CHECK(*state == "pending");

    auto buttons = space.read<bool, std::string>(metrics_root + "/buttons_enabled");
    REQUIRE(buttons);
    CHECK(*buttons == false);
}

TEST_CASE("HistoryBinding updates telemetry for actions") {
    SP::PathSpace space;
    auto widget_path = std::string("/widgets/demo");

    SP::UI::Declarative::InitializeHistoryMetrics(space, widget_path);
    SP::UI::Declarative::HistoryBindingOptions options{
        .history_root = widget_path,
    };
    std::shared_ptr<SP::UI::Declarative::HistoryBinding> binding;
    {
        auto binding_result = SP::UI::Declarative::CreateHistoryBinding(space, options);
        REQUIRE(binding_result);
        binding = *binding_result;
    }

    SP::UI::Declarative::SetHistoryBindingButtonsEnabled(space, *binding, true);
    auto metrics_root = binding->metrics_root;
    CHECK(binding->buttons_enabled);

    SP::UI::Declarative::RecordHistoryBindingActionResult(space,
                                                          *binding,
                                                          SP::UI::Declarative::HistoryBindingAction::Undo,
                                                          true);
    CHECK(binding->undo_total == 1);

    SP::Error sample_error{SP::Error::Code::UnknownError, "sample"};
    auto error_info = SP::UI::Declarative::RecordHistoryBindingError(space,
                                                                     metrics_root,
                                                                     "UndoableSpace::undo",
                                                                     &sample_error);
    CHECK(error_info.context == "UndoableSpace::undo");
    CHECK_FALSE(error_info.message.empty());
    CHECK_FALSE(error_info.code.empty());
}

TEST_CASE("HistoryBinding lookup exposes registered bindings and cleans up expired entries") {
    SP::PathSpace space;
    auto widget_path = std::string("/widgets/paint_lookup");

    SP::UI::Declarative::InitializeHistoryMetrics(space, widget_path);
    SP::UI::Declarative::HistoryBindingOptions options{
        .history_root = widget_path,
    };
    std::shared_ptr<SP::UI::Declarative::HistoryBinding> binding;
    {
        auto binding_result = SP::UI::Declarative::CreateHistoryBinding(space, options);
        REQUIRE(binding_result);
        binding = *binding_result;
    }

    auto lookup = SP::UI::Declarative::LookupHistoryBinding(widget_path);
    REQUIRE(lookup);
    CHECK(lookup->root == widget_path);

    // Drop all strong references and confirm lookup no longer returns a binding.
    lookup.reset();
    binding.reset();
    auto expired_lookup = SP::UI::Declarative::LookupHistoryBinding(widget_path);
    CHECK_FALSE(expired_lookup);
}

#endif // PATHSPACE_ENABLE_UI
}
