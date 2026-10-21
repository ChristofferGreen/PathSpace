#include "PathSpaceJsonExporterTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.json.limits");

TEST_CASE("PathSpace JSON exporter enforces queue limits") {
    PathSpace space;
    REQUIRE(space.insert("/queue", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/queue", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/queue", 3).nbrValuesInserted == 1);

    std::size_t queueDepth = 0;
    space.visit(
        [&](PathEntry const& entry, ValueHandle& handle) {
            if (entry.path == "/queue") {
                auto snapshot = handle.snapshot();
                REQUIRE(snapshot);
                queueDepth = snapshot->queueDepth;
            }
            return VisitControl::Continue;
        },
        VisitOptions{.root = "/queue"});
    INFO("queue depth=" << queueDepth);

    PathSpaceJsonOptions fullOptions;
    fullOptions.mode                 = PathSpaceJsonOptions::Mode::Debug;
    fullOptions.includeStructureFields = true;
    fullOptions.maxQueueEntries = queueDepth == 0 ? 1 : queueDepth;

    auto fullDoc   = dump(space, fullOptions);
    auto fullNode  = findNode(fullDoc, "/", "/queue");
    INFO("queue depth=" << queueDepth);
    CHECK(fullNode.at("values").size() == queueDepth);
    CHECK_FALSE(fullNode.at("values_truncated").get<bool>());

    PathSpaceJsonOptions truncatedOptions;
    truncatedOptions.mode                 = PathSpaceJsonOptions::Mode::Debug;
    truncatedOptions.includeStructureFields = true;
    truncatedOptions.maxQueueEntries = 0;
    auto truncatedDoc   = dump(space, truncatedOptions);
    auto truncatedNode  = findNode(truncatedDoc, "/", "/queue");
    CHECK(truncatedNode.at("values").empty());
    CHECK(truncatedNode.at("values_truncated").get<bool>() == (queueDepth > 0));
}

TEST_CASE("PathSpace JSON exporter honors explicit maxDepth truncation") {
    PathSpace space;
    REQUIRE(space.insert("/root/child/value", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/child/grand/value", 2).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root             = "/root";
    options.visit.maxDepth         = 1;

    auto doc       = dump(space, options);
    auto meta      = doc.at("_meta").at("limits");
    CHECK(meta.at("max_depth") == 1);

    auto childNode = findNode(doc, "/root", "/root/child");
    CHECK(childNode.at("children_truncated").get<bool>());
    CHECK(childNode.at("depth_truncated").get<bool>());
    bool hasGrandChild = childNode.contains("children")
                      && childNode.at("children").contains("grand");
    CHECK_FALSE(hasGrandChild);
}

TEST_CASE("PathSpace JSON exporter honors includeValues toggle") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 9).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                 = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.includeValues = false;

    auto doc    = dump(space, options);
    auto node   = findNode(doc, "/", "/alpha/value");
    auto it = node.find("values");
    if (it != node.end()) {
        CHECK(it->empty());
    }
    CHECK_FALSE(node.at("values_truncated").get<bool>());
    CHECK_FALSE(node.at("values_sampled").get<bool>());
}

TEST_CASE("PathSpace JSON exporter reports sampling fields for nodes without values") {
    PathSpace space;
    REQUIRE(space.insert("/root/child", 1).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root             = "/root";

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/root", "/root");

    CHECK_FALSE(node.at("has_value").get<bool>());
    CHECK(node.at("values").empty());
    CHECK_FALSE(node.at("values_truncated").get<bool>());
    CHECK(node.at("values_sampled").get<bool>());
}

TEST_CASE("PathSpace JSON exporter reports truncation when sampling disabled and maxQueueEntries is zero") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 5).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.includeValues    = false;
    options.maxQueueEntries        = 0;

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/", "/alpha/value");
    CHECK(node.at("values").empty());
    CHECK(node.at("values_truncated").get<bool>());
    CHECK_FALSE(node.at("values_sampled").get<bool>());
}

TEST_CASE("PathSpace JSON exporter reports truncation when maxQueueEntries is zero and values are sampled") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 8).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.includeValues    = true;
    options.maxQueueEntries        = 0;

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/", "/alpha/value");
    CHECK(node.at("values").empty());
    CHECK(node.at("values_truncated").get<bool>());
    CHECK(node.at("values_sampled").get<bool>());
}

