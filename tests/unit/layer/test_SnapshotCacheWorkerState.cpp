#include "SnapshotCachedPathSpaceTestSupport.hpp"

#include <future>

namespace {

// Backing space whose visit always fails, counting the attempts.
struct FailingVisitSpace final : PathSpace {
    using PathSpace::PathSpace;

    auto visit(PathVisitor const&, VisitOptions const&) -> Expected<void> override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++attempts;
        }
        cv.notify_all();
        return std::unexpected(Error{Error::Code::UnknownError, "visit failed"});
    }

    auto waitForAttempts(int count, std::chrono::milliseconds limit) -> bool {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, limit, [&] { return attempts >= count; });
    }

    auto attemptCount() -> int {
        std::lock_guard<std::mutex> lock(mutex);
        return attempts;
    }

    std::mutex              mutex;
    std::condition_variable cv;
    int                     attempts = 0;
};

// Backing space whose visits block until released, once armed.
struct GatedVisitSpace final : PathSpace {
    using PathSpace::PathSpace;

    auto visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> override {
        if (armed.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex);
            ++entered;
            ++inside;
            maxInside = std::max(maxInside, inside);
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
            --inside;
        }
        return PathSpace::visit(visitor, options);
    }

    auto waitForEntered(int count, std::chrono::milliseconds limit) -> bool {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, limit, [&] { return entered >= count; });
    }

    void releaseAll() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }

    std::atomic<bool>       armed{false};
    std::mutex              mutex;
    std::condition_variable cv;
    int                     entered   = 0;
    int                     inside    = 0;
    int                     maxInside = 0;
    bool                    released  = false;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.snapshot_cache.worker_state");

TEST_CASE("a failing rebuild is retried with a backoff, not in a tight loop") {
    auto backing = std::make_shared<FailingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    });
    REQUIRE(backing->waitForAttempts(1, 5s));

    // Observation window: the cache stays dirty because every rebuild fails. A worker that
    // retries immediately performs thousands of full visits here; one that backs off, a handful.
    std::this_thread::sleep_for(120ms);
    auto const attempts = backing->attemptCount();
    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{.enabled = false});

    CHECK(attempts <= 10);
    CHECK(cached.snapshotMetrics().rebuilds == 0);
}

TEST_CASE("synchronous rebuilds on read back off after a failure") {
    auto backing = std::make_shared<FailingVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    cached.setSnapshotOptions(SnapshotCachedPathSpace::SnapshotOptions{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
        .allowSynchronousRebuild = true,
    });
    cached.stopSnapshotWorker();
    auto const before = backing->attemptCount();

    for (int i = 0; i < 200; ++i) {
        (void)cached.read<int>("/missing");
    }
    // Every read found the cache dirty. Each one walking the whole backing space again right
    // after the previous attempt failed turns a read into a full traversal.
    CHECK(backing->attemptCount() - before <= 10);
}

TEST_CASE("reconfiguring during a rebuild does not let a second rebuild run concurrently") {
    auto backing = std::make_shared<GatedVisitSpace>();
    SnapshotCachedPathSpace cached{backing};

    SnapshotCachedPathSpace::SnapshotOptions options{
        .enabled = true,
        .rebuildDebounce = 1h,
        .maxDirtyRoots = 8,
    };
    cached.setSnapshotOptions(options);
    cached.stopSnapshotWorker();
    backing->armed.store(true, std::memory_order_release);

    std::thread first([&] { cached.rebuildSnapshotNow(); });
    REQUIRE(backing->waitForEntered(1, 5s));

    // Same settings again while the first rebuild is inside its visit. This used to reset the
    // "rebuild in progress" flag, so the next rebuild no longer waited for the running one.
    cached.setSnapshotOptions(options);
    cached.stopSnapshotWorker();

    std::thread second([&] { cached.rebuildSnapshotNow(); });
    bool const secondEnteredEarly = backing->waitForEntered(2, 60ms);

    backing->releaseAll();
    first.join();
    second.join();

    CHECK_FALSE(secondEnteredEarly);
    std::lock_guard<std::mutex> lock(backing->mutex);
    CHECK(backing->maxInside == 1);
}

TEST_CASE("enabling the cache while its worker is being stopped does not strand the stop") {
    auto backing = std::make_shared<GatedVisitSpace>();
    SnapshotCachedPathSpace cached{backing};
    REQUIRE(backing->insert("/value", 3).errors.empty());

    SnapshotCachedPathSpace::SnapshotOptions options{
        .enabled = true,
        .rebuildDebounce = 0ms,
        .maxDirtyRoots = 8,
    };
    backing->armed.store(true, std::memory_order_release);
    cached.setSnapshotOptions(options);
    // The worker is now inside its first rebuild, parked in the backing's visit.
    REQUIRE(backing->waitForEntered(1, 5s));

    std::promise<void> stopped;
    auto               stopDone = stopped.get_future();
    std::thread        stopper([&] {
        cached.stopSnapshotWorker();
        stopped.set_value();
    });
    // Give the stopper time to request the stop and start joining. If it has not got that far
    // the test still passes; it only stops covering the interleaving.
    std::this_thread::sleep_for(30ms);

    std::promise<void> configured;
    auto               configureDone = configured.get_future();
    std::thread        configurer([&] {
        cached.setSnapshotOptions(options);
        configured.set_value();
    });
    std::this_thread::sleep_for(30ms);

    backing->armed.store(false, std::memory_order_release);
    backing->releaseAll();

    // The stop request must survive the reconfiguration: the old worker exits and is joined.
    bool const stopReturned = stopDone.wait_for(3s) == std::future_status::ready;
    if (!stopReturned) {
        cached.stopSnapshotWorker(); // unstick the worker so the threads can be joined
    }
    bool const configureReturned = configureDone.wait_for(3s) == std::future_status::ready;
    stopper.join();
    configurer.join();
    CHECK(stopReturned);
    CHECK(configureReturned);

    // The cache was enabled last, so a worker has to be running again: it rebuilds on its own.
    if (stopReturned && configureReturned) {
        auto const deadline = std::chrono::steady_clock::now() + 5s;
        while (cached.snapshotMetrics().rebuilds == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        CHECK(cached.snapshotMetrics().rebuilds >= 1);
    }
}

TEST_SUITE_END();
