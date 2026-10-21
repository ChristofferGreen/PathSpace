#include "PathSpaceVisitTestSupport.hpp"

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

TEST_CASE("PathSpace visit rejects malformed roots and empty visitors") {
    PathSpace space;

    // Empty visitor callback should surface InvalidType.
    PathVisitor emptyVisitor;
    auto emptyResult = space.visit(emptyVisitor, VisitOptions{});
    CHECK_FALSE(emptyResult);
    CHECK(emptyResult.error().code == Error::Code::InvalidType);

    // Malformed indexed root should not crash even if it canonicalizes to '/'.
    auto badRoot = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        VisitOptions{.root = "/alpha[abc]"}); // non-numeric index
    if (badRoot) {
        CHECK(badRoot.has_value());
    } else {
        CHECK(badRoot.error().code == Error::Code::InvalidPath);
    }
}

TEST_CASE("PathSpace visit stops traversal when visitor requests Stop") {
    PathSpace space;
    REQUIRE(space.insert("/alpha", 1).errors.empty());
    REQUIRE(space.insert("/beta", 2).errors.empty());

    std::vector<std::string> seen;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            seen.push_back(entry.path);
            return VisitControl::Stop; // stop immediately after first node
        });
    REQUIRE(result);
    CHECK(seen.size() == 1);
    CHECK(seen.front() == "/");
}

TEST_CASE("PathSpace visit snapshot reports empty nodes") {
    PathSpace space;

    std::optional<ValueSnapshot> snapshot;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/") {
                auto snap = handle.snapshot();
                REQUIRE(snap.has_value());
                snapshot = snap.value();
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(result);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->queueDepth == 0);
    CHECK(snapshot->types.empty());
    CHECK_FALSE(snapshot->hasExecutionPayload);
    CHECK_FALSE(snapshot->hasSerializedPayload);
    CHECK(snapshot->rawBufferBytes == 0);
}

TEST_CASE("PathSpace visit honors SkipChildren and includeValues=false") {
    PathSpace space;
    REQUIRE(space.insert("/root/child/grand", 9).errors.empty());

    VisitOptions opts;
    opts.includeValues = false; // disable value access

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            visited.push_back(entry.path);
            // Attempting to read with values disabled should surface NotSupported.
            auto val = handle.read<int>();
            CHECK_FALSE(val);
            CHECK(val.error().code == Error::Code::NotSupported);

            if (entry.path == "/root/child") {
                return VisitControl::SkipChildren; // should prevent visiting /root/child/grand
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);

    CHECK(std::find(visited.begin(), visited.end(), "/") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/root") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/root/child") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/root/child/grand") == visited.end());
}

TEST_CASE("PathSpace visit treats empty root as canonical slash") {
    PathSpace space;
    REQUIRE(space.insert("/only", 1).errors.empty());

    VisitOptions opts;
    opts.root = ""; // triggers empty-root canonicalization path

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);
    CHECK(std::find(visited.begin(), visited.end(), "/") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/only") != visited.end());
}

TEST_CASE("PathSpace visit rejects missing indexed root component") {
    PathSpace space;

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/missing[1]";

    auto result = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        opts);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::NoSuchPath);
}

TEST_SUITE_END();
