#include "third_party/doctest.h"
#include <atomic>
#include <thread>
#include <vector>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/io/PathIOPointerMixer.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("layer.pathio.pointermixer") {
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
        // Produce a mix of events from two sources via insert
        {
            PathIOPointerMixer::Event ev{};
            ev.type = PathIOPointerMixer::PointerEventType::Move;
            ev.dx = 1; ev.dy = 0; ev.sourceId = 0;
            mixer.insert<"/events">(ev);
        }
        {
            PathIOPointerMixer::Event ev{};
            ev.type = PathIOPointerMixer::PointerEventType::Move;
            ev.dx = 0; ev.dy = 1; ev.sourceId = 1;
            mixer.insert<"/events">(ev);
        }
        {
            PathIOPointerMixer::Event ev{};
            ev.type = PathIOPointerMixer::PointerEventType::Wheel;
            ev.wheel = 2; ev.sourceId = 0;
            mixer.insert<"/events">(ev);
        }
        {
            PathIOPointerMixer::Event ev{};
            ev.type = PathIOPointerMixer::PointerEventType::ButtonDown;
            ev.button = PathIOPointerMixer::PointerButton::Left; ev.sourceId = 1;
            mixer.insert<"/events">(ev);
        }

        // Peek the first event - should be from source 0 (dx=1,dy=0)
        auto peek = mixer.read<"/events", PathIOPointerMixer::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == PathIOPointerMixer::PointerEventType::Move);
        CHECK(peek->sourceId == 0);
        CHECK(peek->dx == 1);
        CHECK(peek->dy == 0);

        // Pop should now consume in the same order
        auto e1 = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(e1.has_value());
        CHECK(e1->sourceId == 0);
        CHECK(e1->type == PathIOPointerMixer::PointerEventType::Move);

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
    }

    SUBCASE("Peek then Pop preserves and consumes single event") {
        {
            PathIOPointerMixer::Event ev{};
            ev.type = PathIOPointerMixer::PointerEventType::AbsoluteMove;
            ev.x = 10; ev.y = 20; ev.sourceId = 2;
            mixer.insert<"/events">(ev);
        }

        auto peek = mixer.read<"/events", PathIOPointerMixer::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == PathIOPointerMixer::PointerEventType::AbsoluteMove);
        CHECK(peek->x == 10);
        CHECK(peek->y == 20);
        CHECK(peek->sourceId == 2);

        auto pop = mixer.take<"/events", PathIOPointerMixer::Event>();
        REQUIRE(pop.has_value());
        CHECK(pop->type == PathIOPointerMixer::PointerEventType::AbsoluteMove);
    }
}

TEST_CASE("PathIOPointerMixer - Blocking wake via provider's condition variable") {
    PathIOPointerMixer mixer;

    // Start a reader thread that blocks waiting for an event; then produce after a short delay
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
    {
        PathIOPointerMixer::Event evw{};
        evw.type = PathIOPointerMixer::PointerEventType::Move;
        evw.dx = 3; evw.dy = 4; evw.sourceId = 7;
        mixer.insert<"/events">(evw);
    }

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
    auto* raw  = mixer.get(); // keep a raw pointer for producing events
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

    // Produce an event; the provider will wake via its condition variable and PathSpace context if present
    {
        PathIOPointerMixer::Event evw{};
        evw.type = PathIOPointerMixer::PointerEventType::ButtonDown;
        evw.button = PathIOPointerMixer::PointerButton::Left;
        evw.sourceId = 1;
        raw->insert<"/events">(evw);
    }

    reader.join();

    CHECK(got.load(std::memory_order_acquire) == true);
    CHECK(ev.type == PathIOPointerMixer::PointerEventType::ButtonDown);
    CHECK(ev.sourceId == 1);
}
}
