#include "third_party/doctest.h"
#include <pathspace/PathSpace.hpp>
#include <atomic>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE_BEGIN("pathspace.snapshot");

TEST_CASE("Snapshot read parity and dirty fallback") {
    PathSpace space;
    CHECK(space.insert("/a", 1).nbrValuesInserted == 1);

    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 16,
    });

    auto first = space.read<int>("/a");
    REQUIRE(first.has_value());
    CHECK(first.value() == 1);

    CHECK(space.insert("/b", 10).nbrValuesInserted == 1);

    auto readA = space.read<int>("/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 1);

    auto readB = space.read<int>("/b");
    REQUIRE(readB.has_value());
    CHECK(readB.value() == 10);
}

TEST_CASE("Snapshot metrics reflect hits and misses") {
    PathSpace space;
    CHECK(space.insert("/root/value", 5).nbrValuesInserted == 1);
    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    space.rebuildSnapshotNow();

    auto metrics = space.snapshotMetrics();
    CHECK(metrics.rebuilds == 1);
    CHECK(metrics.bytes > 0);

    auto hit = space.read<int>("/root/value");
    REQUIRE(hit.has_value());
    CHECK(hit.value() == 5);

    metrics = space.snapshotMetrics();
    CHECK(metrics.hits >= 1);

    CHECK(space.insert("/root/value", 6).nbrValuesInserted == 1);
    auto miss = space.read<int>("/root/value");
    REQUIRE(miss.has_value());
    CHECK(miss.value() == 5);

    metrics = space.snapshotMetrics();
    CHECK(metrics.misses >= 1);
}

TEST_CASE("Snapshot dirty subspace falls back while other paths hit") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    CHECK(nested->insert("/value", 7).nbrValuesInserted == 1);
    CHECK(nested->insert("/value", 8).nbrValuesInserted == 1);
    CHECK(space.insert("/nested", std::move(nested)).nbrSpacesInserted == 1);
    CHECK(space.insert("/other", 3).nbrValuesInserted == 1);

    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    space.rebuildSnapshotNow();

    auto baseline = space.read<int>("/nested/value");
    REQUIRE(baseline.has_value());
    CHECK(baseline.value() == 7);

    auto other = space.read<int>("/other");
    REQUIRE(other.has_value());
    CHECK(other.value() == 3);

    auto before = space.snapshotMetrics();

    auto taken = space.take<int>("/nested/value");
    REQUIRE(taken.has_value());
    CHECK(taken.value() == 7);

    auto dirtyRead = space.read<int>("/nested/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 8);

    auto cleanRead = space.read<int>("/other");
    REQUIRE(cleanRead.has_value());
    CHECK(cleanRead.value() == 3);

    auto after = space.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits >= before.hits + 1);
}

TEST_CASE("Snapshot clears on PathSpace::clear") {
    PathSpace space;
    CHECK(space.insert("/value", 9).nbrValuesInserted == 1);
    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    space.rebuildSnapshotNow();

    auto before = space.read<int>("/value");
    REQUIRE(before.has_value());
    CHECK(before.value() == 9);

    space.clear();

    auto after = space.read<int>("/value");
    CHECK(after.has_error());
}

TEST_CASE("Snapshot supports indexed and span reads") {
    PathSpace space;
    CHECK(space.insert("/ints", 1).nbrValuesInserted == 1);
    CHECK(space.insert("/ints", 2).nbrValuesInserted == 1);
    CHECK(space.insert("/ints", 3).nbrValuesInserted == 1);

    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    space.rebuildSnapshotNow();

    auto indexed = space.read<int>("/ints[1]");
    REQUIRE(indexed.has_value());
    CHECK(indexed.value() == 2);

    int spanValue = 0;
    auto spanResult = space.read("/ints", [&](std::span<const int> values) {
        if (!values.empty()) {
            spanValue = values.front();
        }
    });
    CHECK(spanResult.has_value());
    CHECK(spanValue == 1);
}

TEST_CASE("Snapshot handles concurrent reads with take/insert churn") {
    PathSpace space;
    CHECK(space.insert("/churn", 1).nbrValuesInserted == 1);
    CHECK(space.insert("/stable", 4).nbrValuesInserted == 1);

    space.setSnapshotOptions(PathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 16,
    });
    space.rebuildSnapshotNow();

    std::atomic<bool> running{true};
    std::thread writer([&]() {
        for (int i = 0; i < 50; ++i) {
            auto taken = space.take<int>("/churn");
            if (taken.has_value()) {
                CHECK(space.insert("/churn", taken.value() + 1).nbrValuesInserted == 1);
            }
            std::this_thread::sleep_for(1ms);
        }
        running.store(false, std::memory_order_release);
    });

    while (running.load(std::memory_order_acquire)) {
        auto stable = space.read<int>("/stable");
        REQUIRE(stable.has_value());
        CHECK(stable.value() == 4);
    }

    writer.join();
}

TEST_SUITE_END();
