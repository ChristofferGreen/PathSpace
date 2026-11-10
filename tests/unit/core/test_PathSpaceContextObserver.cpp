#include "core/PathSpaceContext.hpp"
#include "third_party/doctest.h"

#include <algorithm>
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

// Observer that parks inside its first delivery until released, so a second thread can notify
// while the first delivery is still in progress.
struct ParkingObserver final : NotificationSink {
    void notify(std::string const& notificationPath) override {
        bool park = false;
        {
            std::lock_guard<std::mutex> guard(this->mutex);
            this->seen.push_back(notificationPath);
            park = this->seen.size() == 1;
        }
        if (park) {
            this->parked.set_value();
            this->released.wait();
        }
    }

    auto paths() -> std::vector<std::string> {
        std::lock_guard<std::mutex> guard(this->mutex);
        return this->seen;
    }

    std::mutex               mutex;
    std::vector<std::string> seen;
    std::promise<void>       parked;
    std::promise<void>       release;
    std::shared_future<void> released{release.get_future().share()};
};

} // namespace

TEST_SUITE("core.pathspace_context.observer") {

TEST_CASE("the observer receives a notification sent while another delivery is in progress") {
    PathSpaceContext ctx;
    auto             observer = std::make_shared<ParkingObserver>();
    ctx.setSink(observer);

    std::thread first([&] { ctx.notify("/first"); });
    REQUIRE(observer->parked.get_future().wait_for(5s) == std::future_status::ready);

    // A different thread notifies while the first delivery is parked inside the observer. This is
    // not re-entrancy: the notification is unrelated and must be delivered.
    std::thread second([&] { ctx.notify("/second"); });
    second.join();
    auto duringFirst = observer->paths();

    observer->release.set_value();
    first.join();

    REQUIRE(duringFirst.size() == 2);
    CHECK(duringFirst[0] == "/first");
    CHECK(duringFirst[1] == "/second");
}

TEST_CASE("re-entrant notifications from inside the observer are still dropped per thread") {
    PathSpaceContext ctxA;
    PathSpaceContext ctxB;

    struct Relay final : NotificationSink {
        Relay(PathSpaceContext& own, PathSpaceContext& other) : ownContext(own), otherContext(other) {}
        void notify(std::string const& notificationPath) override {
            this->seen.push_back(notificationPath);
            if (this->seen.size() > 8) {
                return; // safety net: never recurse without bound
            }
            this->ownContext.notify(notificationPath + "/self");   // re-entrant: dropped
            this->otherContext.notify(notificationPath + "/peer"); // other context: delivered
        }
        PathSpaceContext&        ownContext;
        PathSpaceContext&        otherContext;
        std::vector<std::string> seen;
    };

    auto relayA = std::make_shared<Relay>(ctxA, ctxB);
    auto relayB = std::make_shared<Relay>(ctxB, ctxA);
    ctxA.setSink(relayA);
    ctxB.setSink(relayB);

    ctxA.notify("/start");

    // A sees the original; B sees A's forward. B's forward back to A arrives while A is still
    // delivering on this thread and is dropped, as are both self-notifications.
    REQUIRE(relayA->seen.size() == 1);
    CHECK(relayA->seen[0] == "/start");
    REQUIRE(relayB->seen.size() == 1);
    CHECK(relayB->seen[0] == "/start/peer");

    // The guard is released afterwards: the next notification is delivered again.
    ctxA.notify("/again");
    CHECK(relayA->seen.size() == 2);
}

}
