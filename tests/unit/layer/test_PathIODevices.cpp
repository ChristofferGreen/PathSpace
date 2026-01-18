#include "third_party/doctest.h"
#include <cstdint>
#include <string>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

using namespace SP;

TEST_SUITE("layer.pathio.devices") {
TEST_CASE("PathIOMouse - Simulation queue basic operations") {
    PathIOMouse mice{PathIOMouse::BackendMode::Off};

    // Enqueue a few events via insert at '/events'
    {
        PathIOMouse::Event ev{};
        ev.type = MouseEventType::Move;
        ev.dx = 5; ev.dy = -3;
        mice.insert<"/events">(ev);
    }
    {
        PathIOMouse::Event ev{};
        ev.type = MouseEventType::ButtonDown;
        ev.button = MouseButton::Left;
        mice.insert<"/events">(ev);
    }
    {
        PathIOMouse::Event ev{};
        ev.type = MouseEventType::Wheel;
        ev.wheel = +2;
        mice.insert<"/events">(ev);
    }

    // Peek should see the first event without popping
    auto e1_opt = mice.read<"/events", PathIOMouse::Event>();
    REQUIRE(e1_opt.has_value());
    auto e1 = *e1_opt;
    CHECK(e1.type == MouseEventType::Move);
    CHECK(e1.dx == 5);
    CHECK(e1.dy == -3);

    // Pop events in order
    auto p1 = mice.take<"/events", PathIOMouse::Event>();
    REQUIRE(p1.has_value());
    CHECK(p1->type == MouseEventType::Move);

    auto p2 = mice.take<"/events", PathIOMouse::Event>();
    REQUIRE(p2.has_value());
    CHECK(p2->type == MouseEventType::ButtonDown);
    CHECK(p2->button == MouseButton::Left);

    auto p3 = mice.take<"/events", PathIOMouse::Event>();
    REQUIRE(p3.has_value());
    CHECK(p3->type == MouseEventType::Wheel);
    CHECK(p3->wheel == 2);
}

TEST_CASE("PathIOKeyboard - Simulation queue basic operations") {
    PathIOKeyboard kb{PathIOKeyboard::BackendMode::Off};

    // Enqueue a few events via insert at '/events'
    {
        PathIOKeyboard::Event ev{};
        ev.type = KeyEventType::KeyDown;
        ev.keycode = 65;
        ev.modifiers = Mod_Shift;
        kb.insert<"/events">(ev);
    }
    {
        PathIOKeyboard::Event ev{};
        ev.type = KeyEventType::Text;
        ev.text = "A";
        ev.modifiers = Mod_Shift;
        kb.insert<"/events">(ev);
    }
    {
        PathIOKeyboard::Event ev{};
        ev.type = KeyEventType::KeyUp;
        ev.keycode = 65;
        ev.modifiers = Mod_Shift;
        kb.insert<"/events">(ev);
    }

    // Peek should see the first event without popping
    auto e1_opt = kb.read<"/events", PathIOKeyboard::Event>();
    REQUIRE(e1_opt.has_value());
    auto e1 = *e1_opt;
    CHECK(e1.type == KeyEventType::KeyDown);
    CHECK(e1.keycode == 65);
    CHECK((e1.modifiers & Mod_Shift) != 0u);

    // Pop events in order
    auto p1 = kb.take<"/events", PathIOKeyboard::Event>();
    REQUIRE(p1.has_value());
    CHECK(p1->type == KeyEventType::KeyDown);

    auto p2 = kb.take<"/events", PathIOKeyboard::Event>();
    REQUIRE(p2.has_value());
    CHECK(p2->type == KeyEventType::Text);
    CHECK(p2->text == "A");

    auto p3 = kb.take<"/events", PathIOKeyboard::Event>();
    REQUIRE(p3.has_value());
    CHECK(p3->type == KeyEventType::KeyUp);
}

TEST_CASE("PathIOMouse - Mounting under PathSpace") {
    // Prepare device and keep a raw pointer to validate it remains alive after insertion.
    auto dev = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Off);
    PathIOMouse* raw = dev.get();

    PathSpace space;
    auto ret = space.insert<"/devices/mouse">(std::move(dev));
    CHECK(ret.nbrSpacesInserted == 1);

    // We can still interact with the mounted device through the retained raw pointer.
    {
        PathIOMouse::Event ev{};
        ev.type = MouseEventType::Move;
        ev.dx = 1; ev.dy = 2;
        raw->insert<"/events">(ev);
    }

    // The nested provider currently doesn't implement out() for std::string; reads should fail gracefully.
    auto r = space.read<"/devices/mouse/events", std::string>();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("PathIOKeyboard - Mounting under PathSpace") {
    auto dev = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Off);
    PathIOKeyboard* raw = dev.get();

    PathSpace space;
    auto ret = space.insert<"/devices/keyboard">(std::move(dev));
    CHECK(ret.nbrSpacesInserted == 1);

    {
        PathIOKeyboard::Event ev{};
        ev.type = KeyEventType::KeyDown;
        ev.keycode = 65;
        ev.modifiers = Mod_Shift;
        raw->insert<"/events">(ev);
    }

    auto r = space.read<"/devices/keyboard/events", std::string>();
    CHECK_FALSE(r.has_value());
}

