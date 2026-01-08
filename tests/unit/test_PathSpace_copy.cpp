#include "PathSpace.hpp"
#include "core/NodeData.hpp"
#include "core/Error.hpp"
#include "core/ExecutionCategory.hpp"
#include "task/Future.hpp"

#include "third_party/doctest.h"

#include <chrono>

using namespace SP;

TEST_SUITE("pathspace.copy") {

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

TEST_CASE("clone retains values alongside execution payloads") {
    PathSpace source;
    REQUIRE(source.insert("/mixed", 11).nbrValuesInserted == 1);
    auto execInsert = source.insert("/mixed", [] { return 42; }, In{.executionCategory = ExecutionCategory::Lazy});
    REQUIRE(execInsert.nbrTasksInserted == 1);

    auto clone = source.clone();

    auto value = clone.read<int>("/mixed", Block{});
    REQUIRE(value.has_value());
    CHECK(value.value() == 11);
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

TEST_CASE("clone avoids tree mutex recursion deadlock") {
    using namespace std::chrono_literals;
    PathSpace source;
    auto nested = std::make_unique<PathSpace>();
    REQUIRE(source.insert("/ns", std::move(nested)).nbrSpacesInserted == 1);

    auto start  = std::chrono::steady_clock::now();
    auto clone  = source.clone();
    auto end    = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    CHECK(millis < 200ms);

    auto names = clone.listChildren("/");
    CHECK_FALSE(names.empty());
}

TEST_CASE("copy handles nested serialization failures gracefully") {
    PathSpace source;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/v", 11).nbrValuesInserted == 1);
    REQUIRE(source.insert("/ns", std::move(nested)).nbrSpacesInserted == 1);

    // Insert an execution payload at the same node to force snapshot fallback (tasks present)
    auto execInsert = source.insert("/ns", [] { return 7; }, In{.executionCategory = ExecutionCategory::Lazy});
    REQUIRE(execInsert.nbrTasksInserted == 1);

    struct HookReset {
        ~HookReset() { NodeDataTestHelper::setNestedSerializeHook({}); }
    } resetHook;

    NodeDataTestHelper::setNestedSerializeHook([]() -> std::optional<Error> {
        return Error{Error::Code::UnknownError, "forced nested serialization failure"};
    });

    PathSpace::CopyStats stats{};
    auto clone = source.clone(&stats);

    CHECK(stats.nestedSpacesCopied == 0);
    CHECK(stats.nestedSpacesSkipped >= 1);

    auto nestedRead = clone.read<int>("/ns/v", Block{});
    CHECK_FALSE(nestedRead.has_value());
}

TEST_CASE("copy skips nested when snapshot restore fails to attach") {
    PathSpace source;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/v", 5).nbrValuesInserted == 1);
    REQUIRE(source.insert("/node", std::move(nested)).nbrSpacesInserted == 1);
    REQUIRE(source.insert("/node", 17).nbrValuesInserted == 1); // value should survive clone

    struct HookReset {
        ~HookReset() { NodeDataTestHelper::setNestedSerializeHook({}); }
    } resetHook;

    NodeDataTestHelper::setNestedSerializeHook([]() -> std::optional<Error> {
        return Error{Error::Code::UnknownError, "forced nested attach failure"};
    });

    PathSpace::CopyStats stats{};
    PathSpace             clone = source.clone(&stats);

    CHECK(stats.nestedSpacesCopied == 0);
    CHECK(stats.nestedSpacesSkipped >= 1);

    auto nestedRead = clone.read<int>("/node/v", Block{});
    CHECK_FALSE(nestedRead.has_value());

    auto valueRead = clone.read<int>("/node", Block{});
    REQUIRE(valueRead.has_value());
    CHECK(valueRead.value() == 17);
}

} // TEST_SUITE
