#include "PathSpaceNestingTestSupport.hpp"

namespace {

struct CountingPathSpace : PathSpace {
    using PathSpace::PathSpace;
    void resetCount() { adoptCalls.store(0); }
    int  count() const { return adoptCalls.load(); }
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        adoptCalls.fetch_add(1);
        PathSpace::adoptContextAndPrefix(std::move(context), std::move(prefix));
    }

private:
    std::atomic<int> adoptCalls{0};
};

} // namespace

TEST_SUITE_BEGIN("pathspace.nesting.lifecycle");

TEST_CASE("PathSpace Nesting/shutdown clears tree even when context is shared by nested space") {
    IntrospectablePathSpace root;
    REQUIRE(root.insert("/v", 1).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    // Create a second shared reference to the context via another nested insert
    REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);

    root.shutdown();
    auto children = root.read<Children>("/");
    REQUIRE(children.has_value());
    CHECK(children->names.empty());
}

TEST_CASE("PathSpace Nesting/listChildren merges multiple nested spaces") {
    PathSpace root;
    auto      first  = std::make_unique<PathSpace>();
    auto      second = std::make_unique<PathSpace>();

    REQUIRE(first->insert("/a", 1).nbrValuesInserted == 1);
    REQUIRE(second->insert("/b", 2).nbrValuesInserted == 1);

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto children = root.read<Children>("/mount/space");
    REQUIRE(children.has_value());
    std::sort(children->names.begin(), children->names.end());
    INFO("children: " << [&]() {
             std::string s;
             for (auto const& c : children->names) {
                 if (!s.empty()) s.append(",");
                 s.append(c);
             }
             return s;
         }());
    CHECK(std::find(children->names.begin(), children->names.end(), "a") != children->names.end());
    CHECK(std::find(children->names.begin(), children->names.end(), "b[1]") != children->names.end());
}

TEST_CASE("PathSpace Nesting/visit includes parent node when starting at nested path") {
    PathSpace root;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child", 7).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(nested)).nbrSpacesInserted == 1);

    std::vector<std::string> visited;
    VisitOptions opts;
    opts.root                = "/mount/space";
    opts.includeNestedSpaces = true;
    root.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.emplace_back(entry.path);
        return VisitControl::Continue;
    }, opts);
    INFO("visited paths: " << [&]() {
             std::string s;
             for (auto const& p : visited) {
                 if (!s.empty()) s.append(",");
                 s.append(p);
             }
             return s;
         }());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/space/child") != visited.end());
}

TEST_CASE("PathSpace Nesting/adoptContextAndPrefix remounts nested spaces with full path") {
    IntrospectablePathSpace root;
    auto level1 = std::make_unique<IntrospectablePathSpace>();
    auto level2 = std::make_unique<IntrospectablePathSpace>();
    auto level2Raw = level2.get();

    REQUIRE(level2->insert("/deep/value", 9).nbrValuesInserted == 1);
    REQUIRE(level1->insert("/nested", std::move(level2)).nbrSpacesInserted == 1);
    CHECK(level2Raw->prefixStr() == "/nested");
    REQUIRE(root.insert("/mount/space", std::move(level1)).nbrSpacesInserted == 1);

    INFO("prefix=" << level2Raw->prefixStr());
    CHECK(level2Raw->prefixStr() == "/mount/space/nested");
}

TEST_CASE("PathSpace Nesting/nested insert does not retarget unchanged nested spaces") {
    PathSpace root;
    auto first = std::make_unique<CountingPathSpace>();
    auto second = std::make_unique<CountingPathSpace>();
    auto* firstRaw  = first.get();
    auto* secondRaw = second.get();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    firstRaw->resetCount();

    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    CHECK(firstRaw->count() == 0);
    CHECK(secondRaw->count() >= 1);
}

