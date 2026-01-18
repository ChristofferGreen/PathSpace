#include "third_party/doctest.h"
#include <pathspace/layer/PathView.hpp>
#include <pathspace/PathSpace.hpp>
#include "type/InputMetadataT.hpp"

using namespace SP;
using namespace std::chrono_literals;

TEST_SUITE("layer.path.view") {
TEST_CASE("PathSpace View") {
    std::shared_ptr<PathSpace> space = std::make_shared<PathSpace>();
    SUBCASE("Capability Types") {
        auto     permissions = [](Iterator const& iterator) -> Permission {
            if (iterator.toStringView().starts_with("/legal"))
                return Permission{true, true, true};
            return Permission{false, false, false}; };
        PathView pspace(space, permissions);
        CHECK(pspace.insert("/legal/test", 4).nbrValuesInserted == 1);
        CHECK(pspace.insert("/illegal/test", 4).nbrValuesInserted == 0);
    }

    SUBCASE("Mouse Space") {
        // CHECK(pspace->insert("/os/dev/io/pointer", PathIO{}).nbrValuesInserted == 1);
        // CHECK(pspace->read<"/os/devices/io/pointer/position", std::tuple<int, int>>() == std::make_tuple(0, 0));
        // CHECK(pspace->read<"/os/devices/io/pointer/position/0", std::tuple<int, int>>() == std::make_tuple(0, 0));
    }
}

TEST_CASE("PathView in/out respect root prefix and permissions") {
    auto backing = std::make_shared<PathSpace>();
    auto perms = [](Iterator const& iter) -> Permission {
        auto sv = iter.toStringView();
        bool allowed = sv.find("/deny") == std::string_view::npos;
        return Permission{.read = allowed, .write = allowed, .execute = true};
    };

    PathView view(backing, perms, "/root");

    // Allowed path should round-trip through backing space with root prefix applied.
    auto insOk = view.insert("/allow/value", 42);
    CHECK(insOk.errors.empty());

    int out = 0;
    auto errOk = view.out(Iterator{"/allow/value"}, InputMetadataT<int>{}, Out{}, &out);
    CHECK_FALSE(errOk.has_value());
    CHECK(out == 42);

    // Denied paths surface InvalidPermissions on both insert and read.
    auto insDenied = view.insert("/deny/value", 7);
    REQUIRE_FALSE(insDenied.errors.empty());
    CHECK(insDenied.errors.front().code == Error::Code::InvalidPermissions);

    auto errDenied = view.out(Iterator{"/deny/value"}, InputMetadataT<int>{}, Out{}, &out);
    REQUIRE(errDenied.has_value());
    CHECK(errDenied->code == Error::Code::InvalidPermissions);
}

TEST_CASE("PathView visit remaps prefix and filters by permission") {
    struct ScriptedVisitSpace : PathSpace {
        std::vector<PathEntry> entries;
        auto visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> override {
            (void)options;
            for (auto const& entry : entries) {
                ValueHandle handle{};
                auto control = visitor(entry, handle);
                if (control == VisitControl::Stop) {
                    break;
                }
            }
            return {};
        }
    };

    auto backing = std::make_shared<ScriptedVisitSpace>();
    backing->entries = {
        PathEntry{"/mount", true, false, false, 0, DataCategory::None},
        PathEntry{"/mount/visible", false, true, false, 0, DataCategory::Fundamental},
        PathEntry{"/elsewhere/skip", false, true, false, 0, DataCategory::Fundamental},
        PathEntry{"/mount/blocked", false, true, false, 0, DataCategory::Fundamental},
    };

    auto perms = [](Iterator const& iter) -> Permission {
        auto sv = iter.toStringView();
        if (sv.find("blocked") != std::string_view::npos) {
            return Permission{.read = false, .write = true, .execute = true};
        }
        return Permission{};
    };

    PathView view(backing, perms, "/mount");

    std::vector<std::string> visited;
    auto result = view.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.push_back(entry.path);
        return VisitControl::Continue;
    });

    REQUIRE(result.has_value());
    CHECK(visited == std::vector<std::string>{"/", "/visible"});
}

TEST_CASE("PathView notify and shutdown forward to backing space") {
    struct TrackingSpace : PathSpace {
        int                     shutdowns{0};
        std::vector<std::string> notifications;
        void shutdown() override { ++shutdowns; }
        void notify(std::string const& path) override { notifications.push_back(path); }
    };

    auto backing = std::make_shared<TrackingSpace>();
    PathView view(backing, [](Iterator const&) { return Permission{}; }, "/");

    view.notify("/note");
    view.shutdown();

    CHECK(backing->notifications == std::vector<std::string>{"/note"});
    CHECK(backing->shutdowns == 1);
}

TEST_CASE("joinCanonical/stripPrefix helpers normalize paths") {
    using testing::joinCanonicalForTest;
    using testing::stripPrefixForTest;

    CHECK(joinCanonicalForTest("/", "") == "/");
    CHECK(joinCanonicalForTest("/root", "/child") == "/root/child");
    CHECK(joinCanonicalForTest("/root/", "/child") == "/root/child");
    CHECK(joinCanonicalForTest("/root", "child") == "/root/child");

    auto stripped = stripPrefixForTest("/root/child/grand", "/root");
    REQUIRE(stripped.has_value());
    CHECK(*stripped == "/child/grand");

    auto strippedRoot = stripPrefixForTest("/root", "/root");
    REQUIRE(strippedRoot.has_value());
    CHECK(*strippedRoot == "/");

    auto noMatch = stripPrefixForTest("/other/path", "/root");
    CHECK_FALSE(noMatch.has_value());
}
}
