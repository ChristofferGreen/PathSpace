#define private public
#include "core/NodeData.hpp"
#undef private
#include "PathSpace.hpp"
#include "core/PodPayload.hpp"
#include "core/Error.hpp"
#include "core/ExecutionCategory.hpp"
#include "task/Future.hpp"
#include "PathSpaceTestHelper.hpp"

#include "third_party/doctest.h"

#include <chrono>

using namespace SP;

TEST_SUITE("pathspace.copy") {

namespace {
struct DummyExecutor final : Executor {
    auto submit(std::weak_ptr<Task>&&) -> std::optional<Error> override { return std::nullopt; }
    auto shutdown() -> void override {}
    [[nodiscard]] auto size() const -> size_t override { return 1; }
};

struct FailingPodPayload final : PodPayloadBase {
    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(int); }
    std::size_t size() const noexcept override { return 1; }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0; }
    std::size_t publishedTail() const noexcept override { return 1; }
    InputMetadata const& podMetadata() const noexcept override { return meta; }
    bool pushValue(void const*) override { return false; }
    std::optional<Error> readTo(void*) const override { return Error{Error::Code::NoObjectFound, "empty"}; }
    std::optional<Error> takeTo(void*) override { return Error{Error::Code::NoObjectFound, "empty"}; }
    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const&) const override {
        return Error{Error::Code::UnknownError, "forced span failure"};
    }
    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const&) override {
        return Error{Error::Code::UnknownError, "forced span failure"};
    }
    std::optional<Error> withSpanRawFrom(std::size_t,
                                         std::function<void(void const*, std::size_t)> const&) const override {
        return Error{Error::Code::UnknownError, "forced span failure"};
    }
    std::optional<Error> withSpanMutableRawFrom(std::size_t,
                                                std::function<void(void*, std::size_t)> const&) override {
        return Error{Error::Code::UnknownError, "forced span failure"};
    }
    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return false; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}

    InputMetadata meta{InputMetadataT<int>{}};
};

struct BadMetadataPodPayload final : PodPayloadBase {
    BadMetadataPodPayload() {
        meta = InputMetadata{InputMetadataT<int>{}};
        meta.serialize = nullptr;
    }
    std::type_info const& type() const noexcept override { return typeid(int); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(int); }
    std::size_t size() const noexcept override { return 1; }
    std::size_t elementSize() const noexcept override { return sizeof(int); }
    std::size_t headIndex() const noexcept override { return 0; }
    std::size_t publishedTail() const noexcept override { return 1; }
    InputMetadata const& podMetadata() const noexcept override { return meta; }
    bool pushValue(void const*) override { return false; }
    std::optional<Error> readTo(void*) const override { return Error{Error::Code::NoObjectFound, "empty"}; }
    std::optional<Error> takeTo(void*) override { return Error{Error::Code::NoObjectFound, "empty"}; }
    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const&) override {
        return std::nullopt;
    }
    std::optional<Error> withSpanRawFrom(std::size_t,
                                         std::function<void(void const*, std::size_t)> const& fn) const override {
        fn(&value, 1);
        return std::nullopt;
    }
    std::optional<Error> withSpanMutableRawFrom(std::size_t,
                                                std::function<void(void*, std::size_t)> const&) override {
        return std::nullopt;
    }
    std::optional<Error> popCount(std::size_t) override { return std::nullopt; }
    bool freezeForUpgrade() override { return false; }
    std::optional<Reservation> reserveOne() override { return std::nullopt; }
    void publishOne(std::size_t) override {}
    void rollbackOne(std::size_t) override {}
    std::optional<ReservationSpan> reserveSpan(std::size_t) override { return std::nullopt; }
    void publishSpan(std::size_t, std::size_t) override {}
    void rollbackSpan(std::size_t, std::size_t) override {}
    std::optional<std::size_t> packSpanStart() const noexcept override { return std::nullopt; }
    void markPackSpanStart(std::size_t) noexcept override {}

    InputMetadata meta{};
    int           value = 42;
};

