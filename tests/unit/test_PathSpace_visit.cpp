#include "third_party/doctest.h"
#include <algorithm>
#include <vector>

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
