#include "PathSpaceNestingTestSupport.hpp"

namespace {

struct LockAwareSpace : PathSpace {
    using PathSpace::PathSpace;
    void setParentMutex(std::mutex* m) { parentMutex = m; }
    bool lastTryLockSucceeded() const { return lastTryLockSuccess; }
    auto listChildrenCanonical(std::string_view) const -> std::vector<std::string> override {
        if (parentMutex) {
            if (parentMutex->try_lock()) {
                lastTryLockSuccess = true;
                parentMutex->unlock();
            } else {
                lastTryLockSuccess = false;
            }
        }
        return {"child"};
    }

private:
    mutable bool     lastTryLockSuccess = false;
    std::mutex*      parentMutex        = nullptr;
};

} // namespace

TEST_SUITE_BEGIN("pathspace.nesting.basic");

TEST_CASE("PathSpace Nesting/Basic Nested PathSpace Operations") {
    PathSpace outerSpace;
    auto      innerSpace = std::make_unique<PathSpace>();

    // Add some data to inner space
    REQUIRE(innerSpace->insert("/test", 42).nbrValuesInserted == 1);
    REQUIRE(innerSpace->insert("/nested/value", "hello").nbrValuesInserted == 1);

    // Insert inner space into outer space
    REQUIRE(outerSpace.insert("/inner", std::move(innerSpace)).nbrSpacesInserted == 1);

    // Verify we can read values through the nested path
    auto result1 = outerSpace.read<int>("/inner/test", Block{});
    REQUIRE(result1.has_value());
    CHECK(result1.value() == 42);

    auto result2 = outerSpace.read<std::string>("/inner/nested/value", Block{});
    REQUIRE(result2.has_value());
    CHECK(result2.value() == "hello");
}

TEST_CASE("PathSpace Nesting/Take and reinsert nested PathSpace") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/payload", 123).nbrValuesInserted == 1);
    REQUIRE(root.insert("/watchlists/foo/space", std::move(nested)).nbrSpacesInserted == 1);

    auto extracted = root.take<std::unique_ptr<PathSpace>>("/watchlists/foo/space");
    REQUIRE(extracted.has_value());
    auto owned = std::move(*extracted);
    auto value = owned->read<int>("/payload", Block{});
    REQUIRE(value.has_value());
    CHECK(value.value() == 123);

    REQUIRE(root.insert("/trash/foo/space", std::move(owned)).nbrSpacesInserted == 1);
    auto movedValue = root.read<int>("/trash/foo/space/payload", Block{});
    REQUIRE(movedValue.has_value());
    CHECK(movedValue.value() == 123);
}

TEST_CASE("PathSpace Nesting/Deep Nesting") {
    PathSpace level1;
    auto      level2 = std::make_unique<PathSpace>();
    auto      level3 = std::make_unique<PathSpace>();

    // Add data at each level
    REQUIRE(level3->insert("/data", 100).nbrValuesInserted == 1);
    REQUIRE(level2->insert("/l3", std::move(level3)).nbrSpacesInserted == 1);
    REQUIRE(level1.insert("/l2", std::move(level2)).nbrSpacesInserted == 1);

    // Verify deep access
    auto result = level1.read<int>("/l2/l3/data", Block{});
    REQUIRE(result.has_value());
    CHECK(result.value() == 100);
}

