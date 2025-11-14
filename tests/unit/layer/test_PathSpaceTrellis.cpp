#include <doctest/doctest.h>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <core/Out.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace SP;

namespace {

auto mountTrellis(std::shared_ptr<PathSpace> const& space) -> PathSpaceTrellis* {
    auto trellis = std::make_unique<PathSpaceTrellis>(space);
    auto* raw    = trellis.get();
    auto ret     = space->insert<"/cursor">(std::move(trellis));
    REQUIRE(ret.errors.empty());
    return raw;
}

} // namespace

TEST_SUITE("PathSpaceTrellis") {
    TEST_CASE("enable/disable commands update registry") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);

        auto enableMouse = space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        CHECK(enableMouse.errors.empty());

        auto enableGamepad = space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});
        CHECK(enableGamepad.errors.empty());

        auto sources = trellis->debugSources();
        std::sort(sources.begin(), sources.end());
        CHECK(sources == std::vector<std::string>{"/data/gamepad", "/data/mouse"});

        // Duplicate enable is idempotent
        auto duplicateEnable = space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        CHECK(duplicateEnable.errors.empty());
        CHECK_EQ(trellis->debugSources().size(), 2);

        // Disable removes the source
        auto disableMouse = space->insert<"/cursor/_system/disable">(std::string{"/data/mouse"});
        CHECK(disableMouse.errors.empty());
        CHECK_EQ(trellis->debugSources(), std::vector<std::string>{"/data/gamepad"});
    }

    TEST_CASE("copyable inserts fan out to every source") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        auto ret = space->insert<"/cursor">(42);
        CHECK(ret.errors.empty());

        auto mouse = space->take<int>("/data/mouse");
        REQUIRE(mouse);
        CHECK_EQ(mouse.value(), 42);

        auto gamepad = space->take<int>("/data/gamepad");
        REQUIRE(gamepad);
        CHECK_EQ(gamepad.value(), 42);
    }

    TEST_CASE("move-only inserts target first available source") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        auto ret = space->insert<"/cursor">(std::make_unique<int>(7));
        CHECK(ret.errors.empty());

        auto mouse = space->take<std::unique_ptr<int>>("/data/mouse");
        auto pad   = space->take<std::unique_ptr<int>>("/data/gamepad");

        bool mouseReceived   = mouse.has_value();
        bool gamepadReceived = pad.has_value();
        CHECK(mouseReceived != gamepadReceived); // exactly one source receives the payload
        CHECK(mouseReceived || gamepadReceived);
        if (mouseReceived) {
            CHECK_EQ(*mouse.value(), 7);
        } else if (gamepadReceived) {
            CHECK_EQ(*pad.value(), 7);
        }
    }

    TEST_CASE("read via trellis follows round robin ordering") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        space->insert<"/cursor/_system/enable">(std::string{"/data/mouse"});
        space->insert<"/cursor/_system/enable">(std::string{"/data/gamepad"});

        space->insert<"/data/mouse">(11);
        space->insert<"/data/gamepad">(22);

        auto first = space->take<int>("/cursor");
        REQUIRE(first);
        CHECK_EQ(first.value(), 11);

        space->insert<"/data/mouse">(33);
        space->insert<"/data/gamepad">(44);

        auto second = space->take<int>("/cursor");
        REQUIRE(second);
        CHECK_EQ(second.value(), 22);
    }

    TEST_CASE("bypass writes forward to backing space") {
        auto space   = std::make_shared<PathSpace>();
        auto* trellis = mountTrellis(space);
        (void)trellis;

        auto ret = space->insert<"/cursor/log/events">(5);
        CHECK(ret.errors.empty());

        auto value = space->take<int>("/cursor/log/events");
        REQUIRE(value);
        CHECK_EQ(value.value(), 5);
    }
}