TEST_CASE("PathSpace JSON exporter stats track child and value truncation") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", 1).errors.empty());
    REQUIRE(space.insert("/root/a", 2).errors.empty());
    REQUIRE(space.insert("/root/b", 3).errors.empty());
    REQUIRE(space.insert("/root/b", 4).errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeMetadata         = true;
    opts.includeStructureFields = true;
    opts.visit.root             = "/root";
    opts.visit.maxChildren       = 1;
    opts.maxQueueEntries         = 1;

    auto jsonStr = space.toJSON(opts);
    REQUIRE(jsonStr);
    auto doc = Json::parse(*jsonStr);

    auto stats = doc.at("_meta").at("stats");
    CHECK(stats.at("node_count") == 2);
    CHECK(stats.at("values_exported") == 1);
    CHECK(stats.at("children_truncated") == 1);
    CHECK(stats.at("values_truncated") == 1);
    CHECK(stats.at("depth_limited") == 0);
}

TEST_CASE("PathSpace JSON exporter stats report depth limits") {
    PathSpace space;
    REQUIRE(space.insert("/root/child/grand", 1).errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeMetadata         = true;
    opts.includeStructureFields = true;
    opts.visit.root             = "/root";
    opts.visit.maxDepth         = 0;

    auto jsonStr = space.toJSON(opts);
    REQUIRE(jsonStr);
    auto doc = Json::parse(*jsonStr);

    auto stats = doc.at("_meta").at("stats");
    CHECK(stats.at("node_count") == 1);
    CHECK(stats.at("depth_limited") == 1);
    CHECK(stats.at("children_truncated") == 1);
    CHECK(stats.at("values_exported") == 0);
    CHECK(stats.at("values_truncated") == 0);
}

TEST_CASE("PathSpace JSON exporter reports unlimited child limit") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/b", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/c", 3).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                 = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root        = "/root";
    options.visit.maxChildren = VisitOptions::UnlimitedChildren;

    auto doc        = dump(space, options);
    auto limits     = doc.at("_meta").at("limits");
    CHECK(limits.at("max_children") == "unlimited");

    auto rootNode = findNode(doc, "/root", "/root");
    CHECK_FALSE(rootNode.at("children_truncated").get<bool>());
}

TEST_CASE("PathSpace JSON exporter reports unlimited depth") {
    PathSpace space;
    auto ins1 = space.insert("/root/a", 1);
    REQUIRE(ins1.errors.empty());
    auto ins2 = space.insert("/root/a/b", 2);
    REQUIRE(ins2.errors.empty());

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root      = "/root";
    options.visit.maxDepth  = VisitOptions::UnlimitedDepth;

    auto doc    = dump(space, options);
    auto limits = doc.at("_meta").at("limits");
    CHECK(limits.at("max_depth") == "unlimited");

    auto node = findNode(doc, "/root", "/root/a/b");
    CHECK(node.at("values").size() == 1);
    CHECK_FALSE(node.at("depth_truncated").get<bool>());
}

TEST_CASE("PathSpace JSON exporter reports enabled child limits and unlimited queue entries") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", 1).errors.empty());
    REQUIRE(space.insert("/root/b", 2).errors.empty());
    REQUIRE(space.insert("/root/c", 3).errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                    = PathSpaceJsonOptions::Mode::Debug;
    opts.includeStructureFields  = true;
    opts.includeMetadata         = true;
    opts.visit.root              = "/root";
    opts.visit.maxChildren       = 1; // enable child limit branch
    opts.maxQueueEntries         = std::numeric_limits<std::size_t>::max();
    opts.dumpIndent              = -1; // exercise compact dump branch

    auto jsonStr = space.toJSON(opts);
    REQUIRE(jsonStr);
    auto doc = Json::parse(*jsonStr);

    auto meta = doc.at("_meta").at("limits");
    CHECK(meta.at("max_children") == 1);
    CHECK(meta.at("max_queue_entries") == "unlimited");

    auto rootNode = findNode(doc, "/root", "/root");
    CHECK(rootNode.at("children_truncated").get<bool>());
}

TEST_SUITE_END();