TEST_CASE("PathIOMouse - typed out()/take() semantics") {
    PathIOMouse mice{PathIOMouse::BackendMode::Off};

    SUBCASE("Non-blocking read on empty returns error") {
        auto r = mice.read<"/events", PathIOMouse::Event>();
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Blocking read times out on empty") {
        auto r = mice.read<"/events", PathIOMouse::Event>(Block{std::chrono::milliseconds(10)});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("Peek then Pop preserves and consumes in order") {
        // Enqueue one event
        {
            PathIOMouse::Event ev{};
            ev.type = MouseEventType::Move;
            ev.dx = 3; ev.dy = 4;
            mice.insert<"/events">(ev);
        }

        // Peek (non-pop) should return the event without consuming it
        auto peek = mice.read<"/events", PathIOMouse::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == MouseEventType::Move);
        CHECK(peek->dx == 3);
        CHECK(peek->dy == 4);

        // Pop should consume it
        auto popped = mice.take<"/events", PathIOMouse::Event>();
        REQUIRE(popped.has_value());
        CHECK(popped->type == MouseEventType::Move);
    }
}

TEST_CASE("PathIOKeyboard - typed out()/take() semantics") {
    PathIOKeyboard kb{PathIOKeyboard::BackendMode::Off};

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
        {
            PathIOKeyboard::Event ev{};
            ev.type = KeyEventType::KeyDown;
            ev.keycode = 65;
            ev.modifiers = Mod_Shift;
            kb.insert<"/events">(ev);
        }

        // Peek (non-pop)
        auto peek = kb.read<"/events", PathIOKeyboard::Event>();
        REQUIRE(peek.has_value());
        CHECK(peek->type == KeyEventType::KeyDown);
        CHECK(peek->keycode == 65);
        CHECK((peek->modifiers & Mod_Shift) != 0u);

        // Pop
        auto popped = kb.take<"/events", PathIOKeyboard::Event>();
        REQUIRE(popped.has_value());
        CHECK(popped->type == KeyEventType::KeyDown);
    }
}

TEST_CASE("PathIO devices expose push config nodes") {
    PathIOMouse device{PathIOMouse::BackendMode::Off};

    bool enabled = true;
    auto retEnabled = device.insert("/config/push/enabled", enabled);
    CHECK(retEnabled.nbrValuesInserted == 1);
    auto readEnabled = device.read<bool>("/config/push/enabled");
    REQUIRE(readEnabled.has_value());
    CHECK(*readEnabled);

    std::uint32_t rate = 480;
    auto retRate = device.insert("/config/push/rate_limit_hz", rate);
    CHECK(retRate.nbrValuesInserted == 1);
    auto readRate = device.read<std::uint32_t>("/config/push/rate_limit_hz");
    REQUIRE(readRate.has_value());
    CHECK(*readRate == rate);

    bool telemetry = true;
    auto retTelemetry = device.insert("/config/push/telemetry_enabled", telemetry);
    CHECK(retTelemetry.nbrValuesInserted == 1);
    auto readTelemetry = device.read<bool>("/config/push/telemetry_enabled");
    REQUIRE(readTelemetry.has_value());
    CHECK(*readTelemetry);

    bool subscriber = true;
    auto retSub = device.insert("/config/push/subscribers/test_subscriber", subscriber);
    CHECK(retSub.nbrValuesInserted == 1);
    auto readSub = device.read<bool>("/config/push/subscribers/test_subscriber");
    REQUIRE(readSub.has_value());
    CHECK(*readSub);

    PathSpace space;
    auto nested = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Off);
    auto mountRet = space.insert<"/system/devices/in/pointer/default">(std::move(nested));
    CHECK(mountRet.nbrSpacesInserted == 1);
    auto spaceSet = space.insert("/system/devices/in/pointer/default/config/push/enabled", true);
    CHECK(spaceSet.nbrValuesInserted == 1);
    auto spaceRead = space.read<bool>("/system/devices/in/pointer/default/config/push/enabled");
    REQUIRE(spaceRead.has_value());
    CHECK(*spaceRead);
}
}
