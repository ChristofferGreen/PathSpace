#include "third_party/doctest.h"
#include <algorithm>
#include <vector>
#include <typeinfo>

#include <pathspace/PathSpace.hpp>

using namespace SP;

namespace {

auto collectPaths(PathSpace& space, VisitOptions options = {}) -> std::vector<std::string> {
    std::vector<std::string> paths;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            paths.push_back(entry.path);
            return VisitControl::Continue;
        },
        options);
    REQUIRE(result);
    return paths;
}

} // namespace

TEST_SUITE_BEGIN("pathspace.visit");

TEST_CASE("PathSpace visit traverses nodes and reads values") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 42).nbrValuesInserted == 1);
    REQUIRE(space.insert("/alpha/beta", 7).nbrValuesInserted == 1);
    REQUIRE(space.insert("/gamma", 9).nbrValuesInserted == 1);

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            visited.push_back(entry.path);
        if (entry.path == "/alpha/value" && entry.hasValue) {
            auto value = handle.read<int>();
            REQUIRE(value);
            CHECK(*value == 42);
        }
            return VisitControl::Continue;
        });
    CHECK(result);

    CHECK(std::find(visited.begin(), visited.end(), "/") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/alpha") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/alpha/beta") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/gamma") != visited.end());
}

TEST_CASE("ValueHandle can read POD fast-path payloads during visit") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 10).errors.empty());
    REQUIRE(space.insert("/pod", 20).errors.empty());

    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            auto snapshot = handle.snapshot();
            REQUIRE(snapshot);
            CHECK_FALSE(snapshot->hasSerializedPayload); // stays on POD fast path
            CHECK(snapshot->queueDepth == 2);

            auto value = handle.read<int>();
            CHECK(value);
            CHECK(*value == 10);
            return VisitControl::Stop;
        });
    REQUIRE(ok);

    auto first = space.take<int>("/pod");
    REQUIRE(first.has_value());
    CHECK(*first == 10);
    auto second = space.take<int>("/pod");
    REQUIRE(second.has_value());
    CHECK(*second == 20);
}

TEST_CASE("PathSpace visit respects root and depth options") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/beta/value", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/alpha/beta/delta/value", 3).nbrValuesInserted == 1);

    VisitOptions options;
    options.root     = "/alpha";
    options.maxDepth = 1;

    auto paths = collectPaths(space, options);
    CHECK(paths == std::vector<std::string>{"/alpha", "/alpha/beta"});

    options.maxDepth = 0;
    paths             = collectPaths(space, options);
    CHECK(paths == std::vector<std::string>{"/alpha"});
}

TEST_CASE("PathSpace visit enforces child limit and disables values when requested") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/b", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/c", 3).nbrValuesInserted == 1);

    VisitOptions options;
    options.root          = "/root";
    options.maxChildren   = 2;
    options.includeValues = false;

    std::vector<std::string> children;
    auto                     result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/root") {
                children.push_back(entry.path);
                auto value = handle.read<int>();
                CHECK_FALSE(value.has_value());
            }
            return VisitControl::Continue;
        },
        options);
    CHECK(result);
    CHECK(children.size() == 2);
}

TEST_CASE("PathSpace visit validates indexed roots and nested traversal") {
    PathSpace space;

    SUBCASE("Indexed nested root requires includeNestedSpaces") {
        auto nested0 = std::make_unique<PathSpace>();
        REQUIRE(nested0->insert("/child0", 1).nbrValuesInserted == 1);
        auto nested1 = std::make_unique<PathSpace>();
        REQUIRE(nested1->insert("/child1", 2).nbrValuesInserted == 1);

        REQUIRE(space.insert("/mount", std::move(nested0)).nbrSpacesInserted == 1);
        REQUIRE(space.insert("/mount", std::move(nested1)).nbrSpacesInserted == 1);

        VisitOptions includeNested;
        includeNested.root               = "/mount[1]";
        includeNested.includeNestedSpaces = true;

        std::vector<std::string> visited;
        auto ok = space.visit(
            [&](PathEntry const& entry, ValueHandle&) {
                visited.push_back(entry.path);
                return VisitControl::Continue;
            },
            includeNested);
        REQUIRE(ok);
        CHECK(std::find(visited.begin(), visited.end(), "/mount[1]") != visited.end());
        CHECK(std::find(visited.begin(), visited.end(), "/mount[1]/child1") != visited.end());

        VisitOptions nestedDisabled;
        nestedDisabled.root                = "/mount[1]";
        nestedDisabled.includeNestedSpaces = false;

        std::vector<std::string> shallow;
        auto skip = space.visit(
            [&](PathEntry const& entry, ValueHandle&) {
                shallow.push_back(entry.path);
                return VisitControl::Continue;
            },
            nestedDisabled);
        REQUIRE(skip);
        CHECK(std::find(shallow.begin(), shallow.end(), "/mount[1]") != shallow.end());
        CHECK(std::find(shallow.begin(), shallow.end(), "/mount[1]/child1") == shallow.end());
    }
}

TEST_CASE("ValueHandle snapshot reflects POD fast path upgrade to generic") {
    PathSpace space;
    REQUIRE(space.insert("/queue", 1).errors.empty());
    REQUIRE(space.insert("/queue", 2).errors.empty());

    ValueSnapshot before{};
    auto preVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/queue") {
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                before = *snapshot;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(preVisit);
    CHECK(before.queueDepth == 2);
    REQUIRE(before.types.size() == 2);
    CHECK(before.types[0].typeInfo == &typeid(int));
    CHECK(before.types[1].typeInfo == &typeid(int));

    // Upgrade the node by inserting a non-POD type.
    REQUIRE(space.insert("/queue", std::string("tail")).errors.empty());

    ValueSnapshot after{};
    auto postVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/queue") {
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                after = *snapshot;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(postVisit);
    CHECK(after.queueDepth >= 2);
    REQUIRE(after.types.size() >= 2);
    CHECK(after.hasSerializedPayload); // migrated off POD fast path
    bool hasInt = false;
    bool hasString = false;
    for (auto const& t : after.types) {
        hasInt |= (t.typeInfo == &typeid(int));
        hasString |= (t.typeInfo == &typeid(std::string));
    }
    CHECK(hasInt);
    CHECK(hasString);

    // Validate queue contents preserved in order after upgrade.
    auto first = space.take<int>("/queue");
    REQUIRE(first.has_value());
    CHECK(*first == 1);
    auto second = space.take<int>("/queue");
    REQUIRE(second.has_value());
    CHECK(*second == 2);
    auto tail = space.take<std::string>("/queue");
    REQUIRE(tail.has_value());
    CHECK(*tail == "tail");
}

TEST_SUITE_END();
