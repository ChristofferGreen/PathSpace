#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/io/IoTrellis.hpp>
#include <pathspace/layer/io/PathIOGamepad.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("IoTrellis normalizes device streams") {
    SP::PathSpace space;

    auto mouse = std::make_unique<SP::PathIOMouse>(SP::PathIOMouse::BackendMode::Off);
    auto keyboard = std::make_unique<SP::PathIOKeyboard>(SP::PathIOKeyboard::BackendMode::Off);

    REQUIRE(space.insert<"/system/devices/in/pointer/default">(std::move(mouse)).errors.empty());
    REQUIRE(space.insert<"/system/devices/in/keyboard/default">(std::move(keyboard)).errors.empty());

    SP::IO::IoTrellisOptions options;
    options.event_wait_timeout = 1ms;
    options.idle_sleep = 1ms;
    options.discovery_interval = 10ms;

    auto handleExpected = SP::IO::CreateIOTrellis(space, options);
    REQUIRE(handleExpected);
    auto handle = std::move(*handleExpected);
    auto wait_for_push = [&](std::string const& path) {
        bool ready = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            auto enabled = space.read<bool, std::string>(path);
            if (enabled && *enabled) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        REQUIRE_MESSAGE(ready, "Timed out waiting for push enable at " << path);
    };
    wait_for_push("/system/devices/in/pointer/default/config/push/enabled");
    wait_for_push("/system/devices/in/keyboard/default/config/push/enabled");

    {
        SP::PathIOMouse::Event move{};
        move.deviceId = 7;
        move.type = SP::MouseEventType::Move;
        move.dx = 5;
        move.dy = -3;
        move.timestampNs = 1234;
        space.insert("/system/devices/in/pointer/default/events", move);

        auto pointer = space.take<SP::IO::PointerEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kPointerQueue},
            SP::Out{} & SP::Block{100ms});
        REQUIRE(pointer);
        CHECK(pointer->device_path == "/system/devices/in/pointer/default");
        CHECK(pointer->pointer_id == 7);
        CHECK(pointer->motion.delta_x == doctest::Approx(5.0f));
        CHECK(pointer->motion.delta_y == doctest::Approx(-3.0f));
        CHECK(pointer->type == SP::IO::PointerType::Mouse);
    }

    {
        SP::PathIOKeyboard::Event key{};
        key.deviceId = 1;
        key.type = SP::KeyEventType::KeyDown;
        key.keycode = 42;
        key.modifiers = SP::Mod_Shift | SP::Mod_Meta;
        key.timestampNs = 5678;
        space.insert("/system/devices/in/keyboard/default/events", key);

        auto button = space.take<SP::IO::ButtonEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kButtonQueue},
            SP::Out{} & SP::Block{100ms});
        REQUIRE(button);
        CHECK(button->source == SP::IO::ButtonSource::Keyboard);
        CHECK(button->button_code == 42U);
        CHECK(button->device_path == "/system/devices/in/keyboard/default");
        CHECK(button->state.pressed);
    }

    {
        SP::PathIOKeyboard::Event text{};
        text.deviceId = 1;
        text.type = SP::KeyEventType::Text;
        text.text = "A";
        text.modifiers = SP::Mod_Shift;
        text.timestampNs = 6000;
        space.insert("/system/devices/in/keyboard/default/events", text);

        auto emitted = space.take<SP::IO::TextEvent, std::string>(
            std::string{SP::IO::IoEventPaths::kTextQueue},
            SP::Out{} & SP::Block{100ms});
        REQUIRE(emitted);
        CHECK(emitted->codepoint == static_cast<char32_t>('A'));
        CHECK(emitted->device_path == "/system/devices/in/keyboard/default");
    }

    handle.shutdown();
}

TEST_CASE("IoTrellis normalizes gamepad axis events") {
    SP::PathSpace space;
    auto gamepad = std::make_unique<SP::PathIOGamepad>(SP::PathIOGamepad::BackendMode::Off);
    REQUIRE(space.insert<"/system/devices/in/gamepad/default">(std::move(gamepad)).errors.empty());

    SP::IO::IoTrellisOptions options;
    options.event_wait_timeout = 1ms;
    options.idle_sleep = 1ms;
    options.discovery_interval = 10ms;

    auto handleExpected = SP::IO::CreateIOTrellis(space, options);
    REQUIRE(handleExpected);
    auto handle = std::move(*handleExpected);

    SP::PathIOGamepad::Event axis{};
    axis.deviceId = 9;
    axis.type = SP::PathIOGamepad::EventType::AxisMove;
    axis.axis = 0;
    axis.value = 0.5f;
    axis.timestampNs = 1000;
    space.insert("/system/devices/in/gamepad/default/events", axis);

    auto pointer = space.take<SP::IO::PointerEvent, std::string>(
        std::string{SP::IO::IoEventPaths::kPointerQueue},
        SP::Out{} & SP::Block{100ms});
    REQUIRE(pointer);
    CHECK(pointer->type == SP::IO::PointerType::GamepadStick);
    CHECK(pointer->device_path == "/system/devices/in/gamepad/default");
    CHECK(pointer->pointer_id != 0);
    CHECK(pointer->motion.absolute);
    CHECK(pointer->motion.absolute_x == doctest::Approx(0.5f));

    handle.shutdown();
}