struct DummyNestedSpace final : PathSpaceBase {
    auto in(Iterator const&, InputData const&) -> InsertReturn override { return {}; }
    auto out(Iterator const&, InputMetadata const&, Out const&, void*) -> std::optional<Error> override {
        return Error{Error::Code::NoObjectFound, "dummy nested space"};
    }
    auto shutdown() -> void override {}
    auto notify(std::string const&) -> void override {}
};
} // namespace

TEST_CASE("context executor not backed by TaskPool falls back to singleton pool") {
    DummyExecutor exec;
    auto context = std::make_shared<PathSpaceContext>(&exec);
    PathSpace space{context, "/mount"};

    CHECK(PathSpaceTestHelper::executor(space) == &exec);
    CHECK(PathSpaceTestHelper::pool(space) == &TaskPool::Instance());
}

TEST_CASE("copy constructor preserves prefix and values") {
    DummyExecutor exec;
    auto context = std::make_shared<PathSpaceContext>(&exec);
    PathSpace source{context, "/prefix"};
    REQUIRE(source.insert("/value", 5).nbrValuesInserted == 1);

    PathSpace copy{source};

    CHECK(PathSpaceTestHelper::prefix(copy) == PathSpaceTestHelper::prefix(source));
    auto value = copy.read<int>("/value");
    REQUIRE(value.has_value());
    CHECK(value.value() == 5);
}

TEST_CASE("copy assignment short-circuits self assignment") {
    PathSpace space;
    REQUIRE(space.insert("/value", 12).nbrValuesInserted == 1);

    space = space;

    auto value = space.read<int>("/value");
    REQUIRE(value.has_value());
    CHECK(value.value() == 12);
}

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

    auto names = clone.read<Children>("/");
    REQUIRE(names.has_value());
    CHECK_FALSE(names->names.empty());
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

TEST_CASE("copy skips pod payloads when span snapshot fails") {
    PathSpace source;
    auto* root = PathSpaceTestHelper::root(source);
    auto& node = root->getOrCreateChild("pod");
    node.podPayload = std::make_shared<FailingPodPayload>();

    PathSpace::CopyStats stats{};
    auto clone = source.clone(&stats);

    CHECK(stats.payloadsSkipped >= 1);
    auto value = clone.read<int>("/pod");
    CHECK_FALSE(value.has_value());
}

TEST_CASE("copy skips pod payloads with invalid metadata") {
    PathSpace source;
    auto* root = PathSpaceTestHelper::root(source);
    auto& node = root->getOrCreateChild("badmeta");
    node.podPayload = std::make_shared<BadMetadataPodPayload>();

    PathSpace::CopyStats stats{};
    auto clone = source.clone(&stats);

    CHECK(stats.payloadsSkipped >= 1);
    auto value = clone.read<int>("/badmeta");
    CHECK_FALSE(value.has_value());
}

TEST_CASE("copy handles missing snapshots with mixed nested slots") {
    PathSpace source;
    auto      nested = std::make_unique<PathSpace>();
    REQUIRE(nested->insert("/value", 9).nbrValuesInserted == 1);
    REQUIRE(source.insert("/ns", std::move(nested)).nbrSpacesInserted == 1);

    auto* root = PathSpaceTestHelper::root(source);
    REQUIRE(root != nullptr);
    auto* node = root->getChild("ns");
    REQUIRE(node != nullptr);
    REQUIRE(node->data);

    node->data->nestedSlots.push_back(nullptr);
    auto dummySlot = std::make_shared<NodeData::NestedSlot>();
    dummySlot->ptr = std::make_unique<DummyNestedSpace>();
    dummySlot->self = dummySlot;
    node->data->nestedSlots.push_back(dummySlot);

    node->data->types.clear();

    PathSpace::CopyStats stats{};
    auto clone = source.clone(&stats);

    CHECK(stats.payloadsSkipped >= 1);
    CHECK(stats.nestedSpacesCopied >= 1);
    CHECK(stats.nestedSpacesSkipped >= 2);

    auto value = clone.read<int>("/ns/value");
    REQUIRE(value.has_value());
    CHECK(value.value() == 9);
}

} // TEST_SUITE
