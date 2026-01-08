#include "third_party/doctest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <pathspace/PathSpace.hpp>
#include <pathspace/tools/PathSpaceJsonConverters.hpp>

using Json = nlohmann::json;
using namespace SP;

namespace {

auto dump(PathSpace& space, PathSpaceJsonOptions options) -> Json {
    auto result = space.toJSON(options);
    REQUIRE(result);
    return Json::parse(*result);
}

auto split_path(std::string const& path) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < path.size() && path[pos] == '/') {
        ++pos;
    }
    while (pos < path.size()) {
        auto next = path.find('/', pos);
        auto end  = next == std::string::npos ? path.size() : next;
        parts.emplace_back(path.substr(pos, end - pos));
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return parts;
}

auto findNode(Json const& doc, std::string const& rootPath, std::string const& path) -> Json {
    REQUIRE(doc.contains(rootPath));
    auto rootParts   = split_path(rootPath);
    auto targetParts = split_path(path);
    REQUIRE(targetParts.size() >= rootParts.size());
    REQUIRE(std::equal(rootParts.begin(), rootParts.end(), targetParts.begin()));

    Json node = doc.at(rootPath);
    for (std::size_t idx = rootParts.size(); idx < targetParts.size(); ++idx) {
        node = node.at("children").at(targetParts[idx]);
    }
    return node;
}

} // namespace

TEST_SUITE_BEGIN("pathspace.json");

TEST_CASE("PathSpace JSON exporter serializes primitive values (minimal)") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/int", 42).nbrValuesInserted == 1);
    REQUIRE(space.insert("/alpha/name", std::string{"Ada"}).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/alpha";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/alpha", "/alpha");
    CHECK_FALSE(rootNode.contains("child_count"));
    CHECK(rootNode.at("children").contains("int"));
    CHECK(rootNode.at("children").contains("name"));

    auto intNode  = findNode(doc, "/alpha", "/alpha/int");
    auto nameNode = findNode(doc, "/alpha", "/alpha/name");

    REQUIRE(intNode.at("values").size() == 1);
    CHECK(intNode.at("values")[0].at("value") == 42);

    REQUIRE(nameNode.at("values").size() == 1);
    CHECK(nameNode.at("values")[0].at("value") == "Ada");
}

TEST_CASE("PathSpace JSON exporter exposes structure and diagnostics in debug mode") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/int", 1).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root             = "/alpha";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/alpha", "/alpha");
    CHECK(rootNode.at("child_count") == 1);
    CHECK(rootNode.at("children").contains("int"));
    auto valueNode = findNode(doc, "/alpha", "/alpha/int");
    CHECK(valueNode.at("values").size() == 1);
    CHECK(valueNode.contains("values_truncated"));
    CHECK(valueNode.contains("values_sampled"));
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

TEST_CASE("PathSpace JSON exporter adds execution placeholders") {
    PathSpace space;
    auto result = space.insert("/jobs/task", [] { return 7; });
    REQUIRE(result.errors.empty());

    PathSpaceJsonOptions options;
    options.mode                 = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    auto doc     = dump(space, options);
    auto jobNode = findNode(doc, "/", "/jobs/task");

    REQUIRE(jobNode.at("values").size() == 1);
    auto entry = jobNode.at("values")[0];
    CHECK(entry.at("placeholder") == "execution");
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

TEST_CASE("PathSpace JSON exporter reports unlimited child limit") {
    PathSpace space;
    REQUIRE(space.insert("/root/a", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/b", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/c", 3).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                 = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    options.visit.root        = "/root";
    options.visit.maxChildren = VisitOptions::kUnlimitedChildren;

    auto doc        = dump(space, options);
    auto limits     = doc.at("_meta").at("limits");
    CHECK(limits.at("max_children") == "unlimited");

    auto rootNode = findNode(doc, "/root", "/root");
    CHECK_FALSE(rootNode.at("children_truncated").get<bool>());
}

TEST_CASE("PathSpace JSON exporter honors friendly converter aliases") {
    struct FriendlyStruct {
        int a = 0;
        int b = 0;
    };

    PathSpaceJsonRegisterConverterAs<FriendlyStruct>(
        "FriendlyStruct",
        [](FriendlyStruct const& payload) {
            return Json{{"a", payload.a}, {"b", payload.b}};
        });

    PathSpace space;
    FriendlyStruct payload{.a = 7, .b = 9};
    REQUIRE(space.insert("/custom/value", payload).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode                 = PathSpaceJsonOptions::Mode::Debug;
    options.includeStructureFields = true;
    auto doc    = dump(space, options);
    auto node   = findNode(doc, "/", "/custom/value");
    auto values = node.at("values");
    REQUIRE(values.size() == 1);
    auto entry = values[0];
    CHECK(entry.at("type") == "FriendlyStruct");
    CHECK(entry.at("value").at("a") == 7);
    CHECK(entry.at("value").at("b") == 9);
}

TEST_CASE("PathSpace JSON exporter metadata is opt-in") {
    PathSpace space;
    REQUIRE(space.insert("/meta/value", 7).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/";

    auto minimalDoc = dump(space, options);
    CHECK_FALSE(minimalDoc.contains("_meta"));

    options.includeMetadata = true;
    auto metaDoc            = dump(space, options);
    REQUIRE(metaDoc.contains("_meta"));
    auto meta = metaDoc.at("_meta");
    CHECK(meta.at("root") == "/");
    CHECK(meta.at("flags").at("include_metadata").get<bool>());
}

TEST_SUITE_END();
