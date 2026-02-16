#define private public
#include "core/NodeData.hpp"
#include "task/Task.hpp"
#undef private

#include "core/NotificationSink.hpp"
#include "task/TaskPool.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "pathspace/PathSpace.hpp"
#include "third_party/doctest.h"

#include <array>
#include <functional>
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

auto throwingDeserializePop(void*, SlidingBuffer&) -> void {
    throw std::runtime_error("boom-pop");
}

NodeData* gCorruptNode = nullptr;

auto corruptingDeserializePop(void* obj, SlidingBuffer& buffer) -> void {
    if (auto pop = InputMetadataT<int>::deserializePop) {
        pop(obj, buffer);
    }
    if (gCorruptNode) {
        gCorruptNode->valueSizes.clear();
    }
}

struct NestedHookGuard {
    explicit NestedHookGuard(std::function<std::optional<Error>()> hook) {
        NodeDataTestHelper::setNestedSerializeHook(std::move(hook));
    }

    ~NestedHookGuard() {
        NodeDataTestHelper::setNestedSerializeHook({});
    }
};
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

TEST_CASE("serialize surfaces nested serialize hook errors") {
    NestedHookGuard guard([]() {
        return Error{Error::Code::InvalidType, "hook"};
    });

    NodeData node;
    std::unique_ptr<PathSpaceBase> space = std::make_unique<PathSpace>();
    InputData input{std::move(space)};

    auto err = node.serialize(input);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::InvalidType);
}

TEST_CASE("serialize accepts nested payload when hook passes") {
    NestedHookGuard guard([]() {
        return std::nullopt;
    });

    NodeData node;
    std::unique_ptr<PathSpaceBase> space = std::make_unique<PathSpace>();
    InputData input{std::move(space)};

    auto err = node.serialize(input);
    REQUIRE_FALSE(err.has_value());
    CHECK(node.nestedCount() == 1);
}

TEST_CASE("deserializeIndexed reports missing deserializer") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserialize = nullptr;

    int out = 0;
    auto err = node.deserializeIndexed(1, meta, false, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnserializableType);
    CHECK(err->message == std::optional<std::string>{"No deserialization function provided"});
}

TEST_CASE("deserializeIndexed reports missing pop deserializer") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserializePop = nullptr;

    int out = 0;
    auto err = node.deserializeIndexed(1, meta, true, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::UnserializableType);
    CHECK(err->message == std::optional<std::string>{"No pop deserialization function provided"});
}

TEST_CASE("deserializeIndexed reports exceptions from deserializer") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserialize = &throwingDeserialize;

    int out = 0;
    auto err = node.deserializeIndexed(1, meta, false, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"boom"});
}

TEST_CASE("deserializeIndexed reports exceptions from pop deserializer") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserializePop = &throwingDeserializePop;

    int out = 0;
    auto err = node.deserializeIndexed(1, meta, true, &out);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"boom-pop"});
}

TEST_CASE("deserializeIndexed reports missing length metadata after pop mutation") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    InputMetadata meta{InputMetadataT<int>{}};
    meta.deserializePop = &corruptingDeserializePop;
    gCorruptNode = &node;

    int out = 0;
    auto err = node.deserializeIndexed(1, meta, true, &out);

    gCorruptNode = nullptr;

    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
    CHECK(err->message == std::optional<std::string>{"Serialized payload missing value length metadata"});
}

TEST_CASE("deserializeIndexed reads second value without popping") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    REQUIRE_FALSE(node.serialize(InputData{2}).has_value());

    int out = 0;
    auto err = node.deserializeIndexed(1, InputMetadataT<int>{}, false, &out);
    REQUIRE_FALSE(err.has_value());
    CHECK(out == 2);
    CHECK(node.valueSizes.size() == 2);
}

TEST_CASE("deserializeIndexed pops a later type run") {
    NodeData node;
    REQUIRE_FALSE(node.serialize(InputData{1}).has_value());
    float value = 3.0f;
    REQUIRE_FALSE(node.serialize(InputData{value}).has_value());

    float out = 0.0f;
    auto err = node.deserializeIndexed(1, InputMetadataT<float>{}, true, &out);
    REQUIRE_FALSE(err.has_value());
    CHECK(out == doctest::Approx(3.0f));
    CHECK(node.types.size() == 1);
    CHECK(node.valueSizes.size() == 1);
}

TEST_CASE("popFrontSerialized reports empty queues") {
    NodeData node;
    NodeData dest;

    auto err = node.popFrontSerialized(dest);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("popFrontSerialized reports missing value sizes") {
    NodeData node;
    NodeData dest;
    node.types.push_back(ElementType{&typeid(int), 1, DataCategory::Fundamental});

    auto err = node.popFrontSerialized(dest);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
}

TEST_CASE("popFrontSerialized reports payload larger than buffer") {
    NodeData node;
    NodeData dest;
    node.types.push_back(ElementType{&typeid(int), 1, DataCategory::Fundamental});
    node.valueSizes.push_back(4);

    auto err = node.popFrontSerialized(dest);
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::MalformedInput);
}

