#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/core/Out.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include <numeric>
#include <string_view>
#include <optional>
#include <string>

using namespace std::chrono_literals;

namespace SP {

namespace {
struct ScopedEnvVar {
    ScopedEnvVar(char const* key, char const* value) : name(key) {
        if (auto* current = std::getenv(key)) {
            previous = std::string{current};
        }
        ::setenv(key, value, 1);
    }
    ~ScopedEnvVar() {
        if (previous) {
            ::setenv(name.c_str(), previous->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
    std::string               name;
    std::optional<std::string> previous;
};

auto encodeStateKey(std::string_view path) -> std::string {
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string           encoded;
    encoded.reserve(path.size() * 2);
    for (unsigned char ch : path) {
        encoded.push_back(hexDigits[(ch >> 4) & 0x0F]);
        encoded.push_back(hexDigits[ch & 0x0F]);
    }
    return encoded;
}

auto runtimeDescriptorPath(std::string_view outputPath) -> std::string {
    auto key = encodeStateKey(outputPath);
    std::string path{"/state/"};
    path.append(key);
    path.append("/runtime/descriptor");
    return path;
}

} // namespace

TEST_SUITE("PathSpaceTrellis") {
TEST_CASE("Internal PathSpace is instantiated") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    auto internal = trellis.debugInternalSpace();
    REQUIRE(internal);
    CHECK(internal->listChildren().empty());

    EnableTrellisCommand enable{
        .name    = "/probe/out",
        .sources = {"/probe/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    for (auto const& err : enableResult.errors) {
        std::cerr << "[legacy-test] enable error code=" << static_cast<int>(err.code)
                  << " message=" << err.message.value_or("<none>") << std::endl;
    }
    REQUIRE(enableResult.errors.empty());

    auto internalAfter = trellis.debugInternalSpace();
    REQUIRE(internalAfter);
    CHECK(internalAfter.get() == internal.get());

    DisableTrellisCommand disable{.name = "/probe/out"};
   auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());
}

TEST_CASE("Runtime descriptor mirrors trellis state") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/descriptor/out",
        .sources = {"/descriptor/a", "/descriptor/b"},
        .mode    = "queue",
        .policy  = "round_robin",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto internal = trellis.debugInternalSpace();
    REQUIRE(internal);

    auto const descriptorPath = runtimeDescriptorPath("/descriptor/out");

    auto descriptor = internal->read<TrellisRuntimeDescriptor>(descriptorPath);
    REQUIRE(descriptor);
    CHECK(descriptor->config.name == "/descriptor/out");
    CHECK(descriptor->config.mode == "queue");
    CHECK(descriptor->config.policy == "round_robin");
    CHECK(descriptor->config.sources
          == std::vector<std::string>{"/descriptor/a", "/descriptor/b"});
    CHECK(descriptor->readyQueue.empty());
    CHECK(descriptor->bufferedReady == 0);
    CHECK(descriptor->flags.shuttingDown == false);

    auto inserted = backing->insert("/descriptor/a", 77);
    REQUIRE(inserted.errors.empty());

    auto value = trellis.take<int>("/descriptor/out");
    REQUIRE(value);
    CHECK(value.value() == 77);

    auto descriptorAfterTake = internal->read<TrellisRuntimeDescriptor>(descriptorPath);
    REQUIRE(descriptorAfterTake);
    CHECK(descriptorAfterTake->trace.events.size() >= descriptor->trace.events.size());

    DisableTrellisCommand disable{.name = "/descriptor/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());

    auto descriptorAfterDisable = internal->read<TrellisRuntimeDescriptor>(descriptorPath);
    REQUIRE_FALSE(descriptorAfterDisable);
    auto code = descriptorAfterDisable.error().code;
    bool isExpectedCode = (code == Error::Code::NoObjectFound) || (code == Error::Code::NoSuchPath);
    CHECK(isExpectedCode);
}
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
    for (auto const& err : enableResult.errors) {
        std::cerr << "[legacy-test] enable error code=" << static_cast<int>(err.code)
                  << " message=" << err.message.value_or("<none>") << std::endl;
    }
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
        std::this_thread::sleep_for(40ms);
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

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);

