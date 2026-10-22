#include "SnapshotCachedPathSpaceTestSupport.hpp"

namespace {

struct BlockingVisitSpace final : PathSpace {
    using PathSpace::PathSpace;

    auto visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> override {
        if (!armed.load(std::memory_order_acquire)) {
            return PathSpace::visit(visitor, options);
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++entered;
            cv.notify_all();
            cv.wait(lock, [&]() { return releases > 0; });
            --releases;
        }
        return PathSpace::visit(visitor, options);
    }

    // Visits pass straight through until armed, so a background snapshot worker that
    // starts before the test stops it cannot get stuck in a blocked visit.
    void arm() {
        armed.store(true, std::memory_order_release);
    }

    void waitForVisits(int count) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return entered >= count; });
    }

    void releaseVisits(int count) {
        std::lock_guard<std::mutex> lock(mutex);
        releases += count;
        cv.notify_all();
    }

    std::atomic<bool>       armed{false};
    std::mutex              mutex;
    std::condition_variable cv;
    int                     entered = 0;
    int                     releases = 0;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.snapshot_cache.rebuild");

TEST_CASE("Snapshot cache rebuild returns when disabled while waiting") {
    auto backing = std::make_shared<BlockingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    cached.stopSnapshotWorker();
    backing->arm();

    std::thread first([&]() { cached.rebuildSnapshotNow(); });
    backing->waitForVisits(1);

    std::atomic<bool> secondStarted{false};
    std::thread second([&]() {
        secondStarted.store(true, std::memory_order_release);
        cached.rebuildSnapshotNow();
    });

    while (!secondStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    backing->releaseVisits(1);

    first.join();
    second.join();
}

TEST_CASE("Snapshot cache rebuild waits for in-progress rebuild") {
    auto backing = std::make_shared<BlockingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    cached.stopSnapshotWorker();
    backing->arm();

    std::thread first([&]() { cached.rebuildSnapshotNow(); });
    backing->waitForVisits(1);

    std::atomic<bool> secondStarted{false};
    std::thread second([&]() {
        secondStarted.store(true, std::memory_order_release);
        cached.rebuildSnapshotNow();
    });

    while (!secondStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    backing->releaseVisits(2);

    first.join();
    second.join();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds >= 2);
}

TEST_CASE("Snapshot cache rebuild aborts if disabled mid-visit") {
    auto backing = std::make_shared<BlockingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    cached.stopSnapshotWorker();
    backing->arm();

    std::thread rebuild([&]() { cached.rebuildSnapshotNow(); });
    backing->waitForVisits(1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    backing->releaseVisits(1);
    rebuild.join();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds == 0);
}

TEST_CASE("Snapshot cache stays dirty when mutation happens mid-rebuild") {
    auto backing = std::make_shared<BlockingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    cached.stopSnapshotWorker();
    backing->arm();

    std::thread rebuild([&]() { cached.rebuildSnapshotNow(); });
    backing->waitForVisits(1);

    CHECK(cached.insert("/other", 2).nbrValuesInserted == 1);
    backing->releaseVisits(1);
    rebuild.join();

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
}

TEST_CASE("Snapshot cache trySnapshotRead returns false when disabled mid-rebuild") {
    auto backing = std::make_shared<BlockingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 3).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    cached.stopSnapshotWorker();
    backing->arm();
    cached.markSnapshotDirty("/value");

    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;
    std::atomic<bool> result{false};
    std::thread reader([&]() {
        result.store(cached.trySnapshotRead(Iterator{"/value"}, meta, Out{}, &outValue),
                     std::memory_order_release);
    });

    backing->waitForVisits(1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    backing->releaseVisits(1);
    reader.join();

    CHECK_FALSE(result.load(std::memory_order_acquire));
}

TEST_CASE("Snapshot cache synchronous rebuild happens on read when enabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 5).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    cached.stopSnapshotWorker();

    InputMetadata meta{InputMetadataT<int>{}};
    int outValue = 0;
    CHECK(cached.trySnapshotRead(Iterator{"/value"}, meta, Out{}, &outValue));
    CHECK(outValue == 5);

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds >= 1);
}

TEST_CASE("Snapshot cache starts dirty before first rebuild") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 10).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 10);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache re-enable starts dirty before rebuild") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 6).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto initial = cached.read<int>("/value");
    REQUIRE(initial.has_value());
    CHECK(initial.value() == 6);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 6);

    auto after = cached.snapshotMetrics();
    CHECK(after.misses >= before.misses + 1);
    CHECK(after.hits == before.hits);
}

