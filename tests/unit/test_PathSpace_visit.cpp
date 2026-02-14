#include "third_party/doctest.h"
#include "core/NodeData.hpp"
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

class BrokenVisitSpace final : public PathSpaceBase {
public:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "BrokenVisitSpace does not support out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};

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

TEST_CASE("ValueHandle reports empty snapshot and queue depth for empty nodes") {
    PathSpace space;

    VisitOptions options;
    options.includeValues = true;

    bool sawRoot = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/") {
                sawRoot = true;
                CHECK(handle.queueDepth() == 0);
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                CHECK(snapshot->queueDepth == 0);
                CHECK(snapshot->types.empty());
                CHECK_FALSE(snapshot->hasExecutionPayload);
                CHECK_FALSE(snapshot->hasSerializedPayload);
                CHECK(snapshot->rawBufferBytes == 0);
            }
            return VisitControl::Continue;
        },
        options);
    CHECK(result);
    CHECK(sawRoot);
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

TEST_CASE("ValueHandle reports type mismatch for POD payload reads") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 10).errors.empty());

    VisitOptions options;
    options.includeValues = true;

    bool sawPod = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                sawPod = true;
                auto bad = handle.read<float>();
                CHECK_FALSE(bad.has_value());
                CHECK(bad.error().code == Error::Code::TypeMismatch);
            }
            return VisitControl::Continue;
        },
        options);
    CHECK(result);
    CHECK(sawPod);
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

TEST_CASE("PathSpace visit surfaces nested visit errors") {
    PathSpace space;

    auto broken = std::unique_ptr<PathSpaceBase>(std::make_unique<BrokenVisitSpace>().release());
    REQUIRE(space.insert("/mount", std::move(broken)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.includeNestedSpaces = true;

    auto result = space.visit(
        [&](PathEntry const&, ValueHandle&) {
            return VisitControl::Continue;
        },
        options);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::NotSupported);
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

    SUBCASE("Nested path without child resolves through includeNestedSpaces") {
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/inner/value", 5).nbrValuesInserted == 1);
        REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

        VisitOptions opts;
        opts.includeNestedSpaces = true;
        opts.root                = "/mount/inner";

        std::vector<std::string> visited;
        auto ok = space.visit(
            [&](PathEntry const& entry, ValueHandle&) {
                visited.push_back(entry.path);
                return VisitControl::Continue;
            },
            opts);
        REQUIRE(ok);
        CHECK(std::find(visited.begin(), visited.end(), "/mount/inner") != visited.end());
        CHECK(std::find(visited.begin(), visited.end(), "/mount/inner/value") != visited.end());

        VisitOptions disallow;
        disallow.includeNestedSpaces = false;
        disallow.root                = "/mount/inner";
        auto missing = space.visit(
            [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
            disallow);
        CHECK_FALSE(missing);
        CHECK(missing.error().code == Error::Code::NoSuchPath);
    }

    SUBCASE("Nested mount visit rejects ancestor roots when includeNestedSpaces disabled") {
        auto nested = std::make_unique<PathSpace>();
        REQUIRE(nested->insert("/child", 1).nbrValuesInserted == 1);
        REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

        VisitOptions opts;
        opts.includeNestedSpaces = false;
        opts.root                = "/mount/child";

        auto result = space.visit(
            [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
            opts);
        CHECK_FALSE(result);
        CHECK(result.error().code == Error::Code::NoSuchPath);
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

TEST_CASE("ValueHandle snapshot handles nodes without payload") {
    PathSpace space;
    // Create a subtree so root has children but no payload.
    REQUIRE(space.insert("/root/child", 1).errors.empty());

    VisitOptions opts;
    opts.includeValues = true;
    bool sawRoot       = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/") {
                sawRoot = true;
                auto snap = handle.snapshot();
                REQUIRE(snap);
                CHECK(snap->queueDepth == 0);
                CHECK_FALSE(snap->hasSerializedPayload);
                CHECK_FALSE(snap->hasExecutionPayload);
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);
    CHECK(sawRoot);
}

TEST_CASE("ValueHandle surfaces type mismatch for POD payloads") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 7).errors.empty());

    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            auto wrongType = handle.read<double>();
            CHECK_FALSE(wrongType);
            CHECK(wrongType.error().code == Error::Code::TypeMismatch);
            return VisitControl::Stop;
        });
    REQUIRE(visitResult);
}

TEST_CASE("ValueHandle read reports missing payload when node has no value") {
    PathSpace space;
    // Root has a child but no payload of its own.
    REQUIRE(space.insert("/root/child", 1).errors.empty());

    bool sawRoot = false;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/") {
                return VisitControl::Continue;
            }
            sawRoot = true;
            auto missing = handle.read<int>();
            CHECK_FALSE(missing);
            CHECK(missing.error().code == Error::Code::NoObjectFound);
            return VisitControl::Stop;
        });
    REQUIRE(result);
    CHECK(sawRoot);
}