TEST_CASE("PathSpace Nesting/destructor shuts down shared context even with nested spaces") {
    auto ctx = std::make_shared<PathSpaceContext>();
    {
        PathSpace root{ctx};
        REQUIRE(root.insert("/mount/space", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
        CHECK_FALSE(ctx->isShuttingDown());
    }
    CHECK(ctx->isShuttingDown());
}

TEST_CASE("PathSpace Nesting/clone preserves nested/value queue ordering at same node") {
    PathSpace root;
    REQUIRE(root.insert("/node", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/node", 5).nbrValuesInserted == 1);

    PathSpace clone = root.clone();

    auto taken = clone.take<std::unique_ptr<PathSpace>>("/node", Block{});
    REQUIRE(taken.has_value());

    auto value = clone.take<int>("/node", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "value take failed code=" << static_cast<int>(value.error().code));
    if (value.has_value()) {
        CHECK(value.value() == 5);
    }
}

TEST_CASE("PathSpace Nesting/remaining nested space retargets prefix after removal") {
    IntrospectablePathSpace root;
    auto first  = std::make_unique<IntrospectablePathSpace>();
    auto second = std::make_unique<IntrospectablePathSpace>();

    REQUIRE(root.insert("/mount/space", std::move(first)).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/mount/space", std::move(second)).nbrSpacesInserted == 1);

    auto taken = root.take<std::unique_ptr<PathSpace>>("/mount/space", Block{});
    REQUIRE(taken.has_value());

    auto ctx     = root.contextPtr();
    auto guard   = ctx->wait("/mount/space/value");
    auto deadline = std::chrono::system_clock::now() + 200ms;
    bool insertOk = false;

    std::thread writer([&]() {
        std::this_thread::sleep_for(10ms);
        auto insertResult = root.insert("/mount/space/value", 99);
        insertOk = insertResult.errors.empty();
    });

    auto status = guard.wait_until(deadline);
    writer.join();

    REQUIRE(insertOk);
    CHECK(status == std::cv_status::no_timeout);

    auto value = root.read<int>("/mount/space/value", Block{});
    REQUIRE(value.has_value());
    CHECK(value.value() == 99);
}

TEST_CASE("PathSpace Nesting/visit honors maxDepth for nested spaces") {
    PathSpace root;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/leaf", 7).nbrValuesInserted == 1);
    REQUIRE(root.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.root = "/mount";
    options.includeNestedSpaces = true;
    options.maxDepth = 0;

    std::vector<std::string> visited;
    auto result = root.visit([&](PathEntry const& entry, ValueHandle&) {
        visited.push_back(entry.path);
        return VisitControl::Continue;
    }, options);

    REQUIRE(result.has_value());
    CHECK(std::find(visited.begin(), visited.end(), "/mount") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/leaf") == visited.end());
}

TEST_CASE("PathSpace Nesting/json export keeps values after nested placeholder") {
    PathSpace root;
    REQUIRE(root.insert("/node", std::make_unique<PathSpace>()).nbrSpacesInserted == 1);
    REQUIRE(root.insert("/node", 5).nbrValuesInserted == 1);

    PathSpaceJsonExporter exporter;
    PathSpaceJsonOptions  opts;
    opts.flatPaths = true;
    auto exported = exporter.Export(root, opts);
    REQUIRE(exported);
    INFO(*exported);

    auto json = nlohmann::json::parse(*exported);
    REQUIRE(json.contains("/node"));
    auto values = json["/node"];
    bool found  = false;
    auto visitValue = [&](nlohmann::json const& v) {
        if (v.is_object() && v.contains("value") && v["value"].is_number_integer() && v["value"] == 5) {
            found = true;
        } else if (v.is_number_integer() && v == 5) {
            found = true;
        }
    };
    if (values.is_array()) {
        for (auto const& v : values) {
            visitValue(v);
        }
    } else {
        visitValue(values);
    }
    CHECK(found);
}

TEST_CASE("PathSpace Nesting/escaped bracket path is treated literally") {
    PathSpace root;
    auto insert = root.insert("/node\\[1\\]", 5);
    CHECK(insert.errors.empty());

    auto value = root.read<int>("/node\\[1\\]", Block{});
    CHECK_MESSAGE(value.has_value(),
                  "escaped bracket read failed code=" << static_cast<int>(value.error().code));
    if (value.has_value()) {
        CHECK(value.value() == 5);
    }
}

TEST_CASE("PathSpace Nesting/clear without context is safe") {
    PathSpace rootWithContext;
    REQUIRE(rootWithContext.insert("/v", 1).nbrValuesInserted == 1);

    PathSpace nullContextSpace{std::shared_ptr<PathSpaceContext>{}, "/pref"};
    CHECK_NOTHROW(nullContextSpace.clear());
}

TEST_CASE("PathSpace Nesting/context constructor retains executor/pool") {
    TaskPool customPool{1};
    auto     ctx = std::make_shared<PathSpaceContext>(&customPool);
    PathSpace space{ctx, "/pref"};

    CHECK(PathSpaceTestHelper::pool(space) == &customPool);
    CHECK(PathSpaceTestHelper::executor(space) == &customPool);

    PathSpace clone = space.clone();
    CHECK(PathSpaceTestHelper::pool(clone) == &customPool);
    CHECK(PathSpaceTestHelper::executor(clone) == &customPool);
}

TEST_CASE("PathSpace Nesting/copyNodeRecursive clones nested ordering with placeholders") {
    PathSpace source;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/v", 7).nbrValuesInserted == 1);
    REQUIRE(source.insert("/node", std::move(nested)).nbrSpacesInserted == 1);
    REQUIRE(source.insert("/node", 5).nbrValuesInserted == 1);

    PathSpace dest{source.sharedContext(), PathSpaceTestHelper::prefix(source)};
    PathSpace::CopyStats stats{};
    PathSpaceTestHelper::copyNode(source, dest, source.sharedContext(), PathSpaceTestHelper::prefix(source), "/", stats);

    auto value = dest.read<int>("/node", Block{});
    CHECK(value.has_value());
    CHECK(value.value() == 5);

    auto nestedVal = dest.read<int>("/node/v", Block{});
    CHECK(nestedVal.has_value());
    CHECK(nestedVal.value() == 7);
    CHECK(stats.nestedSpacesCopied == 1);
}

TEST_SUITE_END();
