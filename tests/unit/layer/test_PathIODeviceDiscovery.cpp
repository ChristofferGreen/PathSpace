#include "ext/doctest.h"
#include <string>
#include <chrono>

#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/PathIODeviceDiscovery.hpp>

using namespace SP;
using namespace std::chrono_literals;

static bool contains_line(std::string const& haystack, std::string const& needle) {
    // Ensure we match full lines; add '\n' boundaries
    auto with_newlines = haystack;
    if (!with_newlines.empty() && with_newlines.back() != '\n') with_newlines.push_back('\n');
    auto pat = needle;
    if (!pat.empty() && pat.back() != '\n') pat.push_back('\n');
    return with_newlines.find(pat) != std::string::npos;
}

TEST_CASE("PathIODeviceDiscovery - Empty and basic listing") {
    PathIODeviceDiscovery dev;

    SUBCASE("Root class listing on empty discovery is empty string") {
        auto r = dev.read<std::string>("/");
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
    }

    SUBCASE("Unknown class returns NotFound") {
        auto r = dev.read<"/mice", std::string>();
        CHECK_FALSE(r.has_value());
    }
}

TEST_CASE("PathIODeviceDiscovery - Simulated devices listing and metadata") {
    PathIODeviceDiscovery dev;

    // Add a mouse device and a keyboard device
    {
        PathIODeviceDiscovery::SimDevice m0;
        m0.id = 0; m0.vendor = "Acme"; m0.product = "FastMouse"; m0.connection = "USB";
        m0.capabilities = {"wheel", "buttons:3"};
        dev.addSimulatedDevice("mice", m0);
    }
    {
        PathIODeviceDiscovery::SimDevice k0;
        k0.id = 0; k0.vendor = "Acme"; k0.product = "ClickyKeys"; k0.connection = "Bluetooth";
        k0.capabilities = {"layout:us", "nkey-rollover"};
        dev.addSimulatedDevice("keyboards", k0);
    }

    SUBCASE("Root class listing contains both classes (order not enforced)") {
        auto r = dev.read<std::string>("/");
        REQUIRE(r.has_value());
        auto s = r.value();
        CHECK(contains_line(s, "mice"));
        CHECK(contains_line(s, "keyboards"));
    }

    SUBCASE("Class listing returns device IDs per line") {
        auto rm = dev.read<"/mice", std::string>();
        REQUIRE(rm.has_value());
        CHECK(contains_line(rm.value(), "0"));

        auto rk = dev.read<"/keyboards", std::string>();
        REQUIRE(rk.has_value());
        CHECK(contains_line(rk.value(), "0"));
    }

    SUBCASE("Synonym class names are normalized (mouse -> mice)") {
        PathIODeviceDiscovery::SimDevice m1;
        m1.id = 1; m1.vendor = "Globex"; m1.product = "Precision"; m1.connection = "USB-C";
        dev.addSimulatedDevice("mouse", m1); // synonym
        auto rm = dev.read<"/mice", std::string>();
        REQUIRE(rm.has_value());
        CHECK(contains_line(rm.value(), "1"));
    }

    SUBCASE("Device metadata is exposed as key=value lines") {
        auto meta = dev.read<"/mice/0/meta", std::string>();
        REQUIRE(meta.has_value());
        auto s = meta.value();
        CHECK(contains_line(s, "id=0"));
        CHECK(contains_line(s, "vendor=Acme"));
        CHECK(contains_line(s, "product=FastMouse"));
        CHECK(contains_line(s, "connection=USB"));
    }

    SUBCASE("Device capabilities are exposed as one per line") {
        auto caps = dev.read<"/mice/0/capabilities", std::string>();
        REQUIRE(caps.has_value());
        auto s = caps.value();
        CHECK(contains_line(s, "wheel"));
        CHECK(contains_line(s, "buttons:3"));
    }

    SUBCASE("Type mismatch returns error") {
        auto bad = dev.read<"/mice", int>();
        CHECK_FALSE(bad.has_value());
    }

    SUBCASE("Blocking read option is ignored (returns immediately)") {
        auto r = dev.read<"/mice", std::string>(Block{10ms});
        REQUIRE(r.has_value());
        CHECK(contains_line(r.value(), "0"));
    }
}

TEST_CASE("PathIODeviceDiscovery - Removal updates visibility") {
    PathIODeviceDiscovery dev;

    PathIODeviceDiscovery::SimDevice m0;
    m0.id = 0; m0.vendor = "Acme"; m0.product = "GoneSoon"; m0.connection = "USB";
    dev.addSimulatedDevice("mice", m0);

    // Sanity: present
    {
        auto rm = dev.read<"/mice", std::string>();
        REQUIRE(rm.has_value());
        CHECK(contains_line(rm.value(), "0"));
    }

    // Remove and verify NotFound
    dev.removeSimulatedDevice("mice", 0);

    {
        auto rm = dev.read<"/mice", std::string>();
        CHECK_FALSE(rm.has_value()); // Class now empty -> NotFound by contract
    }
    {
        auto meta = dev.read<"/mice/0/meta", std::string>();
        CHECK_FALSE(meta.has_value());
    }
}

TEST_CASE("PathIODeviceDiscovery - Mounted under PathSpace at /dev") {
    PathSpace space;

    // Create discovery and keep raw pointer for simulation updates
    auto disc = std::make_unique<PathIODeviceDiscovery>();
    auto* raw = disc.get();
    auto ret = space.insert<"/dev">(std::move(disc));
    REQUIRE(ret.errors.empty());
    REQUIRE(ret.nbrSpacesInserted == 1);

    // Populate via raw pointer
    PathIODeviceDiscovery::SimDevice m0;
    m0.id = 7; m0.vendor = "Acme"; m0.product = "MountMouse"; m0.connection = "USB";
    m0.capabilities = {"wheel", "buttons:3"};
    raw->addSimulatedDevice("mice", m0);

    SUBCASE("Parent read lists devices for a specific class under the mount") {
        auto r = space.read<"/dev/mice", std::string>();
        REQUIRE(r.has_value());
        CHECK(contains_line(r.value(), "7"));
    }

    SUBCASE("Parent read sees device ids under mounted class path") {
        auto r = space.read<"/dev/mice", std::string>();
        REQUIRE(r.has_value());
        CHECK(contains_line(r.value(), "7"));
    }

    SUBCASE("Parent read sees metadata and capabilities") {
        auto meta = space.read<"/dev/mice/7/meta", std::string>();
        REQUIRE(meta.has_value());
        CHECK(contains_line(meta.value(), "id=7"));
        CHECK(contains_line(meta.value(), "product=MountMouse"));

        auto caps = space.read<"/dev/mice/7/capabilities", std::string>();
        REQUIRE(caps.has_value());
        CHECK(contains_line(caps.value(), "wheel"));
    }
}