    auto tracePath = std::string{"/_system/trellis/state/"};
    tracePath.append(keys.front());
    tracePath.append("/stats/latest_trace");
    auto trace = backing->read<TrellisTraceSnapshot>(tracePath);
    REQUIRE(trace);
    for (auto const& event : trace->events) {
        std::cerr << "event: " << event.message << std::endl;
    }
    auto contains = [&](std::string_view needle) {
        return std::any_of(trace->events.begin(), trace->events.end(), [&](TrellisTraceEvent const& event) {
            return event.message.find(needle) != std::string::npos;
        });
    };
    CHECK(contains("wait_queue.block src=/await/a"));
    CHECK(contains("wait_queue.result src=/await/a outcome=success"));
    CHECK(contains("serve_queue.result src=/await/a outcome=empty"));
    CHECK(contains("serve_queue.result src=/await/a outcome=success"));
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
        std::this_thread::sleep_for(40ms);
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

TEST_CASE("Latest mode trace captures priority wake path") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/trace/out",
        .sources = {"/trace/a", "/trace/b"},
        .mode    = "latest",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto consumerFuture = std::async(std::launch::async, [&]() {
        return trellis.read<int>("/trace/out", Block{200ms});
    });

    std::this_thread::sleep_for(40ms);
    REQUIRE(backing->insert("/trace/b", 77).errors.empty());

    auto result = consumerFuture.get();
    REQUIRE(result);
    CHECK(result.value() == 77);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);

    auto tracePath = std::string{"/_system/trellis/state/"};
    tracePath.append(keys.front());
    tracePath.append("/stats/latest_trace");

    auto trace = backing->read<TrellisTraceSnapshot>(tracePath);
    REQUIRE(trace);

    auto containsMessage = [&](std::string_view needle) -> bool {
        return std::any_of(trace->events.begin(),
                           trace->events.end(),
                           [&](TrellisTraceEvent const& event) {
                               return event.message.find(needle) != std::string::npos;
                           });
    };

    CHECK(containsMessage("wait_latest.block"));
    CHECK(containsMessage("notify.ready output=/trace/out src=/trace/b"));
    CHECK(containsMessage("wait_latest.result policy=priority src=/trace/b outcome=success"));

    DisableTrellisCommand disable{.name = "/trace/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());

    auto traceAfterDisable = backing->read<TrellisTraceSnapshot>(tracePath);
    REQUIRE_FALSE(traceAfterDisable);
    auto code = traceAfterDisable.error().code;
    bool cleared = code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath;
    CHECK(cleared);
}

TEST_CASE("Latest mode priority polls secondary sources promptly") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/latency/out",
        .sources = {"/latency/a", "/latency/b"},
        .mode    = "latest",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto begin = std::chrono::steady_clock::now();
    auto consumer = std::async(std::launch::async, [&]() {
        return trellis.read<int>("/latency/out", Block{150ms});
    });

    std::this_thread::sleep_for(10ms);
    REQUIRE(backing->insert("/latency/b", 9).errors.empty());

    auto result = consumer.get();
    auto elapsed = std::chrono::steady_clock::now() - begin;
    REQUIRE(result);
    CHECK(result.value() == 9);
    CHECK(elapsed < 80ms);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto tracePath = std::string{"/_system/trellis/state/"};
    tracePath.append(keys.front());
    tracePath.append("/stats/latest_trace");
    auto trace = backing->read<TrellisTraceSnapshot>(tracePath);
    REQUIRE(trace);
    auto contains = [&](std::string_view needle) {
        return std::any_of(trace->events.begin(), trace->events.end(), [&](TrellisTraceEvent const& event) {
            return event.message.find(needle) != std::string::npos;
        });
    };
    CHECK(contains("wait_latest.ready policy=priority src=/latency/b outcome=success"));

    DisableTrellisCommand disable{.name = "/latency/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());
}

TEST_CASE("Latest mode priority wakes every source") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/wake/out",
        .sources = {"/wake/a", "/wake/b"},
        .mode    = "latest",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto produceAndConsume = [&](std::string const& src, int value) {
        auto begin = std::chrono::steady_clock::now();
        auto future = std::async(std::launch::async, [&]() {
            return trellis.read<int>("/wake/out", Block{200ms});
        });
        std::this_thread::sleep_for(10ms);
        REQUIRE(backing->insert(src, value).errors.empty());
        auto result = future.get();
        REQUIRE(result);
        CHECK(result.value() == value);
        auto elapsed = std::chrono::steady_clock::now() - begin;
        CHECK(elapsed < 120ms);
    };

    produceAndConsume("/wake/a", 301);
    produceAndConsume("/wake/b", 302);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);

