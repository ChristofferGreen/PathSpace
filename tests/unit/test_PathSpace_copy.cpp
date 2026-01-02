#include "PathSpace.hpp"
#include "core/Error.hpp"
#include "task/Future.hpp"

#include "third_party/doctest.h"

using namespace SP;

TEST_SUITE("PathSpaceCopy") {

TEST_CASE("copies_plain_values") {
    PathSpace source;
    auto inserted = source.insert("/a", 7);
    CHECK(inserted.nbrValuesInserted == 1);

    auto clone = source.clone();

    auto value = clone.read<int>("/a");
    REQUIRE(value);
    CHECK(*value == 7);

    // Source remains intact
    auto original = source.read<int>("/a");
    REQUIRE(original);
    CHECK(*original == 7);
}

TEST_CASE("skips_execution_payloads") {
    PathSpace source;
    auto inserted = source.insert("/exec", [] { return 3; });
    CHECK(inserted.nbrTasksInserted == 1);

    auto clone = source.clone();

    auto result = clone.read<int>("/exec");
    CHECK_FALSE(result);
    auto code = result.error().code;
    bool acceptable = (code == Error::Code::NoObjectFound)
                   || (code == Error::Code::NoSuchPath)
                   || (code == Error::Code::NotSupported);
    CHECK(acceptable);
}

TEST_CASE("copies_nested_space_structure") {
    PathSpace source;
    auto nested = std::make_unique<PathSpace>();
    nested->insert("/inner", 42);
    source.insert("/ns", std::move(nested));

    auto clone = source.clone();

    auto value = clone.read<int>("/ns/inner");
    REQUIRE(value);
    CHECK(*value == 42);
}

} // TEST_SUITE
