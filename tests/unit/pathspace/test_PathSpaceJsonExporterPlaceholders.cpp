#include "PathSpaceJsonExporterTestSupport.hpp"

namespace {

class DummyReader final : public detail::PathSpaceJsonValueReader {
private:
    auto popImpl(void*, InputMetadata const&) -> std::optional<Error> override {
        return Error{Error::Code::NotSupported, "DummyReader pop"};
    }
};

} // namespace

TEST_SUITE_BEGIN("pathspace.json.placeholders");

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
