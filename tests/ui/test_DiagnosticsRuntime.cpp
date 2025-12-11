#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

using namespace SP;
using namespace SP::UI;
using namespace SP::UI::Runtime;

TEST_CASE("Diagnostics error stats track severity and clears") {
    PathSpace space;
    ConcretePathString target{"/renderers/test/targets/main"};
    auto targetView = ConcretePathStringView{target.getPath()};

    Runtime::Diagnostics::PathSpaceError error{};
    error.message = "renderer crashed";
    error.code = static_cast<int>(SP::Error::Code::InvalidError);
    error.severity = Runtime::Diagnostics::PathSpaceError::Severity::Info;

    REQUIRE(Runtime::Diagnostics::WriteTargetError(space, targetView, error));

    auto stats = Runtime::Diagnostics::ReadTargetErrorStats(space, targetView);
    REQUIRE(stats);
    CHECK_EQ(stats->total, 1u);
    CHECK_EQ(stats->fatal, 1u);
    CHECK_EQ(stats->info, 0u);
    CHECK_EQ(stats->warning, 0u);
    CHECK_EQ(stats->recoverable, 0u);
    CHECK_EQ(stats->last_code, static_cast<std::uint64_t>(error.code));
    CHECK(stats->last_severity == Runtime::Diagnostics::PathSpaceError::Severity::Fatal);

    auto metrics = Runtime::Diagnostics::ReadTargetMetrics(space, targetView);
    REQUIRE(metrics);
    CHECK_EQ(metrics->error_total, 1u);
    CHECK_EQ(metrics->error_fatal, 1u);
    CHECK_EQ(metrics->last_error_code, error.code);
    CHECK(metrics->last_error_severity == Runtime::Diagnostics::PathSpaceError::Severity::Fatal);
    CHECK_EQ(metrics->last_error, error.message);

    REQUIRE(Runtime::Diagnostics::ClearTargetError(space, targetView));

    stats = Runtime::Diagnostics::ReadTargetErrorStats(space, targetView);
    REQUIRE(stats);
    CHECK_EQ(stats->cleared, 1u);
    CHECK_EQ(stats->total, 1u);
    CHECK_EQ(stats->fatal, 1u);
}

TEST_CASE("ReadTargetMetrics captures HTML adapter metrics") {
    PathSpace space;
    ConcretePathString target{"/renderers/html_renderer/targets/main"};
    auto targetView = ConcretePathStringView{target.getPath()};

    auto htmlBase = std::string(target.getPath()) + "/output/v1/html";
    auto insert_value = [&](std::string const& path, auto&& value) {
        auto result = space.insert(path, std::forward<decltype(value)>(value));
        REQUIRE(result.errors.empty());
    };

    insert_value(htmlBase + "/domNodeCount", static_cast<uint64_t>(42));
    insert_value(htmlBase + "/commandCount", static_cast<uint64_t>(17));
    insert_value(htmlBase + "/assetCount", static_cast<uint64_t>(3));
    insert_value(htmlBase + "/usedCanvasFallback", true);
    insert_value(htmlBase + "/mode", std::string{"canvas"});
    insert_value(htmlBase + "/options/maxDomNodes", static_cast<uint64_t>(9999));
    insert_value(htmlBase + "/options/preferDom", false);
    insert_value(htmlBase + "/options/allowCanvasFallback", true);

    auto metrics = Runtime::Diagnostics::ReadTargetMetrics(space, targetView);
    REQUIRE(metrics);
    CHECK_EQ(metrics->html_dom_node_count, 42u);
    CHECK_EQ(metrics->html_command_count, 17u);
    CHECK_EQ(metrics->html_asset_count, 3u);
    CHECK(metrics->html_used_canvas_fallback);
    CHECK_EQ(metrics->html_mode, std::string{"canvas"});
    CHECK_EQ(metrics->html_max_dom_nodes, 9999u);
    CHECK_FALSE(metrics->html_prefer_dom);
    CHECK(metrics->html_allow_canvas_fallback);
}
