#include "ext/doctest.h"


#include <string>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIOGamepad.hpp>

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("PathIOGamepad") {
    TEST_CASE("Simulation queue basic operations") {
        PathIOGamepad pad{PathIOGamepad::BackendMode::Simulation};

        CHECK(pad.pending() == 0);

        // Enqueue a few events
        pad.simulateConnected();
        pad.simulateButtonDown(/*button=*/0);
        pad.simulateAxisMove(/*axis=*/1, /*value=*/0.5f);

        CHECK(pad.pending() == 3);

        // Peek should see the first event without popping
        auto e1_opt = pad.peek();
        REQUIRE(e1_opt.has_value());
        auto e1 = *e1_opt;
        CHECK(e1.type == PathIOGamepad::EventType::Connected);

        // Pop events in order
        auto p1 = pad.pop();
        REQUIRE(p1.has_value());
        CHECK(p1->type == PathIOGamepad::EventType::Connected);

        auto p2 = pad.pop();
        REQUIRE(p2.has_value());
        CHECK(p2->type == PathIOGamepad::EventType::ButtonDown);
        CHECK(p2->button == 0);

        auto p3 = pad.pop();
        REQUIRE(p3.has_value());
        CHECK(p3->type == PathIOGamepad::EventType::AxisMove);
        CHECK(p3->axis == 1);
        CHECK(p3->value == doctest::Approx(0.5f));

        CHECK(pad.pending() == 0);
    }

    TEST_CASE("Typed out()/take() semantics") {
        PathIOGamepad pad{PathIOGamepad::BackendMode::Simulation};

        SUBCASE("Non-blocking read on empty returns error") {
            auto r = pad.read<"/events", PathIOGamepad::Event>();
            CHECK_FALSE(r.has_value());
            CHECK(r.error().code == Error::Code::NoObjectFound);
        }

        SUBCASE("Blocking read times out on empty") {
            auto r = pad.read<"/events", PathIOGamepad::Event>(Block{10ms});
            CHECK_FALSE(r.has_value());
            CHECK(r.error().code == Error::Code::Timeout);
        }

        SUBCASE("Peek then Pop preserves and consumes in order") {
            // Enqueue one event
            pad.simulateButtonDown(/*button=*/1);

            // Peek (non-pop) should return the event without consuming it
            auto peek = pad.read<"/events", PathIOGamepad::Event>();
            REQUIRE(peek.has_value());
            CHECK(peek->type == PathIOGamepad::EventType::ButtonDown);
            CHECK(peek->button == 1);
            CHECK(pad.pending() == 1);

            // Pop should consume it
            auto popped = pad.take<"/events", PathIOGamepad::Event>();
            REQUIRE(popped.has_value());
            CHECK(popped->type == PathIOGamepad::EventType::ButtonDown);
            CHECK(pad.pending() == 0);
        }
    }

    TEST_CASE("Haptics in() accepts and rejects appropriately") {
        PathIOGamepad sim{PathIOGamepad::BackendMode::Simulation};

        SUBCASE("Accept HapticsCommand at /rumble") {
            PathIOGamepad::HapticsCommand cmd = PathIOGamepad::HapticsCommand::constant(0.8f, 0.4f, 250);
            auto ret = sim.insert<"/rumble">(cmd);
            CHECK(ret.nbrValuesInserted == 1);
            CHECK(ret.errors.empty());
        }

        SUBCASE("Accept HapticsCommand at /haptics") {
            PathIOGamepad::HapticsCommand cmd{1.0f, 1.0f, 100};
            auto ret = sim.insert<"/haptics">(cmd);
            CHECK(ret.nbrValuesInserted == 1);
            CHECK(ret.errors.empty());
        }

        SUBCASE("Reject wrong type") {
            std::string wrongType = "not-a-haptics-command";
            auto ret = sim.insert<"/rumble">(wrongType);
            CHECK(ret.nbrValuesInserted == 0);
            REQUIRE_FALSE(ret.errors.empty());
            CHECK(ret.errors.front().code == Error::Code::InvalidType);
        }

        SUBCASE("Reject unsupported control path") {
            PathIOGamepad::HapticsCommand cmd{0.2f, 0.1f, 50};
            auto ret = sim.insert<"/control/unknown">(cmd);
            CHECK(ret.nbrValuesInserted == 0);
            REQUIRE_FALSE(ret.errors.empty());
            CHECK(ret.errors.front().code == Error::Code::InvalidPath);
        }

        SUBCASE("OS backend currently unsupported for haptics") {
            PathIOGamepad os{PathIOGamepad::BackendMode::OS};
            PathIOGamepad::HapticsCommand cmd{0.3f, 0.3f, 100};
            auto ret = os.insert<"/rumble">(cmd);
            CHECK(ret.nbrValuesInserted == 0);
            REQUIRE_FALSE(ret.errors.empty());
            CHECK(ret.errors.front().code == Error::Code::InvalidPermissions);
        }
    }

    TEST_CASE("Mounting under PathSpace and typed take") {
        using GamepadEvent = PathIOGamepad::Event;

        PathSpace space;

        // Mount a simulation gamepad at canonical input path
        auto dev = std::make_unique<PathIOGamepad>(PathIOGamepad::BackendMode::Simulation);
        PathIOGamepad* raw = dev.get();
        auto ir = space.insert<"/system/devices/in/gamepad/default">(std::move(dev));
        CHECK(ir.nbrSpacesInserted == 1);

        // Simulate an event on the raw provider
        raw->simulateAxisMove(/*axis=*/2, /*value=*/-0.25f);

        // Take the event via the space from the canonical events path
        auto evt = space.take<"/system/devices/in/gamepad/default/events", GamepadEvent>(Block{50ms});
        REQUIRE(evt.has_value());
        CHECK(evt->type == PathIOGamepad::EventType::AxisMove);
        CHECK(evt->axis == 2);
        CHECK(evt->value == doctest::Approx(-0.25f));

        // Issue a haptics command through the mounted provider by writing to the mount + "/rumble"
        PathIOGamepad::HapticsCommand cmd{0.6f, 0.2f, 120};
        auto hr = space.insert<"/system/devices/in/gamepad/default/rumble">(cmd);
        CHECK(hr.errors.empty());
        CHECK(hr.nbrValuesInserted == 1);
    }
}