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

TEST_CASE("Latest mode provides non-destructive reads") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/latest/out",
        .sources = {"/latest/a"},
        .mode    = "latest",
        .policy  = "priority",
    };

    auto result = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(result.errors.empty());

    REQUIRE(backing->insert("/latest/a", 1337).errors.empty());

    auto read = trellis.read<int>("/latest/out", Block{});
    REQUIRE(read);
    CHECK(read.value() == 1337);

    auto second = trellis.take<int>("/latest/out", Block{});
    REQUIRE(second);
    CHECK(second.value() == 1337);

    auto direct = backing->read<int>("/latest/a", Block{});
    REQUIRE(direct);
    CHECK(direct.value() == 1337);
}

TEST_CASE("Latest mode round-robin rotates across sources") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/rr/out",
        .sources = {"/rr/a", "/rr/b"},
        .mode    = "latest",
        .policy  = "round_robin",
    };

    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/rr/a", std::string("alpha")).errors.empty());
    REQUIRE(backing->insert("/rr/b", std::string("beta")).errors.empty());

    auto first = trellis.read<std::string>("/rr/out", Block{});
    REQUIRE(first);
    CHECK(first.value() == "alpha");

    auto second = trellis.read<std::string>("/rr/out", Block{});
    REQUIRE(second);
    CHECK(second.value() == "beta");

    auto third = trellis.read<std::string>("/rr/out", Block{});
    REQUIRE(third);
    CHECK(third.value() == "alpha");
}

TEST_CASE("Latest mode blocks until data arrives") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/wait/out",
        .sources = {"/wait/a", "/wait/b"},
        .mode    = "latest",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    std::thread producer([backing] {
        std::this_thread::sleep_for(30ms);
        backing->insert("/wait/b", 2025);
    });

    auto begin  = std::chrono::steady_clock::now();
    auto resultLatest = trellis.read<int>("/wait/out", Block{200ms});
    producer.join();

    if (!resultLatest) {
        CAPTURE(static_cast<int>(resultLatest.error().code));
        CAPTURE(resultLatest.error().message.value_or("<none>"));
    }
    REQUIRE(resultLatest);
    CHECK(resultLatest.value() == 2025);
    CHECK((std::chrono::steady_clock::now() - begin) >= 25ms);
}

TEST_CASE("Trellis configuration persists to backing state registry") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/persist/out",
        .sources = {"/persist/a", "/persist/b"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto configPath = std::string{"/_system/trellis/state/"};
    configPath.append(keys.front());
    configPath.append("/config");

    auto stored = backing->read<TrellisPersistedConfig>(configPath);
    REQUIRE(stored);
    CHECK(stored->name == "/persist/out");
    CHECK(stored->sources == std::vector<std::string>{"/persist/a", "/persist/b"});
    CHECK(stored->mode == "queue");
    CHECK(stored->policy == "priority");

    auto statsPath = std::string{"/_system/trellis/state/"};
    statsPath.append(keys.front());
    statsPath.append("/stats");

    auto stats = backing->read<TrellisStats>(statsPath);
    REQUIRE(stats);
    CHECK(stats->name == "/persist/out");
    CHECK(stats->servedCount == 0);
    CHECK(stats->sourceCount == 2);
    CHECK(stats->mode == "queue");
    CHECK(stats->policy == "priority");

    DisableTrellisCommand disable{.name = "/persist/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());

    auto removed = backing->read<TrellisPersistedConfig>(configPath);
    REQUIRE_FALSE(removed);
    auto removeCode   = removed.error().code;
    bool acceptable   = removeCode == Error::Code::NoObjectFound || removeCode == Error::Code::NoSuchPath;
    CAPTURE(static_cast<int>(removeCode));
    CHECK(acceptable);

    auto removedStats = backing->read<TrellisStats>(statsPath);
    REQUIRE_FALSE(removedStats);
    auto statsCode     = removedStats.error().code;
    bool statsCleared  = statsCode == Error::Code::NoObjectFound || statsCode == Error::Code::NoSuchPath;
    CAPTURE(static_cast<int>(statsCode));
    CHECK(statsCleared);
}

TEST_CASE("Persisted trellis config restores on new instance") {
    auto backing = std::make_shared<PathSpace>();
    {
        PathSpaceTrellis trellis(backing);
        EnableTrellisCommand enable{
            .name    = "/reload/out",
            .sources = {"/reload/a"},
            .mode    = "queue",
            .policy  = "round_robin",
        };
        auto enableResult = trellis.insert("/_system/trellis/enable", enable);
        REQUIRE(enableResult.errors.empty());
    }

    PathSpaceTrellis restored(backing);

    REQUIRE(backing->insert("/reload/a", 77).errors.empty());

    auto result = restored.take<int>("/reload/out", Block{});
    REQUIRE(result);
    CHECK(result.value() == 77);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto statsPath = std::string{"/_system/trellis/state/"};
    statsPath.append(keys.front());
    statsPath.append("/stats");

    auto stats = backing->read<TrellisStats>(statsPath);
    REQUIRE(stats);
    CHECK(stats->servedCount == 1);
    CHECK(stats->lastSource == "/reload/a");
    CHECK(stats->waitCount == 0);
}

TEST_CASE("Trellis stats capture wait counts when blocking") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/stats/out",
        .sources = {"/stats/source"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    std::thread producer([backing] {
        std::this_thread::sleep_for(25ms);
        backing->insert("/stats/source", 9001);
    });

    auto value = trellis.take<int>("/stats/out", Block{200ms});
    producer.join();
    REQUIRE(value);
    CHECK(value.value() == 9001);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto statsPath = std::string{"/_system/trellis/state/"};
    statsPath.append(keys.front());
    statsPath.append("/stats");

    auto stats = backing->read<TrellisStats>(statsPath);
    REQUIRE(stats);
    CHECK(stats->servedCount == 1);
    CHECK(stats->waitCount == 1);
    CHECK(stats->lastSource == "/stats/source");
}

} // TEST_SUITE

} // namespace SP