    auto tracePath = std::string{"/_system/trellis/state/"};
    tracePath.append(keys.front());
    tracePath.append("/stats/latest_trace");

    auto trace = backing->read<TrellisTraceSnapshot>(tracePath);
    REQUIRE(trace);
    for (auto const& event : trace->events) {
        std::cerr << "event: " << event.message << std::endl;
    }

    auto contains = [&](std::string_view needle) {
        return std::any_of(trace->events.begin(), trace->events.end(), [&](TrellisTraceEvent const& event) {
            return event.message.find(needle) != std::string::npos;
        });
    };

    CHECK(contains("notify.ready output=/wake/out src=/wake/a"));
    CHECK(contains("notify.ready output=/wake/out src=/wake/b"));
    CHECK(contains("wait_latest.result policy=priority src=/wake/a outcome=success"));
    CHECK(contains("wait_latest.result policy=priority src=/wake/b outcome=success"));

    DisableTrellisCommand disable{.name = "/wake/out"};
    auto disableResult = trellis.insert("/_system/trellis/disable", disable);
    REQUIRE(disableResult.errors.empty());
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

    auto limitPath = std::string{"/_system/trellis/state/"};
    limitPath.append(keys.front());
    limitPath.append("/backpressure/max_waiters");
    auto limit = backing->read<std::uint32_t>(limitPath);
    REQUIRE(limit);
    CHECK(limit.value() == 0);

    auto legacyBackpressurePath = std::string{"/_system/trellis/state/"};
    legacyBackpressurePath.append(keys.front());
    legacyBackpressurePath.append("/config/max_waiters");
    auto legacyBackpressure = backing->read<std::uint32_t>(legacyBackpressurePath);
    if (!legacyBackpressure) {
        auto legacyCode      = legacyBackpressure.error().code;
        bool legacyNotFound  = legacyCode == Error::Code::NoObjectFound || legacyCode == Error::Code::NoSuchPath;
        CHECK(legacyNotFound);
    } else {
        CHECK(legacyBackpressure.value() == 0);
    }

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
    CHECK(stats->backpressureCount == 0);

    auto bufferedPath = statsPath + "/buffered_ready";
    auto bufferedReady = backing->read<std::uint64_t>(bufferedPath);
    if (!bufferedReady) {
        auto code = bufferedReady.error().code;
        bool expected = code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath;
        REQUIRE(expected);
    } else {
        CHECK(bufferedReady.value() == 0);
    }

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
    auto removedBuffered = backing->read<std::uint64_t>(bufferedPath);
    REQUIRE_FALSE(removedBuffered);
    auto bufferedCode = removedBuffered.error().code;
    bool bufferedCleared = bufferedCode == Error::Code::NoObjectFound
                           || bufferedCode == Error::Code::NoSuchPath;
    CHECK(bufferedCleared);

    auto removedLimit = backing->read<std::uint32_t>(limitPath);
    REQUIRE_FALSE(removedLimit);
    auto limitCode = removedLimit.error().code;
    bool limitCleared = limitCode == Error::Code::NoObjectFound
                        || limitCode == Error::Code::NoSuchPath;
    CHECK(limitCleared);

    auto removedLegacy = backing->read<std::uint32_t>(legacyBackpressurePath);
    REQUIRE_FALSE(removedLegacy);
    auto legacyCode = removedLegacy.error().code;
    bool legacyCleared = legacyCode == Error::Code::NoObjectFound
                         || legacyCode == Error::Code::NoSuchPath;
    CHECK(legacyCleared);

    auto legacyRootPath = std::string{"/_system/trellis/state/"};
    legacyRootPath.append(keys.front());
    auto legacyRoot = backing->read<std::string>(legacyRootPath.c_str());
    REQUIRE_FALSE(legacyRoot);
    auto rootCode = legacyRoot.error().code;
    bool rootCleared = rootCode == Error::Code::NoObjectFound
                       || rootCode == Error::Code::NoSuchPath;
    CHECK(rootCleared);
}

