#include <doctest/doctest.h>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <core/Out.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace SP;
using namespace std::chrono_literals;

namespace {

auto mountTrellis(std::shared_ptr<PathSpace> const& space) -> PathSpaceTrellis* {
    auto trellis = std::make_unique<PathSpaceTrellis>(space);
    auto* raw    = trellis.get();
    auto ret     = space->insert<"/cursor">(std::move(trellis));
    REQUIRE(ret.errors.empty());
    return raw;
}

} // namespace

TEST_SUITE("PathSpaceTrellis") {
    TEST_CASE("enable/disable commands update registry") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);

        auto enableMouse = space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        CHECK(enableMouse.errors.empty());

        auto enableGamepad = space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});
        CHECK(enableGamepad.errors.empty());

        auto sources = trellis->debugSources();
        std::sort(sources.begin(), sources.end());
        CHECK(sources == std::vector<std::string>{"/data/gamepad", "/data/mouse"});

        // Duplicate enable is idempotent
        auto duplicateEnable = space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        CHECK(duplicateEnable.errors.empty());
        CHECK_EQ(trellis->debugSources().size(), 2);

        // Disable removes the source
        auto disableMouse = space->insert<"/cursor/_system/disable">(std::string{"/data/mouse"});
        CHECK(disableMouse.errors.empty());
        CHECK_EQ(trellis->debugSources(), std::vector<std::string>{"/data/gamepad"});
    }

    TEST_CASE("copyable inserts fan out to every source") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        auto ret = space->insert<"/cursor">(42);
        CHECK(ret.errors.empty());

        auto mouse = space->take<int>("/data/mouse");
        REQUIRE(mouse);
        CHECK_EQ(mouse.value(), 42);

        auto gamepad = space->take<int>("/data/gamepad");
        REQUIRE(gamepad);
        CHECK_EQ(gamepad.value(), 42);
    }

    TEST_CASE("move-only inserts target first available source") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        auto ret = space->insert<"/cursor">(std::make_unique<int>(7));
        CHECK(ret.errors.empty());

        auto mouse = space->take<std::unique_ptr<int>>("/data/mouse");
        auto pad   = space->take<std::unique_ptr<int>>("/data/gamepad");

        bool mouseReceived   = mouse.has_value();
        bool gamepadReceived = pad.has_value();
        CHECK(mouseReceived != gamepadReceived); // exactly one source receives the payload
        CHECK(mouseReceived || gamepadReceived);
        if (mouseReceived) {
            CHECK_EQ(*mouse.value(), 7);
        } else if (gamepadReceived) {
            CHECK_EQ(*pad.value(), 7);
        }
    }

    TEST_CASE("read via trellis follows round robin ordering") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        space->insert<"/data/mouse">(11);
        space->insert<"/data/gamepad">(22);

        auto first = space->take<int>("/cursor");
        REQUIRE(first);
        CHECK_EQ(first.value(), 11);

        space->insert<"/data/mouse">(33);
        space->insert<"/data/gamepad">(44);

        auto second = space->take<int>("/cursor");
        REQUIRE(second);
        CHECK_EQ(second.value(), 22);
    }

    TEST_CASE("bypass writes forward to backing space") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        auto ret = space->insert<"/cursor/log/events">(5);
        CHECK(ret.errors.empty());

        auto value = space->take<int>("/cursor/log/events");
        REQUIRE(value);
        CHECK_EQ(value.value(), 5);
    }

    TEST_CASE("blocking take waits until a source publishes") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        std::promise<int> received;
        auto future = received.get_future();

        std::thread consumer([&]() {
            auto value = space->take<int>("/cursor", Block{100ms});
            REQUIRE(value);
            received.set_value(value.value());
        });

        std::this_thread::sleep_for(5ms);
        space->insert<"/data/gamepad">(77);

        CHECK_EQ(future.get(), 77);
        consumer.join();
    }

    TEST_CASE("take with timeout returns timeout when no sources ready") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});

        auto result = space->take<int>("/cursor", Block{10ms});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Error::Code::Timeout || result.error().code == Error::Code::NoObjectFound);
    }

    TEST_CASE("shutdown wakes blocked readers") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});

        std::promise<Error::Code> observed;
        auto future = observed.get_future();

        std::thread consumer([&]() {
            auto value = space->take<int>("/cursor", Block{});
            REQUIRE_FALSE(value.has_value());
            observed.set_value(value.error().code);
        });

        std::this_thread::sleep_for(5ms);
        trellis->shutdown();

        auto code = future.get();
        CHECK(code == Error::Code::Timeout || code == Error::Code::NoObjectFound);
        consumer.join();
    }

    TEST_CASE("concurrent producers and consumers preserve delivery") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        constexpr int perProducer   = 10;
        constexpr int producerCount = 2;
        constexpr int expectedTotal = perProducer * producerCount;

        std::atomic<int> consumed{0};
        std::mutex       valuesMutex;
        std::vector<int> values;
        values.reserve(expectedTotal);

        std::thread prodMouse([&]() {
            for (int i = 0; i < perProducer; ++i) {
                space->insert<"/data/mouse">(1000 + i);
            }
        });
        std::thread prodGamepad([&]() {
            for (int i = 0; i < perProducer; ++i) {
                space->insert<"/data/gamepad">(2000 + i);
            }
        });

        auto consumerWorker = [&]() {
            while (consumed.load(std::memory_order_relaxed) < expectedTotal) {
                auto value = space->take<int>("/cursor", Block{50ms});
                if (!value.has_value())
                    continue;
                {
                    std::lock_guard<std::mutex> lock(valuesMutex);
                    values.push_back(value.value());
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        };

        std::thread consA(consumerWorker);
        std::thread consB(consumerWorker);

        prodMouse.join();
        prodGamepad.join();
        consA.join();
        consB.join();

        CHECK_EQ(consumed.load(), expectedTotal);
        std::sort(values.begin(), values.end());
        CHECK(values.size() == static_cast<size_t>(expectedTotal));
        CHECK(std::count_if(values.begin(), values.end(), [](int v) { return v >= 1000 && v < 1000 + perProducer; }) == perProducer);
        CHECK(std::count_if(values.begin(), values.end(), [](int v) { return v >= 2000 && v < 2000 + perProducer; }) == perProducer);
    }
}
