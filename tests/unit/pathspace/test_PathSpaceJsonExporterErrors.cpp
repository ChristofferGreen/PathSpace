#include "PathSpaceJsonExporterTestSupport.hpp"

namespace {

class CorruptSnapshotSpace final : public PathSpaceBase {
public:
    explicit CorruptSnapshotSpace(NodeData data) {
        node.data = std::make_unique<NodeData>(std::move(data));
    }

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

        auto handle = VisitDetail::Access::MakeHandle(*this, node, entry.path, true);
        visitor(entry, handle);
        return {};
    }

private:
    Node node;

    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "CorruptSnapshotSpace does not support out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
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

class FailingVisitSpace final : public PathSpaceBase {
public:
    auto visit(PathVisitor const&, VisitOptions const& = {}) -> Expected<void> override {
        return std::unexpected(Error{Error::Code::NotSupported, "Visit failed"});
    }

private:
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "FailingVisitSpace does not support out"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};

} // namespace

TEST_SUITE_BEGIN("pathspace.json.errors");

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

TEST_CASE("PathSpace JSON exporter skips malformed serialized snapshots") {
    int value = 42;
    InputData input{value};
    NodeData  nodeData{input};

    auto rawSize = nodeData.data.rawSize();
    nodeData.data.frontOffset = rawSize + 1;

    CorruptSnapshotSpace space{std::move(nodeData)};

    PathSpaceJsonOptions options;
    options.mode       = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root = "/root";

    auto result = PathSpaceJsonExporter::Export(space, options);
    REQUIRE(result);

    auto doc  = Json::parse(*result);
    auto node = findNode(doc, "/root", "/root");
    CHECK(node.contains("values"));
}

TEST_CASE("PathSpace JSON exporter propagates visit errors") {
    FailingVisitSpace space;

    auto result = PathSpaceJsonExporter::Export(space, PathSpaceJsonOptions{});
    CHECK_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
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

TEST_SUITE_END();