TEST_CASE("deserialize reports no task when execution type lacks tasks") {
    NodeData node;
    node.types.push_back(ElementType{&typeid(int), 1, DataCategory::Execution});

    int out = 0;
    auto err = node.deserialize(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
    CHECK(err->message == std::optional<std::string>{"No task available"});
}

TEST_CASE("deserialize reports NotSupported for nested payloads") {
    NodeData node;
    node.appendNested(std::make_unique<PathSpace>());
    node.types.push_back(ElementType{nullptr, 1, DataCategory::UniquePtr});

    int out = 0;
    auto err = node.deserializePop(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NotSupported);
}

TEST_CASE("deserialize skips empty nested slots and reports no data") {
    NodeData node;
    auto slot = std::make_shared<NodeData::NestedSlot>();
    slot->self = slot;
    node.nestedSlots.push_back(std::move(slot));
    node.types.push_back(ElementType{nullptr, 1, DataCategory::UniquePtr});

    int out = 0;
    auto err = node.deserializePop(&out, InputMetadataT<int>{});
    REQUIRE(err.has_value());
    CHECK(err->code == Error::Code::NoObjectFound);
}

TEST_CASE("peekFuture helpers report empty execution queues") {
    NodeData node;
    node.types.push_back(ElementType{&typeid(int), 1, DataCategory::Execution});

    CHECK_FALSE(node.peekFuture().has_value());
    CHECK_FALSE(node.peekAnyFuture().has_value());
}

TEST_CASE("nestedAt reports missing slots and null entries") {
    NodeData node;
    CHECK(node.nestedAt(0) == nullptr);

    auto slot = std::make_shared<NodeData::NestedSlot>();
    slot->self = slot;
    node.nestedSlots.push_back(std::move(slot));
    CHECK(node.nestedAt(0) == nullptr);
}

TEST_CASE("nestedAt reports null slots") {
    NodeData node;
    node.nestedSlots.push_back({});
    CHECK(node.nestedAt(0) == nullptr);
}

TEST_CASE("takeNestedAt returns empty for missing slots") {
    NodeData node;
    auto missing = node.takeNestedAt(0);
    CHECK_FALSE(missing);

    auto slot = std::make_shared<NodeData::NestedSlot>();
    slot->self = slot;
    node.nestedSlots.push_back(std::move(slot));
    auto outOfRange = node.takeNestedAt(1);
    CHECK_FALSE(outOfRange);
}

TEST_CASE("emplaceNestedAt allocates empty slots and rejects occupied slots") {
    NodeData node;
    node.nestedSlots.resize(1);

    std::unique_ptr<PathSpaceBase> first = std::make_unique<PathSpace>();
    auto err = node.emplaceNestedAt(0, first);
    REQUIRE_FALSE(err.has_value());
    CHECK(first == nullptr);

    std::unique_ptr<PathSpaceBase> second = std::make_unique<PathSpace>();
    auto occupied = node.emplaceNestedAt(0, second);
    REQUIRE(occupied.has_value());
    CHECK(occupied->code == Error::Code::InvalidType);
}

TEST_CASE("retargetTasks rewrites notification prefixes") {
    {
        NodeData node;
        auto task = Task::Create(std::weak_ptr<NotificationSink>{},
                                 "events/update",
                                 [] { return 1; },
                                 ExecutionCategory::Immediate);
        node.tasks.push_back(task);
        node.retargetTasks({}, nullptr, "/old", "/root");
        CHECK(task->notificationPath == "/root/events/update");
    }

    {
        NodeData node;
        auto task = Task::Create(std::weak_ptr<NotificationSink>{},
                                 "",
                                 [] { return 1; },
                                 ExecutionCategory::Immediate);
        node.tasks.push_back(task);
        node.retargetTasks({}, nullptr, "", "/root/");
        CHECK(task->notificationPath == "/root/");
    }

    {
        NodeData node;
        auto task = Task::Create(std::weak_ptr<NotificationSink>{},
                                 "/old/events",
                                 [] { return 1; },
                                 ExecutionCategory::Immediate);
        node.tasks.push_back(task);
        node.retargetTasks({}, nullptr, "/old/", "/root");
        CHECK(task->notificationPath == "/root/events");
    }

    {
        NodeData node;
        node.tasks.push_back({});
        node.retargetTasks({}, nullptr, "/old", "/root");
        CHECK(node.tasks.front() == nullptr);
    }
}

TEST_CASE("fromSerializedValue and frontSerializedValueBytes cover empty and invalid buffers") {
    InputMetadata meta{InputMetadataT<int>{}};
    std::array<std::byte, 4> bytes{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};

    auto nodeResult = NodeData::fromSerializedValue(meta, std::span<const std::byte>(bytes.data(), bytes.size()));
    REQUIRE(nodeResult.has_value());
    CHECK(nodeResult->valueSizes.size() == 1);

    auto front = nodeResult->frontSerializedValueBytes();
    REQUIRE(front.has_value());
    CHECK(front->size() == bytes.size());

    nodeResult->valueSizes.front() = bytes.size() + 1;
    CHECK_FALSE(nodeResult->frontSerializedValueBytes().has_value());
}

TEST_CASE("deserializeSnapshot rejects missing metadata chunks") {
    {
        std::array<std::byte, 4> bytes{
            std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
        CHECK_FALSE(node.has_value());
    }

    {
        std::array<std::byte, 8> bytes{
            std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
        CHECK_FALSE(node.has_value());
    }
}

TEST_CASE("deserializeSnapshot rejects truncated value size metadata") {
    {
        std::array<std::byte, 8> bytes{
            std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
        CHECK_FALSE(node.has_value());
    }

    {
        std::array<std::byte, 12> bytes{
            std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
        CHECK_FALSE(node.has_value());
    }
}

TEST_CASE("deserializeSnapshot rejects missing raw buffer metadata") {
    std::array<std::byte, 12> bytes{
        std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    auto node = NodeData::deserializeSnapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
    CHECK_FALSE(node.has_value());
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