TEST_CASE("Legacy trellis config migrates to backpressure path") {
    auto backing = std::make_shared<PathSpace>();

    auto encodeKey = [](std::string_view path) {
        constexpr char hexDigits[] = "0123456789abcdef";
        std::string    encoded;
        encoded.reserve(path.size() * 2);
        for (unsigned char ch : path) {
            encoded.push_back(hexDigits[(ch >> 4) & 0x0F]);
            encoded.push_back(hexDigits[ch & 0x0F]);
        }
        return encoded;
    };

    auto const key        = encodeKey("/legacy/out");
    auto       stateRoot  = std::string{"/_system/trellis/state/"};
    stateRoot.append(key);

    TrellisPersistedConfig legacyConfig{
        .name    = "/legacy/out",
        .sources = {"/legacy/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    REQUIRE(backing->insert(stateRoot + "/config", legacyConfig).errors.empty());
    REQUIRE(backing->insert(stateRoot + "/config/max_waiters", static_cast<std::uint32_t>(3)).errors.empty());

    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/legacy/out",
        .sources = {"/legacy/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto configPath = stateRoot + "/config";
    auto stored     = backing->read<TrellisPersistedConfig>(configPath);
    REQUIRE(stored);
    CHECK(stored->name == "/legacy/out");

    auto limitPath = stateRoot + "/backpressure/max_waiters";
    auto limit     = backing->read<std::uint32_t>(limitPath);
    REQUIRE(limit);
    CHECK(limit.value() == 3);

    auto legacyLimit = backing->read<std::uint32_t>(stateRoot + "/config/max_waiters");
    REQUIRE_FALSE(legacyLimit);
    auto legacyCode = legacyLimit.error().code;
    bool legacyAbsent = legacyCode == Error::Code::NoObjectFound
                        || legacyCode == Error::Code::NoSuchPath;
    CHECK(legacyAbsent);
}

TEST_CASE("Persisted trellis config restores on new instance") {
    auto backing = std::make_shared<PathSpace>();
    {
        PathSpaceTrellis trellis(backing);
        EnableTrellisCommand enable{
            .name    = "/reload/out",
            .sources = {"/reload/a"},
            .mode    = "queue",
            .policy  = "round_robin,max_waiters=1",
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
    CHECK(stats->backpressureCount == 0);
    auto bufferedPath = statsPath + "/buffered_ready";
    auto bufferedReady = backing->read<std::uint64_t>(bufferedPath);
    if (!bufferedReady) {
        auto code = bufferedReady.error().code;
        bool expected = code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath;
        REQUIRE(expected);
    } else {
        CHECK(bufferedReady.value() == 0);
    }
}

TEST_CASE("Buffered readiness updates stats when queues fill") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/buffer/out",
        .sources = {"/buffer/source"},
        .mode    = "queue",
        .policy  = "priority,max_waiters=1",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/buffer/source", 7).errors.empty());
    trellis.notify("/buffer/source");

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto statsPath = std::string{"/_system/trellis/state/"};
    statsPath.append(keys.front());
    statsPath.append("/stats");

    auto bufferedPath = statsPath + "/buffered_ready";
    auto awaitBufferedReady = [&](std::uint64_t expected) -> std::uint64_t {
        Expected<std::uint64_t> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 250ms;
        do {
            snapshot = backing->read<std::uint64_t>(bufferedPath);
            if (snapshot && snapshot.value() == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        return snapshot.value();
    };

    CHECK(awaitBufferedReady(1) == 1);

    auto value = trellis.take<int>("/buffer/out", Block{});
    REQUIRE(value);
    CHECK(value.value() == 7);

    CHECK(awaitBufferedReady(0) == 0);
}

TEST_CASE("Internal runtime mirrors ready queue state") {
    ScopedEnvVar runtimeGuard("PATHSPACE_TRELLIS_INTERNAL_RUNTIME", "1");

    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/queue/out",
        .sources = {"/queue/a"},
        .mode    = "queue",
        .policy  = "priority",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    REQUIRE(backing->insert("/queue/a", 99).errors.empty());
    trellis.notify("/queue/a");

    auto internal = trellis.debugInternalSpace();
    REQUIRE(internal);
    ConcretePathStringView stateRoot{"/state"};
    auto awaitStateKey = [&]() -> std::string {
        auto deadline = std::chrono::steady_clock::now() + 250ms;
        std::vector<std::string> keys;
        do {
            keys = internal->listChildren(stateRoot);
            if (!keys.empty()) {
                return keys.front();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE_FALSE(keys.empty());
        return {};
    };
    auto runtimeBase = std::string{"/state/"};
    runtimeBase.append(awaitStateKey());
    runtimeBase.append("/runtime");

    auto readyQueuePath = runtimeBase + "/ready/queue";
    auto readyCountPath = runtimeBase + "/ready/count";

    auto awaitQueueSize = [&](std::size_t expected) -> std::vector<std::string> {
        Expected<std::vector<std::string>> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 250ms;
        do {
            snapshot = internal->read<std::vector<std::string>>(readyQueuePath);
            if (snapshot && snapshot->size() == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        REQUIRE(snapshot->size() == expected);
        return snapshot.value();
    };

    auto awaitReadyCount = [&](std::uint64_t expected) -> std::uint64_t {
        Expected<std::uint64_t> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 250ms;
        do {
            snapshot = internal->read<std::uint64_t>(readyCountPath);
            if (snapshot && snapshot.value() == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        REQUIRE(snapshot.value() == expected);
        return snapshot.value();
    };

    auto readyQueue = awaitQueueSize(1);
    CHECK(readyQueue.front() == "/queue/a");
    CHECK(awaitReadyCount(1) == 1);

    auto taken = trellis.take<int>("/queue/out", Block{100ms});
    REQUIRE(taken);
    CHECK(taken.value() == 99);

    auto queueAfter = awaitQueueSize(0);
    CHECK(queueAfter.empty());
    CHECK(awaitReadyCount(0) == 0);
}

TEST_CASE("Internal runtime tracks waiters, cursor, and shutdown") {
    ScopedEnvVar runtimeGuard("PATHSPACE_TRELLIS_INTERNAL_RUNTIME", "1");

    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/multi/out",
        .sources = {"/multi/a", "/multi/b"},
        .mode    = "queue",
        .policy  = "round_robin,max_waiters=1",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto internal = trellis.debugInternalSpace();
    REQUIRE(internal);
    ConcretePathStringView stateRoot{"/state"};
    auto awaitStateKey = [&]() -> std::string {
        auto deadline = std::chrono::steady_clock::now() + 250ms;
        std::vector<std::string> keys;
        do {
            keys = internal->listChildren(stateRoot);
            if (!keys.empty()) {
                return keys.front();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE_FALSE(keys.empty());
        return {};
    };
    auto runtimeBase = std::string{"/state/"};
    runtimeBase.append(awaitStateKey());
    runtimeBase.append("/runtime");

    auto waitersPath = runtimeBase + "/waiters";
    auto cursorPath  = runtimeBase + "/cursor";
    auto flagsPath   = runtimeBase + "/flags/shutting_down";

    auto readWaiters = [&]() -> TrellisRuntimeWaiterSnapshot {
        auto snapshot = internal->read<TrellisRuntimeWaiterSnapshot>(waitersPath);
        REQUIRE(snapshot);
        return snapshot.value();
    };

    auto awaitWaitersEmpty = [&]() -> TrellisRuntimeWaiterSnapshot {
        Expected<TrellisRuntimeWaiterSnapshot> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 750ms;
        do {
            snapshot = internal->read<TrellisRuntimeWaiterSnapshot>(waitersPath);
            if (snapshot && snapshot->entries.empty()) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        CAPTURE(snapshot->entries.size());
        if (snapshot && !snapshot->entries.empty()) {
            CAPTURE(snapshot->entries.front().source);
            CAPTURE(snapshot->entries.front().count);
        }
        CHECK(snapshot->entries.empty());
        return snapshot ? snapshot.value() : TrellisRuntimeWaiterSnapshot{};
    };

    auto awaitCursor = [&](std::uint64_t expected) -> std::uint64_t {
        Expected<std::uint64_t> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 300ms;
        do {
            snapshot = internal->read<std::uint64_t>(cursorPath);
            if (snapshot && snapshot.value() == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        REQUIRE(snapshot.value() == expected);
        return snapshot.value();
    };

    auto awaitFlags = [&](bool expected) -> TrellisRuntimeFlags {
        Expected<TrellisRuntimeFlags> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 300ms;
        do {
            snapshot = internal->read<TrellisRuntimeFlags>(flagsPath);
            if (snapshot && snapshot->shuttingDown == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        REQUIRE(snapshot->shuttingDown == expected);
        return snapshot.value();
    };

    CHECK(awaitCursor(0) == 0);

    std::promise<void> waiterReady;
    auto waiterFuture = waiterReady.get_future();
    auto asyncTake = std::async(std::launch::async, [&]() {
        waiterReady.set_value();
        return trellis.take<int>("/multi/out", Block{200ms});
    });

    waiterFuture.wait();
    std::this_thread::sleep_for(20ms);
    auto waitersSnapshot = readWaiters();
    if (!waitersSnapshot.entries.empty()) {
        CHECK(waitersSnapshot.entries.front().source == "/multi/a");
        CHECK(waitersSnapshot.entries.front().count >= 1);
    }

    REQUIRE(backing->insert("/multi/a", 10).errors.empty());
    trellis.notify("/multi/a");
    auto takenFirst = asyncTake.get();
    REQUIRE(takenFirst);
    CHECK(takenFirst.value() == 10);

    std::this_thread::sleep_for(20ms);
    auto waitersAfter = awaitWaitersEmpty();
    CHECK(waitersAfter.entries.empty());

    REQUIRE(backing->insert("/multi/a", 101).errors.empty());
    trellis.notify("/multi/a");
    auto first = trellis.take<int>("/multi/out", Block{100ms});
    REQUIRE(first);
    CHECK(first.value() == 101);

    CHECK(awaitCursor(1) == 1);

    REQUIRE(backing->insert("/multi/b", 202).errors.empty());
    trellis.notify("/multi/b");
    auto second = trellis.take<int>("/multi/out", Block{100ms});
    REQUIRE(second);
    CHECK(second.value() == 202);

    CHECK(awaitCursor(0) == 0);

    trellis.shutdown();

    auto flags = awaitFlags(true);
    CHECK(flags.shuttingDown);
}

TEST_CASE("Internal runtime multi-waiter shutdown clears waiters") {
    ScopedEnvVar runtimeGuard("PATHSPACE_TRELLIS_INTERNAL_RUNTIME", "1");

    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/stress/out",
        .sources = {"/stress/a"},
        .mode    = "queue",
        .policy  = "priority,max_waiters=2",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    auto internal = trellis.debugInternalSpace();
    REQUIRE(internal);
    ConcretePathStringView stateRoot{"/state"};
    auto awaitStateKey = [&]() -> std::string {
        auto deadline = std::chrono::steady_clock::now() + 300ms;
        std::vector<std::string> keys;
        do {
            keys = internal->listChildren(stateRoot);
            if (!keys.empty()) {
                return keys.front();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE_FALSE(keys.empty());
        return {};
    };
    auto runtimeBase = std::string{"/state/"};
    runtimeBase.append(awaitStateKey());
    runtimeBase.append("/runtime");

    auto waitersPath = runtimeBase + "/waiters";
    auto flagsPath   = runtimeBase + "/flags/shutting_down";

    auto awaitWaiterSnapshot = [&](std::uint64_t expectedTotal)
        -> TrellisRuntimeWaiterSnapshot {
        Expected<TrellisRuntimeWaiterSnapshot> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 2500ms;
        do {
            snapshot = internal->read<TrellisRuntimeWaiterSnapshot>(waitersPath);
            if (snapshot) {
                auto total = std::accumulate(snapshot->entries.begin(),
                                             snapshot->entries.end(),
                                             std::uint64_t{0},
                                             [](std::uint64_t acc, TrellisRuntimeWaiterEntry const& entry) {
                                                 return acc + entry.count;
                                             });
                if (total == expectedTotal) {
                    return snapshot.value();
                }
            } else if (expectedTotal == 0) {
                auto const code = snapshot.error().code;
                if (code == Error::Code::NoSuchPath || code == Error::Code::NotFound) {
                    return TrellisRuntimeWaiterSnapshot{};
                }
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        auto total = std::accumulate(snapshot->entries.begin(),
                                     snapshot->entries.end(),
                                     std::uint64_t{0},
                                     [](std::uint64_t acc, TrellisRuntimeWaiterEntry const& entry) {
                                         return acc + entry.count;
                                     });
        CAPTURE(total);
        if (snapshot && !snapshot->entries.empty()) {
            CAPTURE(snapshot->entries.front().source);
            CAPTURE(snapshot->entries.front().count);
        }
        REQUIRE(total == expectedTotal);
        return snapshot.value();
    };

    auto awaitFlags = [&](bool expected) -> TrellisRuntimeFlags {
        Expected<TrellisRuntimeFlags> snapshot;
        auto deadline = std::chrono::steady_clock::now() + 400ms;
        do {
            snapshot = internal->read<TrellisRuntimeFlags>(flagsPath);
            if (snapshot && snapshot->shuttingDown == expected) {
                return snapshot.value();
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(snapshot);
        REQUIRE(snapshot->shuttingDown == expected);
        return snapshot.value();
    };

    auto waiter1 = std::async(std::launch::async, [&]() {
        return trellis.take<int>("/stress/out", Block{1500ms});
    });

    auto waitersAfterFirst = awaitWaiterSnapshot(1);
    REQUIRE(waitersAfterFirst.entries.size() == 1);
    CHECK(waitersAfterFirst.entries.front().source == "/stress/a");
    CHECK(waitersAfterFirst.entries.front().count == 1);

    auto waiter2 = std::async(std::launch::async, [&]() {
        return trellis.take<int>("/stress/out", Block{1500ms});
    });

    auto waitersBeforeShutdown = awaitWaiterSnapshot(2);
    REQUIRE(waitersBeforeShutdown.entries.size() == 1);
    CHECK(waitersBeforeShutdown.entries.front().source == "/stress/a");
    CHECK(waitersBeforeShutdown.entries.front().count == 2);

    trellis.shutdown();

    auto flags = awaitFlags(true);
    CHECK(flags.shuttingDown);

    auto result1 = waiter1.get();
    auto result2 = waiter2.get();
    REQUIRE_FALSE(result1);
    REQUIRE(result1.error().code == Error::Code::Timeout);
    REQUIRE_FALSE(result2);
    REQUIRE(result2.error().code == Error::Code::Timeout);

    auto waitersAfterShutdown = awaitWaiterSnapshot(0);
    CHECK(waitersAfterShutdown.entries.empty());
}

TEST_CASE("Back-pressure limit caps simultaneous waiters per source") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/bp/out",
        .sources = {"/bp/source"},
        .mode    = "queue",
        .policy  = "priority,max_waiters=1",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    std::promise<void> ready;
    std::atomic<bool>  waiterRunning{false};
    std::optional<Expected<int>> waiterResult;
    std::thread waiter([&] {
        ready.set_value();
        waiterRunning.store(true, std::memory_order_release);
        waiterResult = trellis.take<int>("/bp/out", Block{500ms});
    });
    ready.get_future().wait();

    // Allow the first waiter to register.
    while (!waiterRunning.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);

    Expected<int> second;
    auto          capacityDeadline = std::chrono::steady_clock::now() + 300ms;
    do {
        second = trellis.take<int>("/bp/out", Block{50ms});
        if (!second) {
            if (second.error().code == Error::Code::CapacityExceeded) {
                break;
            }
            if (second.error().code == Error::Code::Timeout) {
                continue;
            }
        }
    } while (std::chrono::steady_clock::now() < capacityDeadline);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == Error::Code::CapacityExceeded);

    // Provide data to release the first waiter.
    REQUIRE(backing->insert("/bp/source", 42).errors.empty());
    trellis.notify("/bp/source");
    waiter.join();
    REQUIRE(waiterResult.has_value());
    REQUIRE(waiterResult->has_value());
    CHECK(waiterResult->value() == 42);

    ConcretePathStringView stateRoot{"/_system/trellis/state"};
    auto                   keys = backing->listChildren(stateRoot);
    REQUIRE(keys.size() == 1);
    auto statsPath = std::string{"/_system/trellis/state/"};
    statsPath.append(keys.front());
    statsPath.append("/stats");

    auto stats = backing->read<TrellisStats>(statsPath);
    REQUIRE(stats);
    CHECK(stats->servedCount == 1);
    CHECK(stats->backpressureCount == 1);
    CHECK(stats->errorCount >= 1);
    CHECK(stats->lastErrorCode == static_cast<std::int32_t>(Error::Code::CapacityExceeded));
}

TEST_CASE("Trellis stats capture wait counts when blocking") {
    auto backing = std::make_shared<PathSpace>();
    PathSpaceTrellis trellis(backing);

    EnableTrellisCommand enable{
        .name    = "/stats/out",
        .sources = {"/stats/source"},
        .mode    = "queue",
        .policy  = "priority,max_waiters=2",
    };
    auto enableResult = trellis.insert("/_system/trellis/enable", enable);
    REQUIRE(enableResult.errors.empty());

    std::thread producer([backing, &trellis] {
        std::this_thread::sleep_for(25ms);
        backing->insert("/stats/source", 9001);
        trellis.notify("/stats/source");
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
    CHECK(stats->backpressureCount == 0);
}

} // TEST_SUITE

} // namespace SP
