#include "ext/doctest.h"
#include <atomic>
#include <thread>
#include <vector>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIOPointerMixer.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_CASE("PathIOPointerMixer - Basic aggregation order and peek/pop") {
    PathIOPointerMixer mixer;

    // Non-blocking read on empty should return error
    SUBCASE("Non-blocking read on empty returns error") {
        auto r = mixer.read<"/events", PathIOPointerMixer::Event>();
        CHECK_FALSE(r.has_value());
    }

    // Blocking read on empty should time out
    SUBCASE("Blocking read on empty times out") {
        auto r = mixer.read<"/events", PathIOPointerMixer::Event>(Block{10ms});
        CHECK_FALSE(r.has_value());
    }

    // Enqueue multiple events and verify order
    SUBCASE("Aggregation preserves arrival order across sources") {
        CHECK(mixer.pending() == 0);

        // Simulate a mix of events from two sources
        mixer.simulateMove(1, 0, /*sourceId=*/0);
        mixer.simulateMove(0, 1, /*sourceId=*/1);
        mixer.simulateWheel(+2, 0);
        mixer.simulateButtonDown(PathIOPointerMixer::PointerButton::Left, 1);

        CHECK(mixer.pending() == 4);

        // Peek the first event - should be from source 0 (dx=1,dy=0)
        auto peek = mixer.read<"/events", PathIOPointerMixer::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == PathIOPointerMixer::PointerEventType::Move);
        CHECK(peek->sourceId == 0);
        CHECK(peek->dx == 1);
        CHECK(peek->dy == 0);
        CHECK(mixer.pending() == 4); // peek didn't consume

        // Pop should now consume in the same order
        auto e1 = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(e1.has_value());
        CHECK(e1->sourceId == 0);
        CHECK(e1->type == PathIOPointerMixer::PointerEventType::Move);
        CHECK(mixer.pending() == 3);

        auto e2 = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(e2.has_value());
        CHECK(e2->sourceId == 1);
        CHECK(e2->type == PathIOPointerMixer::PointerEventType::Move);

        auto e3 = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(e3.has_value());
        CHECK(e3->type == PathIOPointerMixer::PointerEventType::Wheel);
        CHECK(e3->wheel == 2);

        auto e4 = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(e4.has_value());
        CHECK(e4->type == PathIOPointerMixer::PointerEventType::ButtonDown);
        CHECK(e4->sourceId == 1);

        CHECK(mixer.pending() == 0);
    }

    SUBCASE("Peek then Pop preserves and consumes single event") {
        mixer.simulateAbsolute(10, 20, /*sourceId=*/2);

        auto peek = mixer.read<"/events", PathIOPointerMixer::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == PathIOPointerMixer::PointerEventType::AbsoluteMove);
        CHECK(peek->x == 10);
        CHECK(peek->y == 20);
        CHECK(peek->sourceId == 2);
        CHECK(mixer.pending() == 1);

        auto pop = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(pop.has_value());
        CHECK(pop->type == PathIOPointerMixer::PointerEventType::AbsoluteMove);
        CHECK(mixer.pending() == 0);
    }
}

TEST_CASE("PathIOPointerMixer - Blocking wake via provider's condition variable") {
    PathIOPointerMixer mixer;

    // Start a reader thread that blocks waiting for an event; then simulate after a short delay
    std::atomic<bool> got{false};
    PathIOPointerMixer::Event ev{};
    std::thread t([&] {
        auto r = mixer.read<"/events", PathIOPointerMixer::Event>(Block{250ms});
        if (r.has_value()) {
            ev = *r;
            got.store(true, std::memory_order_release);
        }
    });

    // Give the reader a moment to enter the blocking wait
    std::this_thread::sleep_for(20ms);
    mixer.simulateMove(3, 4, /*sourceId=*/7);

    t.join();

    CHECK(got.load(std::memory_order_acquire) == true);
    CHECK(ev.type == PathIOPointerMixer::PointerEventType::Move);
    CHECK(ev.dx == 3);
    CHECK(ev.dy == 4);
    CHECK(ev.sourceId == 7);
}

TEST_CASE("PathIOPointerMixer - Mounted under PathSpace (notifyAll wake)") {
    // Mount mixer in a parent space and ensure a PathSpace::read(Block) is woken when an event arrives.
    PathSpace space;

    auto mixer = std::make_unique<PathIOPointerMixer>();
    auto* raw  = mixer.get(); // keep a raw pointer for simulateEvent
    auto ret   = space.insert<"/pointer">(std::move(mixer));
    REQUIRE(ret.errors.empty());
    REQUIRE(ret.nbrSpacesInserted == 1);

    std::atomic<bool> got{false};
    PathIOPointerMixer::Event ev{};

    std::thread reader([&] {
        // Blocked read via the parent at a nested path; PathSpace will wait/notify using its context.
        auto r = space.read<"/pointer/events", PathIOPointerMixer::Event>(Block{500ms});
        if (r.has_value()) {
            ev = *r;
            got.store(true, std::memory_order_release);
        }
    });

    // Allow time for the reader to register its wait
    std::this_thread::sleep_for(50ms);

    // Simulate an event; the provider will notifyAll() on its context if present
    raw->simulateButtonDown(PathIOPointerMixer::PointerButton::Left, /*sourceId=*/1);

    reader.join();

    CHECK(got.load(std::memory_order_acquire) == true);
    CHECK(ev.type == PathIOPointerMixer::PointerEventType::ButtonDown);
    CHECK(ev.sourceId == 1);
}
