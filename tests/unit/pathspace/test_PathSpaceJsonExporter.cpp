#define private public
#include "core/NodeData.hpp"
#undef private
#include "third_party/doctest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <pathspace/PathSpaceBase.hpp>
#include <pathspace/PathSpace.hpp>
#include <pathspace/tools/PathSpaceJsonExporter.hpp>
#include <pathspace/tools/PathSpaceJsonConverters.hpp>
#include <pathspace/type/DataCategory.hpp>

#include "../PathSpaceTestHelper.hpp"

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

class IntValueReader final : public detail::PathSpaceJsonValueReader {
public:
    explicit IntValueReader(int valueIn) : value(valueIn) {}

    int calls = 0;

private:
    auto popImpl(void* destination, InputMetadata const& metadata) -> std::optional<Error> override {
        ++calls;
        if (metadata.typeInfo != &typeid(int)) {
            return Error{Error::Code::TypeMismatch, "Expected int metadata"};
        }
        *static_cast<int*>(destination) = value;
        return std::nullopt;
    }

    int value = 0;
};

class DummyReader final : public detail::PathSpaceJsonValueReader {
private:
    auto popImpl(void*, InputMetadata const&) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "DummyReader pop"};
    }
};

class BrokenVisitSpace final : public PathSpaceBase {
public:
    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override {
        if (!visitor) {
            return std::unexpected(Error{Error::Code::InvalidType, "Visitor callback is empty"});
        }
        PathEntry entry;
        entry.path             = options.root.empty() ? std::string{"/"} : options.root;
        entry.hasValue         = true;
        entry.hasChildren      = false;
        entry.hasNestedSpace   = false;
        entry.approxChildCount = 0;
        entry.frontCategory    = DataCategory::Fundamental;

        ValueHandle handle;
        visitor(entry, handle);
        return {};
    }

private:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "BrokenVisitSpace does not support out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};

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

TEST_CASE("PathSpaceJsonValueReader pop forwards metadata and values") {
    IntValueReader reader{12};

    int out = 0;
    auto popFn = &detail::PathSpaceJsonValueReader::pop<int>;
    auto err = (reader.*popFn)(out);
    CHECK_FALSE(err.has_value());
    CHECK(out == 12);
    CHECK(reader.calls == 1);
}

TEST_CASE("JSON converter registry can register, convert, and describe types") {
    struct LocalType {
        int value = 0;
    };

    bool called = false;
    detail::RegisterPathSpaceJsonConverter(
        std::type_index(typeid(LocalType)),
        "LocalType",
        [&](detail::PathSpaceJsonValueReader&) -> std::optional<Json> {
            called = true;
            return Json("ok");
        });

    DummyReader reader;
    auto converted = detail::ConvertWithRegisteredConverter(std::type_index(typeid(LocalType)), reader);
    REQUIRE(converted.has_value());
    CHECK(converted->get<std::string>() == "ok");
    CHECK(called);

    auto missing = detail::ConvertWithRegisteredConverter(std::type_index(typeid(long double)), reader);
    CHECK_FALSE(missing.has_value());

    CHECK(detail::DescribeRegisteredType(std::type_index(typeid(LocalType))) == "LocalType");
    CHECK(detail::DescribeRegisteredType(std::type_index(typeid(long double))) == typeid(long double).name());
}

TEST_CASE("JSON::Export forwards to PathSpaceJsonExporter") {
    PathSpace space;
    REQUIRE(space.insert("/value", 3).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/";

    auto exportFn = &JSON::Export;
    auto result = exportFn(space, options);
    REQUIRE(result);
    auto doc = Json::parse(*result);
    CHECK(doc.contains("/"));
}

TEST_CASE("PathSpace JSON exporter flattens children capsule nodes") {
    PathSpace space;
    REQUIRE(space.insert("/root/children/alpha", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root/children/beta", 2).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/root";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/root", "/root");

    REQUIRE(rootNode.contains("children"));
    auto const& children = rootNode.at("children");
    CHECK(children.contains("alpha"));
    CHECK(children.contains("beta"));
    CHECK_FALSE(children.contains("children"));
}

TEST_CASE("PathSpace JSON exporter collapses duplicate children capsules in entries") {
    PathSpace space;
    REQUIRE(space.insert("/root/children/children/alpha", 1).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/root";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/root", "/root");

    REQUIRE(rootNode.contains("children"));
    auto const& children = rootNode.at("children");
    CHECK(children.contains("alpha"));
    CHECK_FALSE(children.contains("children"));
}

TEST_CASE("PathSpace JSON exporter reports snapshot errors as value_error") {
    BrokenVisitSpace space;

    PathSpaceJsonOptions options;
    options.mode       = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/root";

    auto result = PathSpaceJsonExporter::Export(space, options);
    REQUIRE(result);

    auto doc  = Json::parse(*result);
    auto node = findNode(doc, "/root", "/root");
    CHECK(node.contains("value_error"));
    CHECK(node.at("value_error") == "unknown_error:ValueHandle missing node");
}

TEST_CASE("PathSpace JSON exporter rejects duplicated children capsules in root") {
    PathSpace space;
    REQUIRE(space.insert("/root/children/children/alpha", 1).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.visit.root = "/root/children/children";

    auto result = space.toJSON(options);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidPath);
}

TEST_CASE("PathSpace JSON exporter drops empty housekeeping nodes") {
    PathSpace space;
    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    root->getOrCreateChild("space");
    root->getOrCreateChild("log");
    root->getOrCreateChild("metrics");
    root->getOrCreateChild("runtime");
    root->getOrCreateChild("keep");

    PathSpaceJsonOptions options;
    options.visit.root = "/";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/", "/");
    REQUIRE(rootNode.contains("children"));
    auto const& children = rootNode.at("children");
    CHECK_FALSE(children.contains("space"));
    CHECK_FALSE(children.contains("log"));
    CHECK_FALSE(children.contains("metrics"));
    CHECK_FALSE(children.contains("runtime"));
    CHECK(children.contains("keep"));
}

TEST_CASE("PathSpace JSON exporter preserves non-empty housekeeping nodes") {
    PathSpace space;
    REQUIRE(space.insert("/root/log/value", 1).errors.empty());

    PathSpaceJsonOptions options;
    options.visit.root = "/root";

    auto doc      = dump(space, options);
    auto rootNode = findNode(doc, "/root", "/root");
    REQUIRE(rootNode.contains("children"));
    auto const& children = rootNode.at("children");
    CHECK(children.contains("log"));

    auto logNode = findNode(doc, "/root", "/root/log");
    CHECK(logNode.contains("children"));
    CHECK(logNode.at("children").contains("value"));
}

TEST_CASE("PathSpace JSON exporter rejects invalid entry components") {
    PathSpace space;
    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);

    root->getOrCreateChild("*");

    PathSpaceJsonOptions options;
    options.visit.root = "/";

    auto result = space.toJSON(options);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("PathSpace JSON exporter emits placeholder for missing type info") {
    PathSpace space;
    REQUIRE(space.insert("/missing/type", std::string{"value"}).nbrValuesInserted == 1);

    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto* missing = root->getChild("missing");
    REQUIRE(missing != nullptr);
    auto* typeNode = missing->getChild("type");
    REQUIRE(typeNode != nullptr);

    {
        std::lock_guard<std::mutex> guard(typeNode->payloadMutex);
        REQUIRE(typeNode->data != nullptr);
        REQUIRE_FALSE(typeNode->data->types.empty());
        typeNode->data->types.front().typeInfo = nullptr;
    }

    PathSpaceJsonOptions options;
    options.mode = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/missing";

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/missing", "/missing/type");
    REQUIRE(node.at("values").size() == 1);
    auto entry = node.at("values")[0];
    CHECK(entry.at("type") == "unknown");
    CHECK(entry.at("reason") == "missing-type-info");
}

TEST_CASE("PathSpace JSON exporter maps unusual data categories") {
    PathSpace space;
    REQUIRE(space.insert("/weird/value", std::string{"data"}).nbrValuesInserted == 1);

    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto* weird = root->getChild("weird");
    REQUIRE(weird != nullptr);
    auto* valueNode = weird->getChild("value");
    REQUIRE(valueNode != nullptr);

    {
        std::lock_guard<std::mutex> guard(valueNode->payloadMutex);
        REQUIRE(valueNode->data != nullptr);
        REQUIRE_FALSE(valueNode->data->types.empty());
        valueNode->data->types.front().category = DataCategory::FunctionPointer;
    }

    PathSpaceJsonOptions options;
    options.mode = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/weird";

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/weird", "/weird/value");
    REQUIRE(node.at("values").size() == 1);
    CHECK(node.at("values")[0].at("category") == "FunctionPointer");

    {
        std::lock_guard<std::mutex> guard(valueNode->payloadMutex);
        valueNode->data->types.front().category = static_cast<DataCategory>(99);
    }

    auto doc2  = dump(space, options);
    auto node2 = findNode(doc2, "/weird", "/weird/value");
    REQUIRE(node2.at("values").size() == 1);
    CHECK(node2.at("values")[0].at("category") == "Unknown");
}

TEST_CASE("PathSpace JSON exporter handles corrupt serialized payloads") {
    PathSpace space;
    REQUIRE(space.insert("/corrupt/value", std::string{"data"}).nbrValuesInserted == 1);

    auto* root = PathSpaceTestHelper::root(space);
    REQUIRE(root != nullptr);
    auto* corrupt = root->getChild("corrupt");
    REQUIRE(corrupt != nullptr);
    auto* valueNode = corrupt->getChild("value");
    REQUIRE(valueNode != nullptr);

    {
        std::lock_guard<std::mutex> guard(valueNode->payloadMutex);
        REQUIRE(valueNode->data != nullptr);
        REQUIRE_FALSE(valueNode->data->valueSizes.empty());
        valueNode->data->valueSizes.front() = std::numeric_limits<std::size_t>::max();
    }

    PathSpaceJsonOptions options;
    options.mode = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/corrupt";

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/corrupt", "/corrupt/value");
    REQUIRE(node.at("values").size() == 1);
    auto entry = node.at("values")[0];
    CHECK(entry.at("placeholder") == "opaque");
    CHECK(entry.at("reason") == "converter-missing");
}

TEST_CASE("PathSpace JSON exporter rejects nested entries outside root") {
    PathSpace space;
    auto nested0 = std::make_unique<PathSpace>();
    REQUIRE(nested0->insert("/child", 1).nbrValuesInserted == 1);
    auto nested1 = std::make_unique<PathSpace>();
    REQUIRE(nested1->insert("/child", 2).nbrValuesInserted == 1);
    REQUIRE(space.insert("/root", std::move(nested0)).nbrSpacesInserted == 1);
    REQUIRE(space.insert("/root", std::move(nested1)).nbrSpacesInserted == 1);

    PathSpaceJsonOptions options;
    options.mode = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/root";
    options.visit.includeNestedSpaces = true;

    auto result = space.toJSON(options);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidPath);
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

TEST_CASE("Flat path export retains empty values when sampling is disabled") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 7).errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeStructureFields = true;
    opts.visit.includeValues    = false;
    opts.flatPaths              = true;
    opts.flatSimpleValues       = true;

    auto flat = space.toJSON(opts);
    REQUIRE(flat);
    auto doc = Json::parse(*flat);
    REQUIRE(doc.contains("/root/value"));
    CHECK(doc.at("/root/value").is_array());
    CHECK(doc.at("/root/value").empty());
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

struct CustomType {
    int value;
};

TEST_CASE("PathSpace JSON exporter emits opaque placeholder for missing converter") {
    PathSpace space;
    REQUIRE(space.insert("/opaque/value", CustomType{7}).errors.empty());

    PathSpaceJsonOptions options;
    options.mode                    = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root              = "/opaque";
    options.includeOpaquePlaceholders = false; // debug mode should override this to true

    auto doc   = dump(space, options);
    auto node  = findNode(doc, "/opaque", "/opaque/value");
    REQUIRE(node.at("values").size() == 1);
    auto entry = node.at("values")[0];
    CHECK(entry.at("placeholder") == "opaque");
    CHECK(entry.at("reason") == "converter-missing");
}

TEST_CASE("PathSpace JSON exporter omits opaque placeholders in minimal mode") {
    PathSpace space;
    REQUIRE(space.insert("/opaque/value", CustomType{3}).errors.empty());

    PathSpaceJsonOptions options;
    options.visit.root = "/opaque";

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/opaque", "/opaque/value");
    REQUIRE(node.at("values").size() == 1);
    auto entry = node.at("values")[0];
    CHECK_FALSE(entry.contains("placeholder"));
    CHECK_FALSE(entry.contains("value"));
 }

TEST_CASE("Flat path export preserves placeholder entries without values") {
    PathSpace space;
    REQUIRE(space.insert("/opaque/value", CustomType{11}).errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode             = PathSpaceJsonOptions::Mode::Debug;
    opts.flatPaths        = true;
    opts.flatSimpleValues = true;

    auto flat = space.toJSON(opts);
    REQUIRE(flat);
    auto doc = Json::parse(*flat);
    REQUIRE(doc.contains("/opaque/value"));

    auto entry = doc.at("/opaque/value");
    REQUIRE(entry.is_array());
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].at("placeholder") == "opaque");
    CHECK_FALSE(entry[0].contains("value"));
}

TEST_CASE("PathSpace JSON exporter rejects entries outside export root") {
    PathSpace space;
    REQUIRE(space.insert("/other/value", 1).errors.empty());

    PathSpaceJsonOptions options;
    options.visit.root = "/root"; // no matching entries in the space

    auto result = space.toJSON(options);
    CHECK_FALSE(result);
    auto code = result.error().code;
    CHECK((code == Error::Code::InvalidPath || code == Error::Code::NoSuchPath));
}

TEST_CASE("PathSpace JSON exporter rejects glob roots") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 1).errors.empty());

    PathSpaceJsonOptions options;
    options.visit.root = "/root/*";

    auto result = space.toJSON(options);
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidPathSubcomponent);
}