TEST_CASE("Snapshot cache hits after rebuild clears initial dirty") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 12).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 12);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits >= before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache rebuild is ignored before configuration") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.rebuildSnapshotNow();
    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds == 0);
    CHECK(metrics.rebuildFailures == 0);
    CHECK(metrics.bytes == 0);
}

TEST_CASE("Snapshot cache root dirty clears after rebuild") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/a", 1).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 2).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 1,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/a", 3, ReplaceExisting{}).nbrValuesInserted == 1);
    CHECK(cached.insert("/b", 4, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readA = cached.read<int>("/a");
    REQUIRE(readA.has_value());
    CHECK(readA.value() == 3);
    auto dirtyMetrics = cached.snapshotMetrics();
    CHECK(dirtyMetrics.misses >= before.misses + 1);

    cached.rebuildSnapshotNow();
    auto beforeHit = cached.snapshotMetrics();
    auto hitA = cached.read<int>("/a");
    REQUIRE(hitA.has_value());
    CHECK(hitA.value() == 3);
    auto afterHit = cached.snapshotMetrics();
    CHECK(afterHit.hits >= beforeHit.hits + 1);
    CHECK(afterHit.misses == beforeHit.misses);
}

TEST_CASE("Snapshot cache dirty roots reset after rebuild") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    auto dirtyRead = cached.read<int>("/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 2);

    auto dirtyMetrics = cached.snapshotMetrics();
    CHECK(dirtyMetrics.misses >= 1);

    cached.rebuildSnapshotNow();
    auto before = cached.snapshotMetrics();
    auto readAfter = cached.read<int>("/value");
    REQUIRE(readAfter.has_value());
    CHECK(readAfter.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.hits >= before.hits + 1);
    CHECK(after.misses == before.misses);
}

TEST_CASE("Snapshot cache rebuild refreshes values after mutations") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 4).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 7, ReplaceExisting{}).nbrValuesInserted == 1);
    auto dirtyRead = cached.read<int>("/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 7);

    cached.rebuildSnapshotNow();
    auto refreshed = cached.read<int>("/value");
    REQUIRE(refreshed.has_value());
    CHECK(refreshed.value() == 7);
}

TEST_CASE("Snapshot cache rebuild clears dirty roots for hits") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);
    auto dirtyRead = cached.read<int>("/value");
    REQUIRE(dirtyRead.has_value());
    CHECK(dirtyRead.value() == 2);

    auto dirtyMetrics = cached.snapshotMetrics();
    CHECK(dirtyMetrics.misses >= 1);

    cached.rebuildSnapshotNow();
    auto beforeHit = cached.snapshotMetrics();
    auto hitRead = cached.read<int>("/value");
    REQUIRE(hitRead.has_value());
    CHECK(hitRead.value() == 2);

    auto afterHit = cached.snapshotMetrics();
    CHECK(afterHit.hits >= beforeHit.hits + 1);
    CHECK(afterHit.misses == beforeHit.misses);
}

TEST_CASE("Snapshot cache rebuild is ignored when disabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();

    auto metrics = cached.snapshotMetrics();
    CHECK(metrics.rebuilds == 0);
    CHECK(metrics.bytes == 0);
}

TEST_CASE("Snapshot cache rebuild after re-enable starts fresh") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 4).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    auto first = cached.snapshotMetrics();
    CHECK(first.rebuilds >= 1);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = false,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    auto disabled = cached.snapshotMetrics();
    CHECK(disabled.rebuilds == 0);

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    });
    cached.rebuildSnapshotNow();
    auto second = cached.snapshotMetrics();
    CHECK(second.rebuilds >= 1);
}

TEST_CASE("Snapshot cache synchronous rebuild disabled does not rebuild on read") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = false,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.rebuilds == before.rebuilds);
}

TEST_CASE("Snapshot cache synchronous rebuild triggers when enabled") {
    auto backing = std::make_shared<PathSpace>();
    SnapshotCachedPathSpace cached{backing};

    CHECK(cached.insert("/value", 1).nbrValuesInserted == 1);
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    cached.rebuildSnapshotNow();

    CHECK(cached.insert("/value", 2, ReplaceExisting{}).nbrValuesInserted == 1);

    auto before = cached.snapshotMetrics();
    auto readValue = cached.read<int>("/value");
    REQUIRE(readValue.has_value());
    CHECK(readValue.value() == 2);

    auto after = cached.snapshotMetrics();
    CHECK(after.rebuilds >= before.rebuilds);
}

TEST_SUITE_END();
