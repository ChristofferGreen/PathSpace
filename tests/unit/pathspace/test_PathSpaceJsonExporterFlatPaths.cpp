#include "PathSpaceJsonExporterTestSupport.hpp"

TEST_SUITE_BEGIN("pathspace.json.flat_paths");

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

TEST_CASE("Flat path export keeps mixed placeholders when simple flattening is enabled") {
    struct MixedType {
        int value;
    };

    PathSpace space;
    REQUIRE(space.insert("/mix/values", 2).errors.empty());
    REQUIRE(space.insert("/mix/values", MixedType{1}).errors.empty());

    PathSpaceJsonOptions options;
    options.mode             = PathSpaceJsonOptions::Mode::Debug;
    options.visit.root       = "/mix";
    options.flatPaths        = true;
    options.flatSimpleValues = true;

    auto flat = space.toJSON(options);
    REQUIRE(flat);
    auto json = Json::parse(*flat);
    REQUIRE(json.contains("/mix/values"));
    auto entry = json.at("/mix/values");
    REQUIRE(entry.is_array());
    REQUIRE(entry.size() == 2);
    bool sawPlaceholder = false;
    bool sawValue = false;
    for (auto const& valueEntry : entry) {
        if (valueEntry.contains("placeholder")) {
            CHECK(valueEntry.at("placeholder") == "opaque");
            CHECK_FALSE(valueEntry.contains("value"));
            sawPlaceholder = true;
        }
        if (valueEntry.contains("value")) {
            CHECK(valueEntry.at("value") == 2);
            sawValue = true;
        }
    }
    CHECK(sawPlaceholder);
    CHECK(sawValue);
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

TEST_SUITE_END();