TEST_CASE("PathSpace JSON exporter flattens values when flatPaths are enabled") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 123).errors.empty());
    REQUIRE(space.insert("/root/list/item", std::string{"x"}).errors.empty());

    PathSpaceJsonOptions options;
    options.mode              = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root        = "/root";
    options.flatPaths         = true;
    options.flatSimpleValues  = true;

    auto flat = space.toJSON(options);
    REQUIRE(flat);
    auto json = Json::parse(*flat);
    REQUIRE(json.contains("/root/value"));
    CHECK(json.at("/root/value") == 123);
    REQUIRE(json.contains("/root/list/item"));
    CHECK(json.at("/root/list/item").is_string());
}

TEST_CASE("Flat path export preserves full entries when flatSimpleValues is false") {
    PathSpace space;
    REQUIRE(space.insert("/root/value", 42).errors.empty());

    PathSpaceJsonOptions options;
    options.mode             = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root       = "/root";
    options.flatPaths        = true;
    options.flatSimpleValues = false;

    auto flat = space.toJSON(options);
    REQUIRE(flat);
    auto json = Json::parse(*flat);
    REQUIRE(json.contains("/root/value"));
    auto entry = json.at("/root/value");
    REQUIRE(entry.is_array());
    REQUIRE(entry.size() == 1);
    CHECK(entry[0].at("value") == 42);
    CHECK(entry[0].contains("type"));
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

TEST_CASE("Minimal mode clears diagnostics even when requested") {
    PathSpace space;
    REQUIRE(space.insert("/alpha/value", 12).errors.empty());

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Minimal;
    options.includeDiagnostics     = true;
    options.includeStructureFields = true;
    options.includeMetadata        = true;

    auto doc  = dump(space, options);
    auto node = findNode(doc, "/", "/alpha/value");
    CHECK_FALSE(node.contains("diagnostics"));

    auto flags = doc.at("_meta").at("flags");
    CHECK_FALSE(flags.at("include_diagnostics").get<bool>());
}

TEST_CASE("Debug mode forces metadata and diagnostics flags") {
    PathSpace space;
    REQUIRE(space.insert("/debug/value", 4).errors.empty());

    PathSpaceJsonOptions options;
    options.mode                   = PathSpaceJsonOptions::Mode::Debug;
    options.includeMetadata         = false;
    options.includeDiagnostics      = false;
    options.includeStructureFields  = false;

    auto doc = dump(space, options);
    REQUIRE(doc.contains("_meta"));
    auto flags = doc.at("_meta").at("flags");
    CHECK(flags.at("include_metadata").get<bool>());
    CHECK(flags.at("include_diagnostics").get<bool>());
    CHECK(flags.at("include_structure").get<bool>());
}

TEST_CASE("JSON namespace Export matches direct exporter") {
    PathSpace space;
    REQUIRE(space.insert("/alias/value", 123).nbrValuesInserted == 1);

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeStructureFields = true;
    opts.visit.root             = "/";

    auto direct = PathSpaceJsonExporter::Export(space, opts);
    REQUIRE(direct);

    auto viaAlias = JSON::Export(space, opts);
    REQUIRE(viaAlias);

    CHECK(*viaAlias == *direct);

    auto doc = Json::parse(*viaAlias);
    auto node = findNode(doc, "/", "/alias/value");
    CHECK(node.at("values").size() == 1);
    CHECK(node.at("values")[0].at("value") == 123);
}

TEST_CASE("JSON namespace Export supports flat paths") {
    PathSpace space;
    REQUIRE(space.insert("/flat/one", 1).nbrValuesInserted == 1);
    REQUIRE(space.insert("/flat/two", 2).nbrValuesInserted == 1);

    PathSpaceJsonOptions opts;
    opts.flatPaths        = true;
    opts.flatSimpleValues = true;
    opts.visit.root       = "/flat";

    auto flat = JSON::Export(space, opts);
    REQUIRE(flat);

    auto doc = Json::parse(*flat);
    CHECK(doc.at("/flat/one") == 1);
    CHECK(doc.at("/flat/two") == 2);
    CHECK(doc.size() == 2);
}

TEST_CASE("Flat path export from root preserves leading slashes") {
    PathSpace space;
    REQUIRE(space.insert("/a", 1).errors.empty());
    REQUIRE(space.insert("/b/c", 2).errors.empty());

    PathSpaceJsonOptions opts;
    opts.flatPaths        = true;
    opts.flatSimpleValues = true;
    opts.visit.root       = "/";

    auto flat = space.toJSON(opts);
    REQUIRE(flat);
    auto doc = Json::parse(*flat);
    REQUIRE(doc.contains("/a"));
    REQUIRE(doc.contains("/b/c"));
    CHECK(doc.at("/a") == 1);
    CHECK(doc.at("/b/c") == 2);
    CHECK_FALSE(doc.contains("a"));
}

TEST_CASE("PathSpace JSON exporter emits placeholders for function pointers") {
    auto sampleFunction = +[]() -> int { return 21; };

    PathSpace space;
    auto ret = space.insert("/fn/pointer", sampleFunction);
    REQUIRE(ret.errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeStructureFields = true;

    auto doc  = dump(space, opts);
    auto node = findNode(doc, "/", "/fn/pointer");
    REQUIRE(node.at("values").size() == 1);

    auto entry = node.at("values")[0];
    INFO(entry.dump());
    CHECK(entry.at("category") == "Execution");
    CHECK(entry.at("placeholder") == "execution");
    CHECK(entry.at("state") == "pending");
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

TEST_CASE("Flat path export flattens multi-value queues") {
    PathSpace space;
    REQUIRE(space.insert("/queue/item", 1).errors.empty());
    REQUIRE(space.insert("/queue/item", 2).errors.empty());

    PathSpaceJsonOptions opts;
    opts.flatPaths        = true;
    opts.flatSimpleValues = true;
    opts.visit.root       = "/queue";

    auto flat = JSON::Export(space, opts);
    REQUIRE(flat);
    auto doc = Json::parse(*flat);

    auto values = doc.at("/queue/item");
    REQUIRE(values.is_array());
    REQUIRE(values.size() == 2);
    CHECK(values[0] == 1);
    CHECK(values[1] == 2);
}

TEST_CASE("JSON namespace alias forwards to PathSpaceJsonExporter::Export") {
    PathSpace space;
    REQUIRE(space.insert("/alias/value", 123).errors.empty());

    PathSpaceJsonOptions opts;
    opts.visit.root = "/alias";

    auto viaNamespace = JSON::Export(space, opts);
    REQUIRE(viaNamespace);

    auto viaClass = PathSpaceJsonExporter::Export(space, opts);
    REQUIRE(viaClass);

    CHECK(*viaNamespace == *viaClass);

    auto doc      = Json::parse(*viaNamespace);
    auto valueNode = findNode(doc, "/alias", "/alias/value");
    REQUIRE(valueNode.at("values").size() == 1);
    CHECK(valueNode.at("values")[0].at("value") == 123);
}

TEST_CASE("PathSpace JSON exporter emits opaque placeholder for PathSpace unique_ptr payloads") {
    PathSpace space;
    auto      nested = std::make_unique<PathSpace>();
    auto ret         = space.insert("/ptr/value", std::move(nested));
    REQUIRE(ret.errors.empty());

    PathSpaceJsonOptions opts;
    opts.mode                   = PathSpaceJsonOptions::Mode::Debug;
    opts.includeStructureFields = true;

    auto doc  = dump(space, opts);
    auto node = findNode(doc, "/", "/ptr/value");

    REQUIRE(node.at("values").size() == 1);
    auto entry = node.at("values")[0];
    CHECK(entry.at("placeholder") == "opaque");
    CHECK(entry.at("category") == "UniquePtr");
    CHECK(entry.at("reason") == "converter-missing");
}

TEST_SUITE_END();
