#include "ext/doctest.h"
#include <atomic>
#include <thread>
#include <vector>
#include <optional>
#include <chrono>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/layer/PathIOMouse.hpp>
#include <pathspace/layer/PathIOPointerMixer.hpp>

using namespace SP;
using namespace std::chrono_literals;

// Helper: poll a non-blocking read until value is available or timeout elapses.
template <typename T, typename Space>
static std::optional<T> poll_read(Space& space, std::string const& path, std::chrono::milliseconds timeout, std::chrono::milliseconds step = 5ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        auto r = space.template read<T>(path, Out{} & OutNoValidation{});
        if (r.has_value()) {
            return r.value();
        }
        std::this_thread::sleep_for(step);
    }
    return std::nullopt;
}

template <typename T, typename Space>
static std::optional<T> poll_take(Space& space, std::string const& path, std::chrono::milliseconds timeout, std::chrono::milliseconds step = 5ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        auto r = space.template take<T>(path, Out{} & OutNoValidation{});
        if (r.has_value()) {
            return r.value();
        }
        std::this_thread::sleep_for(step);
    }
    return std::nullopt;
}

// Demonstrates user-level wiring of: mouse -> pointer mixer -> alias to "default pointer".
// Providers remain path-agnostic; the user composes them and chooses paths.
TEST_CASE("Composition: mouse -> mixer -> alias (user-level wiring, providers are path-agnostic)") {
    // User-owned PathSpace (can be heap or stack; use heap to show shared ownership for alias upstream)
    auto root = std::make_shared<PathSpace>();

    // Create providers (path-agnostic)
    auto mouseUptr  = std::make_unique<PathIOMouse>();
    auto* mouseRaw  = mouseUptr.get();
    auto mixerUptr  = std::make_unique<PathIOPointerMixer>();
    auto* mixerRaw  = mixerUptr.get();

    // Mount providers at user-chosen paths (providers don't care about these)
    {
        auto ret1 = root->insert<"/inputs/mouse/0">(std::move(mouseUptr));
        REQUIRE(ret1.errors.empty());
        REQUIRE(ret1.nbrSpacesInserted == 1);

        auto ret2 = root->insert<"/aggregate/pointer">(std::move(mixerUptr));
        REQUIRE(ret2.errors.empty());
        REQUIRE(ret2.nbrSpacesInserted == 1);
    }

    // Create a forwarding thread that reads typed mouse events from the mouse provider
    // and forwards them to the mixer. This demonstrates "glue" code without hard-coded paths.
    std::atomic<bool> forwarderRunning{true};
    std::thread forwarder([&] {
        while (forwarderRunning.load(std::memory_order_acquire)) {
            // Block briefly waiting for a mouse event; on success, translate to pointer mixer event.
            auto r = mouseRaw->read<"/events", PathIOMouse::Event>(Block{50ms});
            if (r.has_value()) {
                auto const& mev = *r;
                PathIOPointerMixer::Event pev;
                pev.sourceId    = 0;
                // Map fields
                switch (mev.type) {
                    case MouseEventType::Move:
                        pev.type = PathIOPointerMixer::PointerEventType::Move;
                        pev.dx   = mev.dx;
                        pev.dy   = mev.dy;
                        break;
                    case MouseEventType::AbsoluteMove:
                        pev.type = PathIOPointerMixer::PointerEventType::AbsoluteMove;
                        pev.x    = mev.x;
                        pev.y    = mev.y;
                        break;
                    case MouseEventType::ButtonDown:
                        pev.type   = PathIOPointerMixer::PointerEventType::ButtonDown;
                        pev.button = static_cast<PathIOPointerMixer::PointerButton>(static_cast<int>(mev.button));
                        break;
                    case MouseEventType::ButtonUp:
                        pev.type   = PathIOPointerMixer::PointerEventType::ButtonUp;
                        pev.button = static_cast<PathIOPointerMixer::PointerButton>(static_cast<int>(mev.button));
                        break;
                    case MouseEventType::Wheel:
                        pev.type  = PathIOPointerMixer::PointerEventType::Wheel;
                        pev.wheel = mev.wheel;
                        break;
                }
                pev.timestampNs = 0; // not relevant for this test
                mixerRaw->simulateEvent(pev);
            } else {
                // No event available; small backoff
                std::this_thread::sleep_for(2ms);
            }
        }
    });

    // Create a user-level alias that exposes the "default pointer" view (again, providers don't care).
    // The alias simply forwards to the mixer subtree when accessing any relative path.
    auto aliasUptr = std::make_unique<PathAlias>(root, "/aggregate/pointer");
    auto* aliasRaw = aliasUptr.get();
    {
        auto ret = root->insert<"/system/default-pointer">(std::move(aliasUptr));
        REQUIRE(ret.errors.empty());
        REQUIRE(ret.nbrSpacesInserted == 1);
    }

    SUBCASE("Mouse move is visible through alias default pointer path") {
        // Simulate a relative move on the mouse
        mouseRaw->simulateMove(+5, -3);

        // Read via the alias (forwarded to mixer). We poll non-blocking to avoid depending on notify.
        auto evOpt = poll_read<PathIOPointerMixer::Event>(*root, "/system/default-pointer/events", 200ms);
        REQUIRE(evOpt.has_value());
        auto ev = *evOpt;
        CHECK(ev.type == PathIOPointerMixer::PointerEventType::Move);
        CHECK(ev.dx == 5);
        CHECK(ev.dy == -3);
    }

    SUBCASE("Multiple sources can feed the mixer; alias exposes the merged stream") {
        // Source 0: mouse via forwarder
        mouseRaw->simulateButtonDown(MouseButton::Left, /*deviceId=*/0);

        // Source 1: another pointer device feeding directly into the mixer (e.g., tablet)
        mixerRaw->simulateAbsolute(100, 200, /*sourceId=*/1);

        // Read 2 events from the alias; order is by arrival into the mixer
        std::vector<PathIOPointerMixer::Event> got;
        for (int i = 0; i < 2; ++i) {
            auto evOpt = poll_take<PathIOPointerMixer::Event>(*root, "/system/default-pointer/events", 250ms);
            REQUIRE(evOpt.has_value());
            got.push_back(*evOpt);
        }

        // We don't assert exact ordering because it's timing-dependent, but both events should be present.
        bool sawMouseClick = false;
        bool sawAbsolute   = false;
        for (auto const& e : got) {
            if (e.type == PathIOPointerMixer::PointerEventType::ButtonDown) sawMouseClick = true;
            if (e.type == PathIOPointerMixer::PointerEventType::AbsoluteMove && e.x == 100 && e.y == 200) sawAbsolute = true;
        }
        CHECK(sawMouseClick);
        CHECK(sawAbsolute);
    }

    SUBCASE("Alias can be retargeted atomically by user code") {
        // Mount a second mixer
        auto mixer2 = std::make_unique<PathIOPointerMixer>();
        auto* mixer2Raw = mixer2.get();
        auto ret = root->insert<"/aggregate/pointer2">(std::move(mixer2));
        REQUIRE(ret.errors.empty());
        REQUIRE(ret.nbrSpacesInserted == 1);

        // Retarget alias to the new mixer subtree
        aliasRaw->setTargetPrefix("/aggregate/pointer2");

        // Feed an event into the new mixer directly (user wiring could be updated similarly)
        mixer2Raw->simulateWheel(+3, /*sourceId=*/2);

        // Read from the alias; should reflect the new target
        auto evOpt = poll_read<PathIOPointerMixer::Event>(*root, "/system/default-pointer/events", 250ms);
        REQUIRE(evOpt.has_value());
        CHECK(evOpt->type == PathIOPointerMixer::PointerEventType::Wheel);
        CHECK(evOpt->wheel == 3);
    }

    // Tear down
    forwarderRunning.store(false, std::memory_order_release);
    forwarder.join();
}