#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/core/Out.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <numeric>

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

TEST_CASE("Queue mode handles concurrent producers and consumer") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/concurrent/out",
        .sources = {"/concurrent/a", "/concurrent/b"},
        .mode    = "queue",
        .policy  = "round_robin",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    constexpr int kProducerCount     = 4;
    constexpr int kValuesPerProducer = 16;
    constexpr int kTotalValues       = kProducerCount * kValuesPerProducer;

    std::atomic<bool> producerError{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int p = 0; p < kProducerCount; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kValuesPerProducer; ++i) {
                int  value = p * kValuesPerProducer + i;
                auto path  = (i % 2 == 0) ? "/concurrent/a" : "/concurrent/b";
                auto ret   = backing->insert(path, value);
                if (!ret.errors.empty()) {
                    producerError.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    std::atomic<int>  consumedCount{0};
    std::atomic<bool> consumerError{false};
    std::mutex        consumedMutex;
    std::vector<int>  consumed;
    consumed.reserve(kTotalValues);

    std::thread consumer([&]() {
        while (consumedCount.load(std::memory_order_acquire) < kTotalValues) {
            auto value = trellis.take<int>("/concurrent/out", Block{500ms});
            if (value) {
                {
                    std::lock_guard<std::mutex> lock(consumedMutex);
                    consumed.push_back(value.value());
                }
                consumedCount.fetch_add(1, std::memory_order_release);
            } else if (value.error().code == Error::Code::Timeout) {
                continue;
            } else {
                consumerError.store(true, std::memory_order_relaxed);
                break;
            }
        }
    });

    for (auto& producer : producers)
        producer.join();
    consumer.join();

    CHECK_FALSE(producerError.load(std::memory_order_relaxed));
    CHECK_FALSE(consumerError.load(std::memory_order_relaxed));
    CHECK(consumedCount.load(std::memory_order_relaxed) == kTotalValues);

    std::vector<int> expected(kTotalValues);
    std::iota(expected.begin(), expected.end(), 0);

    {
        std::lock_guard<std::mutex> lock(consumedMutex);
        std::sort(consumed.begin(), consumed.end());
        CHECK(consumed == expected);
    }
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

TEST_CASE("Queue mode forwards multiple value types") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/typed/out",
        .sources = {"/typed/a", "/typed/b"},
        .mode    = "queue",
        .policy  = "round_robin",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/typed/a", 3.14159).errors.empty());
    REQUIRE(backing->insert("/typed/b", std::string("hello")).errors.empty());

    auto dbl = trellis.take<double>("/typed/out", Block{});
    REQUIRE(dbl);
    CHECK(dbl.value() == doctest::Approx(3.14159));

    auto str = trellis.take<std::string>("/typed/out", Block{});
    REQUIRE(str);
    CHECK(str.value() == "hello");
}

TEST_CASE("Trellis nested inside PathSpace hierarchy") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    auto context = std::make_shared<PathSpaceContext>();
    trellis.adoptContextAndPrefix(context, "/layers/trellis");

    EnableTrellisCommand enable{
        .name    = "/layers/trellis/out",
        .sources = {"/jobs/a", "/jobs/b"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/jobs/a", std::string("alpha")).errors.empty());
    REQUIRE(backing->insert("/jobs/b", std::string("beta")).errors.empty());

    auto first = trellis.take<std::string>("/out", Block{500ms});
    if (!first) {
        auto err = first.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("<none>"));
    }
    REQUIRE(first);
    auto second = trellis.take<std::string>("/out", Block{500ms});
    if (!second) {
        auto err = second.error();
        CAPTURE(static_cast<int>(err.code));
        CAPTURE(err.message.value_or("<none>"));
    }
    REQUIRE(second);

    CHECK(first.value() == "alpha");
    CHECK(second.value() == "beta");

    auto empty = trellis.read<std::string>("/out");
    CHECK_FALSE(empty);
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
