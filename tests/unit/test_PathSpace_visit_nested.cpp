#include "PathSpaceVisitTestSupport.hpp"

namespace {

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

TEST_SUITE_BEGIN("pathspace.visit.nested");

TEST_CASE("PathSpace visit maps nested roots and skips nested root entries") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/inner/value", 13).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.root = "/mount/inner";
    options.includeNestedSpaces = true;

    std::vector<std::string> paths;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            paths.push_back(entry.path);
            return VisitControl::Continue;
        },
        options);

    CHECK(result.has_value());
    CHECK(std::find(paths.begin(), paths.end(), "/mount/inner") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/mount/inner/value") != paths.end());
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

TEST_CASE("PathSpace visit resolves roots through nested spaces when child nodes are missing") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/missing/leaf", 5).nbrValuesInserted == 1);

    auto inserted = space.insert("/mount", std::move(nested));
    REQUIRE(inserted.nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/mount/missing";

    std::vector<std::string> visited;
    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(ok);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/missing") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/missing/leaf") != visited.end());

    VisitOptions disallow;
    disallow.includeNestedSpaces = false;
    disallow.root                = "/mount/missing";

    auto missing = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        disallow);
    CHECK_FALSE(missing);
    CHECK(missing.error().code == Error::Code::NoSuchPath);
}

TEST_CASE("PathSpace visit resolves missing roots through nested spaces at root") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/missing/leaf", 11).nbrValuesInserted == 1);
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto rootData = std::make_unique<NodeData>();
    rootData->appendNested(std::move(nested));
    {
        std::lock_guard<std::mutex> guard(root->payloadMutex);
        root->data = std::move(rootData);
    }

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/missing";

    std::vector<std::string> visited;
    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(ok);
    CHECK(std::find(visited.begin(), visited.end(), "/missing") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/missing/leaf") != visited.end());
}

TEST_CASE("PathSpace visit stops nested root traversal when visitor requests Stop") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/missing/leaf", 11).nbrValuesInserted == 1);
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto rootData = std::make_unique<NodeData>();
    rootData->appendNested(std::move(nested));
    {
        std::lock_guard<std::mutex> guard(root->payloadMutex);
        root->data = std::move(rootData);
    }

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/missing";

    std::vector<std::string> visited;
    auto ok = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            if (entry.path == "/missing/leaf") {
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(ok);
    CHECK(std::find(visited.begin(), visited.end(), "/missing/leaf") != visited.end());
}

TEST_CASE("PathSpace visit surfaces errors from nested roots") {
    PathSpace space;
    auto broken = std::make_unique<BrokenVisitSpace>();
    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto rootData = std::make_unique<NodeData>();
    rootData->appendNested(std::move(broken));
    {
        std::lock_guard<std::mutex> guard(root->payloadMutex);
        root->data = std::move(rootData);
    }

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/ghost";

    auto result = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        opts);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpace visit reports nested traversal errors for intermediate roots") {
    PathSpace space;
    auto broken = std::make_unique<BrokenVisitSpace>();
    REQUIRE(space.insert("/mount", std::move(broken)).nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.root                = "/mount/child";

    auto result = space.visit(
        [&](PathEntry const&, ValueHandle&) { return VisitControl::Continue; },
        opts);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
}

TEST_CASE("PathSpace visit stops nested traversal when visitor requests stop") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child/grand", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.root = "/mount/child";
    options.includeNestedSpaces = true;

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            if (entry.path == "/mount/child") {
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        options);

    REQUIRE(result);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/child") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount/child/grand") == visited.end());
}

TEST_CASE("PathSpace visit skips nested root entries when walking deeper roots") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child/grand", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.root = "/mount/child";
    options.includeNestedSpaces = true;

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        options);

    REQUIRE(result);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/child") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/") == visited.end());
}

TEST_CASE("PathSpace visit includes nested entries under root traversal") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child", 42).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    std::vector<std::string> visited;
    VisitOptions options;
    options.includeNestedSpaces = true;

    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            return VisitControl::Continue;
        },
        options);

    REQUIRE(result);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/child") != visited.end());
}

TEST_CASE("PathSpace visit remaps nested entries for deeper roots") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/child/grand", 3).nbrValuesInserted == 1);
    REQUIRE(nested->insert("/child/other", 4).nbrValuesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nested)).nbrSpacesInserted == 1);

    VisitOptions options;
    options.root = "/mount/child/grand";
    options.includeNestedSpaces = true;

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            if (entry.path == "/mount/child/grand") {
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        options);

    REQUIRE(result);
    CHECK(std::find(visited.begin(), visited.end(), "/mount/child/grand") != visited.end());
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

TEST_CASE("PathSpace visit stops nested traversal when visitor requests Stop in nested space") {
    PathSpace space;
    REQUIRE(space.insert("/zchild", 3).errors.empty());

    auto nestedA = std::make_unique<PathSpace>();
    REQUIRE(nestedA->insert("/stop", 1).errors.empty());
    auto nestedB = std::make_unique<PathSpace>();
    REQUIRE(nestedB->insert("/keep", 2).errors.empty());

    REQUIRE(space.insert("/mount", std::move(nestedA)).nbrSpacesInserted == 1);
    REQUIRE(space.insert("/mount", std::move(nestedB)).nbrSpacesInserted == 1);

    VisitOptions opts;
    opts.includeNestedSpaces = true;

    std::vector<std::string> visited;
    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            visited.push_back(entry.path);
            if (entry.path.find("stop") != std::string::npos) {
                return VisitControl::Stop;
            }
            return VisitControl::Continue;
        },
        opts);
    REQUIRE(result);

    CHECK(std::find(visited.begin(), visited.end(), "/mount/stop") != visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/zchild") == visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount[1]") == visited.end());
    CHECK(std::find(visited.begin(), visited.end(), "/mount[1]/keep") == visited.end());
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

TEST_CASE("PathSpace visit skips nested roots when maxDepth is exhausted at root") {
    PathSpace space;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/inside", 1).errors.empty());

    Node* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto rootData = std::make_unique<NodeData>();
    rootData->appendNested(std::move(nested));
    {
        std::lock_guard<std::mutex> guard(root->payloadMutex);
        root->data = std::move(rootData);
    }

    VisitOptions opts;
    opts.includeNestedSpaces = true;
    opts.maxDepth            = 0; // root only

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

TEST_SUITE_END();
