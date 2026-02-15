#define private public
#include "core/NodeData.hpp"
#undef private

#include "core/NotificationSink.hpp"
#include "task/Task.hpp"
#include "task/TaskPool.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "pathspace/PathSpace.hpp"
#include "third_party/doctest.h"

#include <stdexcept>
#include <thread>

using namespace SP;

namespace {
auto makeImmediateTask() -> std::shared_ptr<Task> {
    return Task::Create(std::weak_ptr<NotificationSink>{},
                        "/immediate",
                        [] { return 7; },
                        ExecutionCategory::Immediate);
}

auto throwingDeserialize(void*, SlidingBuffer const&) -> void {
    throw std::runtime_error("boom");
}
} // namespace

TEST_SUITE("core.nodedata.deserialize_edges") {

TEST_CASE("deserialize reports missing value size metadata") {
    int value = 5;
    InputData input{value};
    NodeData  node;
    REQUIRE_FALSE(node.serialize(input).has_value());

    node.valueSizes.clear();

    int out = 0;
    auto err = node.deserialize(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"Serialized payload missing value length metadata"});
}

TEST_CASE("deserialize reports missing value bytes when declared length is zero") {
    int value = 5;
    InputData input{value};
    NodeData  node;
    REQUIRE_FALSE(node.serialize(input).has_value());

    node.valueSizes.front() = 0;

    int out = 0;
    auto err = node.deserialize(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"Serialized payload missing value data"});
}

TEST_CASE("deserialize reports buffer shorter than declared length") {
    int value = 5;
    InputData input{value};
    NodeData  node;
    REQUIRE_FALSE(node.serialize(input).has_value());

    node.data.assignRaw({}, 0);

    int out = 0;
    auto err = node.deserialize(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"Serialized payload exceeds buffer"});
}

TEST_CASE("deserialize reports exceptions from deserializer") {
    int value = 5;
    InputData input{value};
    NodeData  node;
    REQUIRE_FALSE(node.serialize(input).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserialize = &throwingDeserialize;

    int out = 0;
    auto err = node.deserialize(&out, meta);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"boom"});
}

TEST_CASE("popType ignores empty type queues") {
    NodeData node;
    CHECK(node.types.empty());
    node.popType();
    CHECK(node.types.empty());
}

TEST_CASE("popType removes nested slots for unique pointer types") {
    NodeData node;
    node.appendNested(std::make_unique<PathSpace>());
    node.types.push_back(ElementType{&typeid(PathSpace), 1, DataCategory::UniquePtr});

    REQUIRE(node.nestedSlots.size() == 1);
    node.popType();
    CHECK(node.nestedSlots.empty());
    CHECK(node.types.empty());
}

TEST_CASE("append rejects invalid serialized buffer offsets") {
    NodeData target;
    NodeData source;
    source.data.storage.clear();
    source.data.frontOffset = 1;

    auto err = target.append(source);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
}

TEST_CASE("append merges matching type runs and value sizes") {
    NodeData left;
    NodeData right;

    REQUIRE_FALSE(left.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(right.serialize(InputData{2}).has_value());

    auto err = left.append(right);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(left.types.size() == 1);
    CHECK(left.types.front().elements == 2);
    CHECK(left.valueSizes.size() == 2);
}

TEST_CASE("append pushes new type runs when categories differ") {
    NodeData left;
    NodeData right;

    REQUIRE_FALSE(left.serialize(InputData{1}).has_value());
    float value = 3.0f;
    REQUIRE_FALSE(right.serialize(InputData{value}).has_value());

    auto err = left.append(right);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(left.types.size() == 2);
    CHECK(left.types.front().typeInfo == &typeid(int));
    CHECK(left.types.back().typeInfo == &typeid(float));
}

TEST_CASE("deserializePop removes completed tasks when futures are cleared") {
    InputMetadataT<int> meta{};
    NodeData            node;
    auto               task = makeImmediateTask();

    InputData input{nullptr, meta};
    input.task     = task;
    TaskPool pool(1);
    input.executor = &pool;

    auto err = node.serialize(input);
    REQUIRE_FALSE(err.has_value());

    while (!task->isCompleted()) {
        std::this_thread::yield();
    }

    node.futures.clear();

    int out = 0;
    auto popErr = node.deserializePop(&out, meta);
    REQUIRE_FALSE(popErr.has_value());
    CHECK(out == 7);
    CHECK(node.tasks.empty());
    CHECK(node.types.empty());
}

TEST_CASE("deserializePop removes futures when task completed and future not ready") {
    InputMetadataT<int> meta{};
    NodeData            node;
    TaskPool            pool(1);
    auto                task = makeImmediateTask();

    InputData input{nullptr, meta};
    input.task     = task;
    input.executor = &pool;

    auto err = node.serialize(input);
    REQUIRE_FALSE(err.has_value());

    while (!task->isCompleted()) {
        std::this_thread::yield();
    }

    auto other = makeImmediateTask();
    node.futures.clear();
    node.futures.push_back(Future::FromShared(other));

    int out = 0;
    auto popErr = node.deserializePop(&out, meta);
    REQUIRE_FALSE(popErr.has_value());
    CHECK(out == 7);
    CHECK(node.futures.empty());
    CHECK(node.tasks.empty());
    CHECK(node.types.empty());
}

} // TEST_SUITE
