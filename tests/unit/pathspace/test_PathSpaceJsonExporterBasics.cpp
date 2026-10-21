#include "PathSpaceJsonExporterTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.json.basics");

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

TEST_SUITE_END();
