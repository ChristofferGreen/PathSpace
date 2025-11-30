#include "third_party/doctest.h"

#include <nlohmann/json.hpp>

#include <pathspace/PathSpace.hpp>

using Json = nlohmann::json;
using namespace SP;

namespace {

auto dump(PathSpace& space, PathSpaceJsonOptions options) -> Json {
    auto result = space.toJSON(options);
    REQUIRE(result);
    return Json::parse(*result);
}

auto findNode(Json const& doc, std::string const& path) -> Json {
    for (auto const& node : doc.at("nodes")) {
        if (node.at("path") == path) {
            return node;
        }
    }
    FAIL_CHECK("Node " << path << " not found");
    return Json::object();
}

} // namespace

TEST_CASE("PathSpace JSON exporter serializes primitive values") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/int", 42).nbrValuesInserted == 1);
    REQUIRE(space.insert("/alpha/name", std::string{"Ada"}).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/alpha";

    auto doc      = dump(space, options);
    auto intNode  = findNode(doc, "/alpha/int");
    auto nameNode = findNode(doc, "/alpha/name");

    REQUIRE(intNode.at("values").size() == 1);
    CHECK(intNode.at("values")[0].at("value") == 42);

    REQUIRE(nameNode.at("values").size() == 1);
    CHECK(nameNode.at("values")[0].at("value") == "Ada");
}

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
    fullOptions.maxQueueEntries = queueDepth == 0 ? 1 : queueDepth;

    auto fullDoc   = dump(space, fullOptions);
    auto fullNode  = findNode(fullDoc, "/queue");
    INFO("queue depth=" << queueDepth);
    CHECK(fullNode.at("values").size() == queueDepth);
    CHECK_FALSE(fullNode.at("values_truncated").get<bool>());

    PathSpaceJsonOptions truncatedOptions;
    truncatedOptions.maxQueueEntries = 0;
    auto truncatedDoc   = dump(space, truncatedOptions);
    auto truncatedNode  = findNode(truncatedDoc, "/queue");
    CHECK(truncatedNode.at("values").empty());
    CHECK(truncatedNode.at("values_truncated").get<bool>() == (queueDepth > 0));
}

TEST_CASE("PathSpace JSON exporter adds execution placeholders") {
    PathSpace space;
    auto result = space.insert("/jobs/task", [] { return 7; });
    REQUIRE(result.errors.empty());

    PathSpaceJsonOptions options;
    auto doc     = dump(space, options);
    auto jobNode = findNode(doc, "/jobs/task");

    REQUIRE(jobNode.at("values").size() == 1);
    auto entry = jobNode.at("values")[0];
    CHECK(entry.at("placeholder") == "execution");
}

TEST_CASE("PathSpace JSON exporter honors includeValues toggle") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 9).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.includeValues = false;

    auto doc    = dump(space, options);
    auto node   = findNode(doc, "/alpha/value");
    auto values = node.at("values");
    CHECK(values.empty());
    CHECK_FALSE(node.at("values_truncated").get<bool>());
    CHECK_FALSE(node.at("values_sampled").get<bool>());
}
