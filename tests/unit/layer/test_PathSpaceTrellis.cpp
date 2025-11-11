#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/core/Out.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace SP {

TEST_SUITE("PathSpaceTrellis") {

TEST_CASE("Queue mode round-robin across sources") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/trellis/out",
        .sources = {"/sources/a", "/sources/b"},
        .mode    = "queue",
        .policy  = "round_robin",
    };

    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/sources/a", 1).errors.empty());
    REQUIRE(backing->insert("/sources/b", 2).errors.empty());
    REQUIRE(backing->insert("/sources/a", 3).errors.empty());

    auto first = trellis.take<int>("/trellis/out");
    REQUIRE(first);
    CHECK(first.value() == 1);

    auto second = trellis.take<int>("/trellis/out");
    REQUIRE(second);
    CHECK(second.value() == 2);

    auto third = trellis.take<int>("/trellis/out");
    REQUIRE(third);
    CHECK(third.value() == 3);
}

TEST_CASE("Queue mode blocks until data arrives") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/await/out",
        .sources = {"/await/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto begin = std::chrono::steady_clock::now();

    std::thread producer([backing] {
        std::this_thread::sleep_for(30ms);
        backing->insert("/await/a", 42);
    });

    auto result = trellis.take<int>("/await/out", Block{200ms});
    producer.join();

    if (!result) {
        auto err = result.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("<none>"));
        auto direct = backing->take<int>("/await/a", Block{200ms});
        if (!direct) {
            CAPTURE(static_cast<int>(direct.error().code));
            CAPTURE(direct.error().message.value_or("<none>"));
        } else {
            CAPTURE(direct.value());
        }
        auto leftover = backing->take<int>("/await/a", Block{0ms});
        if (!leftover) {
            CAPTURE(static_cast<int>(leftover.error().code));
            CAPTURE(leftover.error().message.value_or("<none>"));
        } else {
            CAPTURE(leftover.value());
        }
        FAIL_CHECK("trellis.take failed");
    }
    REQUIRE(result);
    CHECK(result.value() == 42);
    CHECK((std::chrono::steady_clock::now() - begin) >= 25ms);
}

TEST_CASE("Disable removes trellis state") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/fan/out",
        .sources = {"/fan/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    DisableTrellisCommand disable{.name = "/fan/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());

    auto takeResult = trellis.take<int>("/fan/out");
    REQUIRE_FALSE(takeResult);
    CHECK(takeResult.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("Latest mode is reported as unsupported") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/latest/out",
        .sources = {"/latest/a"},
        .mode    = "latest",
        .policy  = "priority",
    };

    auto result = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors.front().code == Error::Code::NotSupported);
}

} // TEST_SUITE

} // namespace SP
