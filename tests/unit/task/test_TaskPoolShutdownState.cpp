#define private public
#include "task/TaskPool.hpp"
#undef private
#include "task/Task.hpp"
#include "third_party/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("task.pool.shutdown.statemachine") {

TEST_CASE("two concurrent shutdown calls both return after the workers were joined once") {
    TaskPool pool(2);

    std::promise<void>       started;
    std::promise<void>       release;
    std::shared_future<void> released = release.get_future().share();
    auto task = Task::Create([&](Task&, bool) {
        started.set_value();
        released.wait();
    });
    REQUIRE_FALSE(pool.addTask(task).has_value());
    REQUIRE(started.get_future().wait_for(5s) == std::future_status::ready);

    // Shared state outlives the test body in case a shutdown call never returns.
    struct Outcome {
        std::promise<void> done;
        std::atomic<bool>  threw{false};
    };
    auto first  = std::make_shared<Outcome>();
    auto second = std::make_shared<Outcome>();
    auto runShutdown = [&pool](std::shared_ptr<Outcome> outcome) {
        try {
            pool.shutdown();
        } catch (...) {
            outcome->threw.store(true);
        }
        outcome->done.set_value();
    };

    auto firstDone  = first->done.get_future();
    auto secondDone = second->done.get_future();
    std::thread firstCaller(runShutdown, first);
    // The first caller has announced the shutdown and is (about to be) joining the busy worker.
    auto const announceDeadline = std::chrono::steady_clock::now() + 5s;
    while (!pool.shuttingDown.load() && std::chrono::steady_clock::now() < announceDeadline) {
        std::this_thread::yield();
    }
    REQUIRE(pool.shuttingDown.load());
    std::thread secondCaller(runShutdown, second);
    // Let the second caller reach the pool; if it has not, the test passes without covering the overlap.
    std::this_thread::sleep_for(50ms);

    release.set_value();

    bool const firstReturned  = firstDone.wait_for(5s) == std::future_status::ready;
    bool const secondReturned = secondDone.wait_for(5s) == std::future_status::ready;
    if (firstReturned) firstCaller.join(); else firstCaller.detach();
    if (secondReturned) secondCaller.join(); else secondCaller.detach();

    CHECK(firstReturned);
    CHECK(secondReturned);
    CHECK_FALSE(first->threw.load());
    CHECK_FALSE(second->threw.load());
    if (firstReturned && secondReturned) {
        CHECK(pool.workers.empty());
        CHECK(pool.size() == 0);
    }
}

}