TEST_CASE("PathSpace Nesting/Multiple Nested Spaces") {
    PathSpace root;
    auto      space1 = std::make_unique<PathSpace>();
    auto      space2 = std::make_unique<PathSpace>();

    // Add data to each space
    REQUIRE(space1->insert("/data", 1).nbrValuesInserted == 1);
    REQUIRE(space2->insert("/data", 2).nbrValuesInserted == 1);

    // Insert both spaces
    REQUIRE(root.insert("/space1", std::move(space1)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/space2", std::move(space2)).nbrSpacesInserted == 1);

    // Verify access to both spaces
    auto result1 = root.read<int>("/space1/data", Block{});
    REQUIRE(result1.has_value());
    CHECK(result1.value() == 1);

    auto result2 = root.read<int>("/space2/data", Block{});
    REQUIRE(result2.has_value());
    CHECK(result2.value() == 2);
}

TEST_CASE("PathSpace Nesting/listChildren isolates indexed nested space") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/nestedValue", 7).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/hostValue", 9).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    auto combined = root.read<Children>("/mount");
    REQUIRE(combined.has_value());
    CHECK(std::find(combined->names.begin(), combined->names.end(), "hostValue") != combined->names.end());
    CHECK(std::find(combined->names.begin(), combined->names.end(), "nestedValue") != combined->names.end());

    auto indexed = root.read<Children>("/mount[0]");
    REQUIRE(indexed.has_value());
    INFO("indexed: " << [&]() {
             std::string s;
             for (auto const& c : indexed->names) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(indexed->names.begin(), indexed->names.end(), "nestedValue") != indexed->names.end());
    CHECK(std::find(indexed->names.begin(), indexed->names.end(), "hostValue") == indexed->names.end());
}

TEST_CASE("PathSpace Nesting/Nested Space with Functions") {
    PathSpace root;
    auto      subspace = std::make_unique<PathSpace>();

    // Add a function to the subspace
    auto func = []() -> int { return 42; };
    REQUIRE(subspace->insert("/func", func, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);

    // Insert subspace
    REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

    // Execute function through nested path
    auto result = root.read<int>("/sub/func", Block{});
    CHECK_MESSAGE(result.has_value(),
                  "nested func read error code=" << static_cast<int>(result.error().code)
                                                 << " msg=" << result.error().message.value_or(""));
    if (result.has_value()) {
        CHECK(result.value() == 42);
    }

}

TEST_CASE("PathSpace Nesting/lazy nested task survives remount") {
    IntrospectablePathSpace root;
    auto                     subspace = std::make_unique<IntrospectablePathSpace>();

    auto func = []() -> int { return 42; };
    REQUIRE(subspace->insert("/func", func, In{.executionCategory = ExecutionCategory::Lazy}).nbrTasksInserted == 1);
    REQUIRE(root.insert("/sub", std::move(subspace)).nbrSpacesInserted == 1);

    auto newCtx = std::make_shared<PathSpaceContext>(PathSpaceTestHelper::executor(root));
    root.rehome(newCtx, "/mount");

    auto result = root.read<int>("/sub/func", Block{200ms});
    REQUIRE_MESSAGE(result.has_value(),
                    "remounted func read error code=" << static_cast<int>(result.error().code)
                                                      << " msg=" << result.error().message.value_or(""));
    if (result.has_value()) {
        CHECK(result.value() == 42);
    }
}

TEST_CASE("PathSpace Nesting/Multiple nested spaces at same path with indexing") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/v", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/v", 2).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto mountChildren = root.read<Children>("/mount");
    REQUIRE(mountChildren.has_value());
    std::string childrenStr;
    for (auto const& c : mountChildren->names) {
        if (!childrenStr.empty()) childrenStr.append(",");
        childrenStr.append(c);
    }
    INFO("mount children: " << childrenStr);
    CHECK(std::find(mountChildren->names.begin(), mountChildren->names.end(), "space") != mountChildren->names.end());

    auto frontRead = root.read<int>("/mount/space/v", Block{200ms});
    REQUIRE(frontRead.has_value());
    CHECK(frontRead.value() == 1);

    auto secondRead = root.read<int>("/mount/space[1]/v", Block{200ms});
    CHECK_MESSAGE(secondRead.has_value(),
                  "secondRead error code=" << static_cast<int>(secondRead.error().code)
                                           << " msg=" << secondRead.error().message.value_or(""));
    if (secondRead.has_value()) {
        CHECK(secondRead.value() == 2);
    }

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.includeNestedSpaces = true;
    root.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.emplace_back(entry.path);
        return VisitControl::Continue;
    }, opts);
    std::string visitedStr;
    for (auto const& p : visited) {
        if (!visitedStr.empty()) visitedStr.append(",");
        visitedStr.append(p);
    }
    INFO("visited paths size=" << visited.size() << " paths=" << visitedStr);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/v") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space[1]/v") != visited.end());

    auto takenFront = root.take<std::unique_ptr<PathSpace>>("/mount/space", Block{200ms});
    REQUIRE(takenFront.has_value());

    auto remainingFront = root.read<int>("/mount/space/v", Block{200ms});
    REQUIRE(remainingFront.has_value());
    CHECK(remainingFront.value() == 2);

    auto missing = root.read<int>("/mount/space[1]/v", Block{200ms});
    CHECK(!missing.has_value());
}

TEST_CASE("PathSpace Nesting/listChildren releases parent lock before nested traversal") {
    IntrospectablePathSpace root;
    auto      nested   = std::make_unique<LockAwareSpace>();
    auto*     nestedRaw = nested.get();

    REQUIRE(root.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    Node* mountNode = root.rootNodePtr()->getChild("mount");
    REQUIRE(mountNode != nullptr);
    Node* spaceNode = mountNode->getChild("space");
    REQUIRE(spaceNode != nullptr);
    nestedRaw->setParentMutex(&spaceNode->payloadMutex);

    auto children = root.read<Children>("/mount/space");
    REQUIRE(children.has_value());
    CHECK(std::find(children->names.begin(), children->names.end(), "child") != children->names.end());
    CHECK(nestedRaw->lastTryLockSucceeded());
}

TEST_SUITE_END();