TEST_CASE("ValueHandle read handles empty POD queues via snapshot fallback") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 1).errors.empty());

    // Drain the queue so the POD payload exists but holds no elements.
    auto first = space.take<int>("/pod");
    REQUIRE(first.has_value());
    auto second = space.take<int>("/pod");
    CHECK_FALSE(second.has_value());

    bool visitedPod = false;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path != "/pod") {
                return VisitControl::Continue;
            }
            visitedPod = true;
            auto empty = handle.read<int>();
            CHECK_FALSE(empty);
            CHECK(empty.error().code == Error::Code::NoObjectFound);
            return VisitControl::Stop;
        });
    REQUIRE(visitResult);
    CHECK(visitedPod);
}

TEST_CASE("ValueHandle queueDepth handles data, pod, and empty handles") {
    PathSpace space;
    REQUIRE(space.insert("/pod", 5).errors.empty());
    REQUIRE(space.insert("/data", std::string{"value"}).errors.empty());

    std::size_t podDepth  = 0;
    std::size_t dataDepth = 0;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                podDepth = handle.queueDepth();
            } else if (entry.path == "/data") {
                dataDepth = handle.queueDepth();
            }
            return VisitControl::Continue;
        });
    REQUIRE(visitResult);

    CHECK(podDepth == 1);
    CHECK(dataDepth == 1);

    ValueHandle empty{};
    CHECK(empty.queueDepth() == 0);
}

TEST_CASE("ValueHandle readInto surfaces permission and missing-node errors") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 9).errors.empty());

    VisitOptions opts;
    opts.includeValues = false;
    int dest = 0;
    auto visitResult = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/root/value") {
                auto err = handle.read<int>();
                REQUIRE_FALSE(err);
                CHECK(err.error().code == Error::Code::NotSupported);
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(visitResult);

    // Moved-from handles keep includeValues=true but lose backing node.
    std::optional<Expected<int>> movedErr;
    auto capture = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/root/value") {
                ValueHandle moved = std::move(handle);
                movedErr = handle.read<int>(); // moved-from handle
                (void)moved;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(capture);
    REQUIRE(movedErr.has_value());
    CHECK_FALSE(*movedErr);
    CHECK(movedErr->error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpace visit caps nested traversal at maxDepth") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/deep/value", 9).errors.empty());
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.maxDepth            = 1; // "/" (0) and "/mount" (1) only

    std::vector<std::string> visited;
    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(ok);

    CHECK(std::find(visited.begin(), visited.end(), "/") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/deep") == visited.end());
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

TEST_CASE("PathSpace visit skips nested spaces when depth budget is exhausted") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/inside", 1).errors.empty());
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.maxDepth            = 0; // only the starting node should be visited

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);
    CHECK(visited.size() == 1);
    CHECK(visited.front() == "/");
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

TEST_CASE("PathSpace visit rejects missing indexed nested space") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child", 3).errors.empty());
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/mount[2]"; // only one nested space exists

    auto result = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        opts);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("SerializeNodeData snapshots node data and POD payloads") {
    PathSpace space;

    // Case 1: Serialize NodeData-backed payload.
    REQUIRE(space.insert("/nodes/alpha", std::string{"alpha"}).errors.empty());
    ValueHandle nodeDataHandle;
    auto nodeDataVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/nodes/alpha") {
                nodeDataHandle = handle;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(nodeDataVisit);
    auto nodeDataBytes = VisitDetail::Access::SerializeNodeData(nodeDataHandle);
    REQUIRE(nodeDataBytes.has_value());
    auto nodeDataSnapshot = NodeData::deserializeSnapshot(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(nodeDataBytes->data()), nodeDataBytes->size()});
    REQUIRE(nodeDataSnapshot);
    std::string recovered{};
    InputMetadata strMeta{InputMetadataT<std::string>{}};
    auto nodeDataErr = nodeDataSnapshot->deserialize(&recovered, strMeta);
    CHECK_FALSE(nodeDataErr);
    CHECK(recovered == "alpha");

    // Case 2: Serialize POD fast-path payload.
    REQUIRE(space.insert("/pod", 11).errors.empty());
    REQUIRE(space.insert("/pod", 22).errors.empty());
    ValueHandle podHandle;
    auto podVisit = space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/pod") {
                podHandle = handle;
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        });
    REQUIRE(podVisit);
    auto podBytes = VisitDetail::Access::SerializeNodeData(podHandle);
    REQUIRE(podBytes.has_value());
    auto podSnapshot = NodeData::deserializeSnapshot(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(podBytes->data()), podBytes->size()});
    REQUIRE(podSnapshot);
    int frontValue = 0;
    InputMetadata intMeta{InputMetadataT<int>{}};
    auto podErr = podSnapshot->deserialize(&frontValue, intMeta);
    CHECK_FALSE(podErr);
    CHECK((frontValue == 11 || frontValue == 22));

    // Case 3: Invalid handle returns no snapshot.
    ValueHandle emptyHandle{};
    CHECK_FALSE(VisitDetail::Access::SerializeNodeData(emptyHandle).has_value());
}

TEST_SUITE_END();
