#include "core/NodeData.hpp"
#include "type/InputData.hpp"
#include "core/NotificationSink.hpp"
#include "type/InputMetadataT.hpp"
#include <pathspace/PathSpace.hpp>

#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace SP;

TEST_SUITE("core.nodedata.nested") {
    TEST_CASE("copy/assign drop nested payload metadata") {
        NodeData original;
        auto     nested = std::make_unique<PathSpace>();

        REQUIRE_FALSE(original.serialize(InputData(nested)).has_value());
        CHECK(original.hasNestedSpaces());

        NodeData copied{original};
        CHECK_FALSE(copied.hasNestedSpaces());
        CHECK(copied.empty());

        NodeData assigned;
        assigned = original;
        CHECK_FALSE(assigned.hasNestedSpaces());
        CHECK(assigned.empty());
    }

    TEST_CASE("remove middle nested updates type queue order") {
        NodeData node;
        int      v1 = 1;
        int      v2 = 2;

        auto n1 = std::make_unique<PathSpace>();
        auto n2 = std::make_unique<PathSpace>();

        REQUIRE_FALSE(node.serialize(InputData{v1}).has_value());
        REQUIRE_FALSE(node.serialize(InputData{n1}).has_value());
        REQUIRE_FALSE(node.serialize(InputData{v2}).has_value());
        REQUIRE_FALSE(node.serialize(InputData{n2}).has_value());

        // Remove the second nested (index 1) while keeping the first in place
        auto removed = node.takeNestedAt(1);
        REQUIRE(removed != nullptr);
        CHECK(node.nestedCount() == 1);

        auto summary = node.typeSummary();
        REQUIRE(summary.size() == 3);
        CHECK(summary[0].category == DataCategory::Fundamental);
        CHECK(summary[1].category == DataCategory::UniquePtr);
        CHECK(summary[2].category == DataCategory::Fundamental);
    }

    TEST_CASE("retarget tasks preserves task queue integrity") {
        NodeData data;
        // Insert lazy execution task
        auto task = Task::Create([](Task&, bool) {});
        InputData input{task};
        input.task     = task;
        input.executor = nullptr;
        REQUIRE_FALSE(data.serialize(input).has_value());

        // Retarget to a mock sink/executor (nullptr ok for this test)
        CHECK_NOTHROW(data.retargetTasks(std::weak_ptr<NotificationSink>{}, nullptr));
        CHECK_FALSE(data.peekFuture().has_value());
    }

    TEST_CASE("serializeSnapshot retains nested ordering placeholders") {
        NodeData data;
        auto nested = std::make_unique<PathSpace>();
        InputData nestedInput{std::move(nested)};
        REQUIRE_FALSE(data.serialize(nestedInput).has_value());

        int value = 7;
        InputData valueInput{value};
        REQUIRE_FALSE(data.serialize(valueInput).has_value());

        auto snapshot = data.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restored = NodeData::deserializeSnapshot(*snapshot);
        REQUIRE(restored.has_value());

        auto summary = restored->typeSummary();
        REQUIRE(summary.size() == 2);
        CHECK(summary[0].category == DataCategory::UniquePtr);
        CHECK(summary[1].category == DataCategory::Fundamental);
    }

    TEST_CASE("deserializeSnapshot keeps value readable after placeholder nested") {
        NodeData data;
        auto nested = std::make_unique<PathSpace>();
        REQUIRE_FALSE(data.serialize(InputData{std::move(nested)}).has_value());

        int value = 11;
        REQUIRE_FALSE(data.serialize(InputData{value}).has_value());

        auto snapshot = data.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restored = NodeData::deserializeSnapshot(*snapshot);
        REQUIRE(restored.has_value());

        int out = 0;
        InputMetadataT<int> meta{};
        auto err = restored->deserialize(&out, meta);
        CHECK_FALSE(err.has_value());
        CHECK(out == value);
    }

    TEST_CASE("deserializePop skips missing nested placeholder to reach value") {
        NodeData data;
        auto nested = std::make_unique<PathSpace>();
        REQUIRE_FALSE(data.serialize(InputData{std::move(nested)}).has_value());

        int value = 23;
        REQUIRE_FALSE(data.serialize(InputData{value}).has_value());

        auto snapshot = data.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restored = NodeData::deserializeSnapshot(*snapshot);
        REQUIRE(restored.has_value());

        int out = 0;
        InputMetadataT<int> meta{};
        auto err = restored->deserializePop(&out, meta);
        CHECK_FALSE(err.has_value());
        CHECK(out == value);
        CHECK(restored->valueCount() == 0);
    }

    TEST_CASE("takeNestedAt blocks until borrow releases") {
        using namespace std::chrono_literals;
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());

        std::atomic<bool> takeDone{false};

        auto borrowed = data.borrowNestedShared(0);
        REQUIRE(borrowed);

        std::thread taker([&]() {
            auto removed = data.takeNestedAt(0);
            (void)removed;
            takeDone.store(true);
        });

        std::this_thread::sleep_for(20ms);
        CHECK_FALSE(takeDone.load()); // still blocked while borrow held

        borrowed.reset(); // release borrow
        taker.join();

        CHECK(takeDone.load());
    }

    TEST_CASE("takeNestedAt waits for multiple borrows of same slot") {
        using namespace std::chrono_literals;
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());

        auto b1 = data.borrowNestedShared(0);
        auto b2 = data.borrowNestedShared(0);
        REQUIRE(b1);
        REQUIRE(b2);

        std::atomic<bool> takeFinished{false};
        std::thread taker([&]() {
            auto removed = data.takeNestedAt(0);
            (void)removed;
            takeFinished.store(true);
        });

        std::this_thread::sleep_for(20ms);
        CHECK_FALSE(takeFinished.load());

        b1.reset();
        std::this_thread::sleep_for(20ms);
        CHECK_FALSE(takeFinished.load()); // still blocked because b2 lives

        b2.reset();
        taker.join();
        CHECK(takeFinished.load());
    }

    TEST_CASE("borrow from placeholder restored slot returns null") {
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        auto snapshot = data.serializeSnapshot();
        REQUIRE(snapshot.has_value());

        auto restored = NodeData::deserializeSnapshot(*snapshot);
        REQUIRE(restored.has_value());

        auto borrowed = restored->borrowNestedShared(0);
        CHECK_FALSE(borrowed); // placeholder has no payload

        auto removed = restored->takeNestedAt(0);
        CHECK(removed == nullptr); // removal of placeholder succeeds
        CHECK(restored->nestedCount() == 0);
    }

    TEST_CASE("borrow survives slot replacement via emplaceNestedAt") {
        using namespace std::chrono_literals;
        // Build placeholder slot via snapshot/restore
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        auto snapshot = data.serializeSnapshot();
        REQUIRE(snapshot.has_value());
        auto restored = NodeData::deserializeSnapshot(*snapshot);
        REQUIRE(restored.has_value());

        // Replace placeholder with real payload
        auto replacement = std::unique_ptr<PathSpaceBase>(std::make_unique<PathSpace>().release());
        auto err = restored->emplaceNestedAt(0, replacement);
        CHECK_FALSE(err.has_value());

        // Borrow sees new payload
        auto borrow = restored->borrowNestedShared(0);
        REQUIRE(borrow);

        std::atomic<bool> takeFinished{false};
        std::unique_ptr<PathSpaceBase> taken;
        std::thread taker([&]() {
            taken = restored->takeNestedAt(0);
            takeFinished.store(true);
        });

        std::this_thread::sleep_for(20ms);
        CHECK_FALSE(takeFinished.load()); // blocked by outstanding borrow

        borrow.reset();
        taker.join();

        CHECK(takeFinished.load());
        CHECK(taken != nullptr);
    }

    TEST_CASE("NodeData destructor can run while nested borrow lives on") {
        auto holder = std::make_unique<NodeData>();
        REQUIRE_FALSE(holder->serialize(InputData{std::make_unique<PathSpace>()}).has_value());

        auto borrowed = holder->borrowNestedShared(0);
        REQUIRE(borrowed);

        std::thread destroyer([&]() { holder.reset(); });

        // Destructor should complete promptly; slot lifetime is extended by the borrow alias.
        destroyer.join();
        CHECK(holder == nullptr);

        // Borrow remains usable.
        CHECK(borrowed != nullptr);
        borrowed.reset();
    }

    TEST_CASE("NodeData destructor does not hang forever on leaked borrow") {
        using namespace std::chrono_literals;

        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        auto borrowed = data.borrowNestedShared(0);
        REQUIRE(borrowed);

        std::atomic<bool> destroyed{false};
        std::thread destroyer([&]() mutable {
            auto local = std::make_unique<NodeData>(std::move(data));
            local.reset(); // should not hang
            destroyed.store(true);
        });

        std::this_thread::sleep_for(50ms);
        CHECK(destroyed.load() == true);

        borrowed.reset(); // clean up borrow regardless of outcome
        destroyer.join();
    }

    TEST_CASE("borrowNestedShared survives move of NodeData") {
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        auto borrowed = data.borrowNestedShared(0);
        REQUIRE(borrowed);

        NodeData moved{std::move(data)};
        CHECK(moved.nestedCount() == 1);

        borrowed.reset();
        auto taken = moved.takeNestedAt(0);
        CHECK(taken != nullptr);
    }

    TEST_CASE("takeNestedAt blocks across move while borrow is held") {
        using namespace std::chrono_literals;
        for (int i = 0; i < 5; ++i) { // small stress loop to exercise move/borrow races
            NodeData first;
            REQUIRE_FALSE(first.serialize(InputData{std::make_unique<PathSpace>()}).has_value());

            auto borrowed = first.borrowNestedShared(0);
            REQUIRE(borrowed);

            NodeData second{std::move(first)};
            NodeData target{std::move(second)};

            std::atomic<bool> takeFinished{false};
            std::unique_ptr<PathSpaceBase> taken;
            std::thread taker([&]() {
                taken        = target.takeNestedAt(0);
                takeFinished = true;
            });

            std::this_thread::sleep_for(5ms);
            CHECK_FALSE(takeFinished.load()); // should still be waiting on borrow

            borrowed.reset(); // release borrow to unblock
            taker.join();

            CHECK(takeFinished.load());
            CHECK(taken != nullptr);
            CHECK(target.nestedCount() == 0);
        }
    }

    TEST_CASE("borrowed later nested survives earlier removal without deadlock") {
        using namespace std::chrono_literals;
        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        REQUIRE(data.nestedCount() == 2);

        auto borrowedSecond = data.borrowNestedShared(1);
        REQUIRE(borrowedSecond);

        // Remove the first (front) nested; should not affect the outstanding borrow on the second.
        auto removedFront = data.takeNestedAt(0);
        REQUIRE(removedFront != nullptr);
        CHECK(data.nestedCount() == 1);

        std::atomic<bool> takeStarted{false};
        std::atomic<bool> takeFinished{false};
        std::unique_ptr<PathSpaceBase> removedRemaining;
        std::thread taker([&]() {
            takeStarted.store(true);
            removedRemaining = data.takeNestedAt(0);
            takeFinished.store(true);
        });

        // Give the taker a moment to block on the outstanding borrow.
        std::this_thread::sleep_for(20ms);
        CHECK(takeStarted.load());
        CHECK_FALSE(takeFinished.load());

        // Releasing the borrow should allow the taker to complete promptly.
        borrowedSecond.reset();
        taker.join();

        CHECK(takeFinished.load());
        CHECK(removedRemaining != nullptr);
        CHECK(data.nestedCount() == 0);
    }

    TEST_CASE("pointer-based borrow accounting survives repeated compaction") {
        using namespace std::chrono_literals;
        for (int iter = 0; iter < 8; ++iter) {
            NodeData data;
            // Populate several nested entries.
            for (int i = 0; i < 4; ++i) {
                REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
            }
            REQUIRE(data.nestedCount() == 4);

            // Borrow a middle element, then remove fronts repeatedly to force compaction.
            auto midBorrow = data.borrowNestedShared(2);
            REQUIRE(midBorrow);

            auto taken0 = data.takeNestedAt(0);
            auto taken1 = data.takeNestedAt(0); // formerly index 1, shifts after erase
            REQUIRE(taken0 != nullptr);
            REQUIRE(taken1 != nullptr);
            CHECK(data.nestedCount() == 2);

            std::atomic<bool> takeDone{false};
            std::unique_ptr<PathSpaceBase> finalTaken;
            std::thread taker([&]() {
                finalTaken = data.takeNestedAt(0); // should wait until borrow released
                takeDone.store(true);
            });

            std::this_thread::sleep_for(10ms);
            CHECK_FALSE(takeDone.load());

            midBorrow.reset(); // release borrow so taker can finish
            taker.join();
            CHECK(takeDone.load());
            CHECK(finalTaken != nullptr);
            CHECK(data.nestedCount() <= 1);
        }
    }
}
