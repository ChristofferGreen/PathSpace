#include "path/ConcretePath.hpp"
#include "path/GlobPath.hpp"
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"
#include "path/ConcreteName.hpp"
#include "path/GlobName.hpp"
#include "layer/PathView.hpp"
#include "PathSpace.hpp"
#include "type/TypeMetadataRegistry.hpp"
#include "core/PathSpaceContext.hpp"

#include "third_party/doctest.h"
#include <expected>
#include <string>
#include <optional>
#include <tuple>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

using namespace SP;

TEST_SUITE("path.view.coverage") {
TEST_CASE("ConcreteName and GlobName basics") {
    ConcreteName cname{"alpha"};
    CHECK(cname == "alpha");
    CHECK(cname == ConcreteName{"alpha"});
    CHECK((cname <=> ConcreteName{"beta"}) == std::strong_ordering::less);

    GlobName gstar{"a*"};
    auto [match, super] = gstar.match(std::string_view{"alpha"});
    CHECK(match);
    CHECK_FALSE(super);

    GlobName gquestion{"a?c"};
    auto [match2, super2] = gquestion.match(std::string_view{"abc"});
    CHECK(match2);
    CHECK_FALSE(super2);

    GlobName gset{"b*d"};
    auto [match3, super3] = gset.match(ConcreteName{"bd"});
    CHECK(match3);
    CHECK_FALSE(super3);
}

TEST_CASE("GlobName operators and edge patterns") {
    GlobName literal{"abc"};
    GlobName same{"abc"};
    GlobName globAll{"*"};

    CHECK((literal <=> same) == std::strong_ordering::equal);
    CHECK(literal == same);
    CHECK(literal == "abc");
    CHECK(literal == ConcreteName{"abc"});

    CHECK(literal.isConcrete());
    CHECK_FALSE(literal.isGlob());
    CHECK(globAll.isGlob());
    CHECK_FALSE(globAll.isConcrete());

    auto [escapedMatch, escapedSuper] = GlobName{"\\x"}.match(std::string_view{"y"});
    CHECK_FALSE(escapedMatch);
    CHECK_FALSE(escapedSuper);

    auto starResult = GlobName{"a*b"}.match(std::string_view{"aaa"});
    CHECK_FALSE(std::get<0>(starResult));
    CHECK_FALSE(std::get<1>(starResult));

    auto bracketResult = GlobName{"[a-c]"}.match(std::string_view{""});
    CHECK_FALSE(std::get<0>(bracketResult));
    CHECK_FALSE(std::get<1>(bracketResult));

    auto exactResult = GlobName{"abc"}.match(std::string_view{"abc"});
    CHECK(std::get<0>(exactResult));
    CHECK_FALSE(std::get<1>(exactResult));
}

TEST_CASE("PathView join/strip helpers cover edge cases") {
    using SP::testing::joinCanonicalForTest;
    using SP::testing::stripPrefixForTest;

    CHECK(joinCanonicalForTest("/root", "/") == "/root");
    CHECK(joinCanonicalForTest("/root", "child") == "/root/child");
    CHECK(joinCanonicalForTest("relative", "../bad") == "relative/../bad");

    CHECK(stripPrefixForTest("/root", "/root") == std::optional<std::string>{"/"});
    CHECK(stripPrefixForTest("/rootchild", "/root") == std::optional<std::string>{"/child"});
    CHECK(stripPrefixForTest("/root/child", "/root") == std::optional<std::string>{"/child"});
    CHECK(stripPrefixForTest("/other", "/root") == std::nullopt);
}

TEST_CASE("ConcretePathIterator and GlobPathIterator iterate components") {
    ConcretePathString concrete{"/one/two"};
    std::vector<std::string> names;
    for (auto it = concrete.begin(); it != concrete.end(); ++it) {
        names.emplace_back(std::string((*it).getName()));
    }
    CHECK(names == std::vector<std::string>{"one", "two"});

    GlobPathString glob{"/o*/t?"};
    std::vector<std::string> globs;
    for (auto it = glob.begin(); it != glob.end(); ++it) {
        globs.emplace_back(std::string((*it).getName()));
    }
    CHECK(globs == std::vector<std::string>{"o*", "t?"});
}

TEST_CASE("Path validity checks") {
    Path<std::string> valid{"/root/child"};
    CHECK(valid.isValid());

    Path<std::string> noSlash{"relative"};
    CHECK_FALSE(noSlash.isValid());

    Path<std::string> dotPath{"/.hidden"};
    CHECK_FALSE(dotPath.isValid());
}

TEST_CASE("PathView respects permissions and root") {
    auto ctx   = std::make_shared<PathSpaceContext>();
    auto space = std::make_shared<PathSpace>(ctx, "");
    auto perm = [](Iterator const& iter) -> Permission {
        auto str = iter.toString();
        bool allowed = str.rfind("/allowed", 0) == 0;
        return Permission{.read = allowed, .write = allowed, .execute = allowed};
    };
    PathView view{space, perm, "/root"};

    // Denied write
    auto denied = view.in(Iterator{"/denied/value"}, InputData{42});
    CHECK_FALSE(denied.errors.empty());

    // Allowed write/read through view
    auto ok = view.in(Iterator{"/allowed/value"}, InputData{123});
    REQUIRE(ok.errors.empty());

    int outValue = 0;
    auto err = view.out(Iterator{"/allowed/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK_FALSE(err.has_value());
    CHECK(outValue == 123);

    // Visit should re-root under view root
    int visitCount = 0;
    VisitOptions opts;
    opts.root = "/allowed";
    opts.includeValues = true;
    auto visitResult = view.visit(
        [&](PathEntry const& entry, ValueHandle& /*handle*/) {
            ++visitCount;
            CHECK_FALSE(entry.path.empty());
            return VisitControl::Continue;
        },
        opts); // explicit root to test joinCanonical
    CHECK(visitResult);
    CHECK(visitCount >= 1);
}

TEST_CASE("TypeMetadataRegistry custom registration and lookup") {
    struct CoverageType {
        int value = 7;
    };

    auto& registry = TypeMetadataRegistry::instance();
    std::string name = "CoverageType_" + std::to_string(reinterpret_cast<std::uintptr_t>(&registry));
    CHECK(registry.registerType<CoverageType>(name));
    CHECK_FALSE(registry.registerType<CoverageType>(name)); // duplicate should fail

    auto byName = registry.findByName(name);
    REQUIRE(byName.has_value());
    CHECK(byName->operations.size == sizeof(CoverageType));
    CHECK(byName->metadata.typeInfo != nullptr);

    auto byType = registry.findByType(typeid(CoverageType));
    REQUIRE(byType.has_value());
    CHECK(byType->type_name == name);
}

TEST_CASE("Expected shim emits what()") {
    std::expected<void, Error> exp = std::unexpected(Error{Error::Code::UnknownError, "boom"});
    try {
        exp.value();
        FAIL("expected to throw");
    } catch (std::bad_expected_access<Error> const& ex) {
        // Touch what() to cover the shim definition.
        CHECK(std::string{ex.what()}.find("bad access to std::expected") != std::string::npos);
    }

    // Explicitly exercise bad_expected_access<void> via a derived shim to hit the TU.
    struct BadVoid : std::bad_expected_access<void> {
        BadVoid() : std::bad_expected_access<void>() {}
        using std::bad_expected_access<void>::what;
    };
    BadVoid vex;
    CHECK(std::string{vex.what()}.find("bad access to std::expected") != std::string::npos);
}

TEST_CASE("PathView handles missing backing space") {
    auto perm = [](Iterator const&) { return Permission{}; };
    PathView view{std::shared_ptr<PathSpaceBase>{}, perm, "/root"};

    auto insertResult = view.insert("/any", 1);
    CHECK_FALSE(insertResult.errors.empty());

    int outVal = 0;
    auto outErr = view.out(Iterator{"/any"}, InputMetadataT<int>{}, Out{}, &outVal);
    CHECK(outErr.has_value());

    // Should be safe no-ops when space is absent.
    view.notify("/any");
    view.shutdown();
}

TEST_CASE("PathView join/strip normalization and notify passthrough") {
    auto ctx   = std::make_shared<PathSpaceContext>();
    auto space = std::make_shared<PathSpace>(ctx, "/root");
    auto perm  = [](Iterator const&) { return Permission{true, true, true}; };

    // Trailing slash + leading slash should collapse to single separator.
    PathView view{space, perm, "/root/"};
    space->insert("/root/child/node", 7);

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.root          = "/child";
    opts.includeValues = true;
    auto visitRes = view.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(visitRes);
    CHECK_FALSE(visited.empty());
    CHECK(visited.front().rfind("/child", 0) == 0);

    // Prefix empty: joinCanonical should reduce to suffix or "/" correctly.
    PathView rootless{space, perm, ""};
    auto visitRes2 = rootless.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        VisitOptions{});
    CHECK(visitRes2);

    // notify should forward to backing space; waiting on PathSpaceContext demonstrates the call.
    std::atomic<std::cv_status> status{std::cv_status::timeout};
    std::thread waiter([&] {
        auto guard = ctx->wait("/root/ping");
        status = guard.wait_until(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    view.notify("/root/ping");
    waiter.join();
}

TEST_CASE("PathView enforces read permissions and root filtering") {
    auto ctx   = std::make_shared<PathSpaceContext>();
    auto space = std::make_shared<PathSpace>(ctx, "/root");

    // Insert data both inside and outside the view root.
    space->insert("/root/public/value", 11);
    space->insert("/root/secret/value", 22);
    space->insert("/other/outside", 33);

    auto perm = [](Iterator const& iter) -> Permission {
        auto path = iter.toString();
        bool allow = path.find("secret") == std::string::npos;
        return Permission{.read = allow, .write = true, .execute = true};
    };

    PathView view{space, perm, "/root"};

    int outValue = -1;
    auto err = view.out(Iterator{"/secret/value"}, InputMetadataT<int>{}, Out{}, &outValue);
    CHECK(err);
    CHECK(err->code == Error::Code::InvalidPermissions);

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.root          = "/"; // request everything under the view root
    opts.includeValues = true;
    auto visitRes = view.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);

    REQUIRE(visitRes);
    auto contains = [&](std::string const& needle) {
        return std::any_of(visited.begin(), visited.end(), [&](std::string const& path) {
            return path.find(needle) != std::string::npos;
        });
    };
    CHECK(contains("public/value"));
    CHECK_FALSE(contains("secret"));
    CHECK_FALSE(contains("outside"));
}

TEST_CASE("PathView visit skips entries outside mount and denied by permissions") {
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
        bool allow = sv.find("blocked") == std::string_view::npos;
        return Permission{.read = allow, .write = true, .execute = true};
    };

    PathView view{backing, perms, "/mount"};

    std::vector<std::string> visited;
    auto result = view.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.push_back(entry.path);
        return VisitControl::Continue;
    });

    REQUIRE(result.has_value());
    CHECK(visited == std::vector<std::string>{"/", "/visible"});
}

TEST_CASE("PathView shutdown forwards when backing is present") {
    struct TrackingSpace : PathSpace {
        int shutdowns{0};
        void shutdown() override { ++shutdowns; }
    };

    auto backing = std::make_shared<TrackingSpace>();
    PathView view{backing, [](Iterator const&) { return Permission{}; }, "/root"};

    view.shutdown();
    CHECK(backing->shutdowns == 1);
}

TEST_CASE("GlobPath supermatch and length mismatches") {
    GlobPathString super{"/**"};
    ConcretePathString deep{"/a/b/c"};
    CHECK(super == deep); // ** should super-match remaining components

    GlobPathString simple{"/foo/*"};
    ConcretePathString tooShort{"/foo"};
    CHECK_FALSE(simple == tooShort); // mismatch in component count

    GlobPathString invalid{"relative"};
    ConcretePathString valid{"/valid"};
    CHECK_FALSE(invalid == valid); // invalid glob should fail quickly

    CHECK(simple.isGlob());
    CHECK_FALSE(simple.isConcrete());
}
}
