#include "ext/doctest.h"
#include <string>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIOMice.hpp>
#include <pathspace/layer/PathIOKeyboard.hpp>

using namespace SP;

TEST_CASE("PathIOMice - Simulation queue basic operations") {
    PathIOMice mice;

    CHECK(mice.pending() == 0);

    // Enqueue a few events
    mice.simulateMove(5, -3, /*deviceId=*/0);
    mice.simulateButtonDown(MouseButton::Left, /*deviceId=*/0);
    mice.simulateWheel(+2, /*deviceId=*/0);

    CHECK(mice.pending() == 3);

    // Peek should see the first event without popping
    auto e1_opt = mice.peek();
    REQUIRE(e1_opt.has_value());
    auto e1 = *e1_opt;
    CHECK(e1.type == MouseEventType::Move);
    CHECK(e1.dx == 5);
    CHECK(e1.dy == -3);

    // Pop events in order
    auto p1 = mice.pop();
    REQUIRE(p1.has_value());
    CHECK(p1->type == MouseEventType::Move);

    auto p2 = mice.pop();
    REQUIRE(p2.has_value());
    CHECK(p2->type == MouseEventType::ButtonDown);
    CHECK(p2->button == MouseButton::Left);

    auto p3 = mice.pop();
    REQUIRE(p3.has_value());
    CHECK(p3->type == MouseEventType::Wheel);
    CHECK(p3->wheel == 2);

    CHECK(mice.pending() == 0);
}

TEST_CASE("PathIOKeyboard - Simulation queue basic operations") {
    PathIOKeyboard kb;

    CHECK(kb.pending() == 0);

    // Enqueue a few events
    kb.simulateKeyDown(/*keycode=*/65, /*modifiers=*/Mod_Shift, /*deviceId=*/0);
    kb.simulateText("A", /*modifiers=*/Mod_Shift, /*deviceId=*/0);
    kb.simulateKeyUp(/*keycode=*/65, /*modifiers=*/Mod_Shift, /*deviceId=*/0);

    CHECK(kb.pending() == 3);

    // Peek should see the first event without popping
    auto e1_opt = kb.peek();
    REQUIRE(e1_opt.has_value());
    auto e1 = *e1_opt;
    CHECK(e1.type == KeyEventType::KeyDown);
    CHECK(e1.keycode == 65);
    CHECK((e1.modifiers & Mod_Shift) != 0u);

    // Pop events in order
    auto p1 = kb.pop();
    REQUIRE(p1.has_value());
    CHECK(p1->type == KeyEventType::KeyDown);

    auto p2 = kb.pop();
    REQUIRE(p2.has_value());
    CHECK(p2->type == KeyEventType::Text);
    CHECK(p2->text == "A");

    auto p3 = kb.pop();
    REQUIRE(p3.has_value());
    CHECK(p3->type == KeyEventType::KeyUp);

    CHECK(kb.pending() == 0);
}

TEST_CASE("PathIOMice - Mounting under PathSpace") {
    // Prepare device and keep a raw pointer to validate it remains alive after insertion.
    auto dev = std::make_unique<PathIOMice>();
    PathIOMice* raw = dev.get();

    PathSpace space;
    auto ret = space.insert<"/devices/mice">(std::move(dev));
    CHECK(ret.nbrSpacesInserted == 1);

    // We can still interact with the mounted device through the retained raw pointer.
    raw->simulateMove(1, 2);
    CHECK(raw->pending() == 1);

    // The nested provider currently doesn't implement out(); reads should fail gracefully.
    auto r = space.read<"/devices/mice/events", std::string>();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("PathIOKeyboard - Mounting under PathSpace") {
    auto dev = std::make_unique<PathIOKeyboard>();
    PathIOKeyboard* raw = dev.get();

    PathSpace space;
    auto ret = space.insert<"/devices/keyboard">(std::move(dev));
    CHECK(ret.nbrSpacesInserted == 1);

    raw->simulateKeyDown(65, Mod_Shift);
    CHECK(raw->pending() == 1);

    auto r = space.read<"/devices/keyboard/events", std::string>();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("PathIOMice - typed out()/take() semantics") {
    PathIOMice mice;

    SUBCASE("Non-blocking read on empty returns error") {
        auto r = mice.read<"/events", PathIOMice::Event>();
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Blocking read times out on empty") {
        auto r = mice.read<"/events", PathIOMice::Event>(Block{std::chrono::milliseconds(10)});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Peek then Pop preserves and consumes in order") {
        // Enqueue one event
        mice.simulateMove(3, 4);

        // Peek (non-pop) should return the event without consuming it
        auto peek = mice.read<"/events", PathIOMice::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == MouseEventType::Move);
        CHECK(peek->dx == 3);
        CHECK(peek->dy == 4);
        CHECK(mice.pending() == 1);

        // Pop should consume it
        auto popped = mice.take<"/events", PathIOMice::Event>();
        REQUIRE(popped.has_value());
        CHECK(popped->type == MouseEventType::Move);
        CHECK(mice.pending() == 0);
    }
}

TEST_CASE("PathIOKeyboard - typed out()/take() semantics") {
    PathIOKeyboard kb;

    SUBCASE("Non-blocking read on empty returns error") {
        auto r = kb.read<"/events", PathIOKeyboard::Event>();
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Blocking read times out on empty") {
        auto r = kb.read<"/events", PathIOKeyboard::Event>(Block{std::chrono::milliseconds(10)});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Peek then Pop preserves and consumes in order") {
        // Enqueue a key down event
        kb.simulateKeyDown(/*keycode=*/65, /*modifiers=*/Mod_Shift);

        // Peek (non-pop)
        auto peek = kb.read<"/events", PathIOKeyboard::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == KeyEventType::KeyDown);
        CHECK(peek->keycode == 65);
        CHECK((peek->modifiers & Mod_Shift) != 0u);
        CHECK(kb.pending() == 1);

        // Pop
        auto popped = kb.take<"/events", PathIOKeyboard::Event>();
        REQUIRE(popped.has_value());
        CHECK(popped->type == KeyEventType::KeyDown);
        CHECK(kb.pending() == 0);
    }
}