#include "PathSpaceJsonExporterTestSupport.hpp"

namespace {

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

} // namespace

TEST_SUITE_BEGIN("pathspace.json.modes");

TEST_CASE("PathSpaceJsonValueReader pop forwards metadata and values") {
    IntValueReader reader{12};

    int out = 0;
    auto popFn = &detail::PathSpaceJsonValueReader::pop<int>;
    auto err = (reader.*popFn)(out);
    CHECK_FALSE(err.has_value());
    CHECK(out == 12);
    CHECK(reader.calls == 1);
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

TEST_CASE("PathSpace JSON exporter records nested and value flags in metadata") {
    PathSpace space;
    REQUIRE(space.insert("/flags/value", 1).nbrValuesInserted == 1);

    PathSpaceJsonOptions options;
    options.includeMetadata         = true;
    options.visit.root              = "/flags";
    options.visit.includeNestedSpaces = true;
    options.visit.includeValues     = false;

    auto doc = dump(space, options);
    auto flags = doc.at("_meta").at("flags");
    CHECK(flags.at("include_nested_spaces").get<bool>());
    CHECK_FALSE(flags.at("include_values").get<bool>());
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

TEST_SUITE_END();
