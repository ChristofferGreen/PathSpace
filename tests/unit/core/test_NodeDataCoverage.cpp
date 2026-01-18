#include "core/NodeData.hpp"
#include "core/NotificationSink.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "task/Task.hpp"
#include "task/Executor.hpp"
#include "PathSpace.hpp"

#include "third_party/doctest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>
#include <thread>
#include <typeinfo>

using namespace SP;
using namespace std::chrono_literals;

namespace {
struct RejectingExecutor : Executor {
    std::optional<Error> submit(std::weak_ptr<Task>&&) override {
        return Error{Error::Code::UnknownError, "executor rejected task"};
    }
    void shutdown() override {}
    auto size() const -> size_t override { return 1; }
};

auto makeImmediateTask() -> std::shared_ptr<Task> {
    return Task::Create(std::weak_ptr<NotificationSink>{},
                        "/immediate",
                        [] { return 7; },
                        ExecutionCategory::Immediate);
}

auto makeLazyTask() -> std::shared_ptr<Task> {
    return Task::Create(std::weak_ptr<NotificationSink>{},
                        "/lazy",
                        [] { return 3; },
                        ExecutionCategory::Lazy);
}

template <typename T>
void appendScalar(std::vector<std::byte>& bytes, T value) {
    auto* raw = reinterpret_cast<std::byte*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}
} // namespace

TEST_SUITE("core.nodedata.coverage") {
    TEST_CASE("deserializeSnapshot rejects truncated buffers") {
        std::array<std::byte, 2> truncated{};
        auto restored = NodeData::deserializeSnapshot(truncated);
        CHECK_FALSE(restored.has_value());
    }

    TEST_CASE("value constructor forwards to serialize") {
        int value = 9;
        InputData input{value};
        NodeData  node{input};

        int              out  = 0;
        InputMetadataT<int> meta{};
        auto err = node.deserialize(&out, meta);
        CHECK_FALSE(err.has_value());
        CHECK(out == value);
    }

    TEST_CASE("serialize detects null unique_ptr payload pointer") {
        InputMetadata meta{};
        meta.dataCategory = DataCategory::UniquePtr;
        meta.typeInfo     = &typeid(PathSpace);
        InputData input{static_cast<void const*>(nullptr), meta};

        NodeData node;
        auto     err = node.serialize(input);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::InvalidType);
    }

    TEST_CASE("nested serialize hook surfaces injected error") {
        NodeDataTestHelper::setNestedSerializeHook([]() -> std::optional<Error> {
            return Error{Error::Code::InvalidPermissions, "hook reject"};
        });

        auto     nested = std::make_unique<PathSpace>();
        InputData input{nested};
        NodeData  node;
        auto      err = node.serialize(input);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::InvalidPermissions);

        NodeDataTestHelper::setNestedSerializeHook(nullptr);
    }

    TEST_CASE("immediate tasks require executor and propagate submission errors") {
        InputMetadataT<int> meta{};

        SUBCASE("executor refusal surfaces error") {
            RejectingExecutor exec;
            InputData         input{nullptr, meta};
            input.task     = makeImmediateTask();
            input.executor = &exec;

            NodeData node;
            auto     err = node.serialize(input);
            REQUIRE(err.has_value());
            CHECK(err->code == Error::Code::UnknownError);
        }

        SUBCASE("missing executor reports UnknownError") {
            InputData input{nullptr, meta};
            input.task = makeImmediateTask();

            NodeData node;
            auto     err = node.serialize(input);
            REQUIRE(err.has_value());
            CHECK(err->code == Error::Code::UnknownError);
        }
    }

    TEST_CASE("serialize reports missing serialization function") {
        InputMetadata meta{};
        meta.dataCategory = DataCategory::SerializedData;
        meta.typeInfo     = &typeid(int);
        // leave serialize/deserialize null

        InputData input{nullptr, meta};
        NodeData  node;
        auto      err = node.serialize(input);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::SerializationFunctionMissing);
    }

    TEST_CASE("self assignment guards leave data intact") {
        NodeData node;
        int      value = 4;
        REQUIRE_FALSE(node.serialize(InputData{value}).has_value());

        InputMetadataT<int> meta{};
        int                  out = 0;

        node = node; // copy self-assign
        CHECK_FALSE(node.deserialize(&out, meta).has_value());
        CHECK(out == value);

        out = 0;
        node = std::move(node); // move self-assign
        CHECK_FALSE(node.deserialize(&out, meta).has_value());
        CHECK(out == value);
    }

    TEST_CASE("deserialize reports missing value length metadata and buffer overruns") {
        // Craft snapshot with one Fundamental type and zero valueSizes.
        std::vector<std::byte> bytes;
        appendScalar<std::uint32_t>(bytes, 2); // version
        appendScalar<std::uint32_t>(bytes, 1); // types count
        appendScalar<std::uintptr_t>(bytes, reinterpret_cast<std::uintptr_t>(&typeid(int)));
        appendScalar<std::uint32_t>(bytes, 2); // elements (two entries)
        bytes.push_back(static_cast<std::byte>(DataCategory::Fundamental));
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        appendScalar<std::uint32_t>(bytes, 0); // valueSizes count -> missing lengths
        appendScalar<std::uint32_t>(bytes, 0); // raw size
        appendScalar<std::uint32_t>(bytes, 0); // front

        auto restored = NodeData::deserializeSnapshot(bytes);
        REQUIRE(restored.has_value());

        int                 out  = 0;
        InputMetadataT<int> meta{};
        auto                err = restored->deserializeIndexed(1, meta, false, &out);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::MalformedInput);

        // Now craft snapshot where declared length exceeds buffer.
        std::vector<std::byte> shortPayload;
        appendScalar<std::uint32_t>(shortPayload, 2);
        appendScalar<std::uint32_t>(shortPayload, 1);
        appendScalar<std::uintptr_t>(shortPayload, reinterpret_cast<std::uintptr_t>(&typeid(int)));
        appendScalar<std::uint32_t>(shortPayload, 2);
        shortPayload.push_back(static_cast<std::byte>(DataCategory::Fundamental));
        shortPayload.push_back(std::byte{0});
        shortPayload.push_back(std::byte{0});
        shortPayload.push_back(std::byte{0});
        appendScalar<std::uint32_t>(shortPayload, 2); // two value lengths
        appendScalar<std::uint32_t>(shortPayload, 4); // first length
        appendScalar<std::uint32_t>(shortPayload, 8); // second length (overruns raw)
        appendScalar<std::uint32_t>(shortPayload, 4); // raw size (only first value present)
        appendScalar<std::uint32_t>(shortPayload, 0); // front
        appendScalar<std::uint32_t>(shortPayload, 0); // raw bytes filler (4 bytes)

        auto bad = NodeData::deserializeSnapshot(shortPayload);
        REQUIRE(bad.has_value());
        err = bad->deserializeIndexed(1, meta, false, &out);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::MalformedInput);
    }

    TEST_CASE("deserialize rejects missing deserializer callbacks") {
        NodeData data;
        int      value = 11;
        REQUIRE_FALSE(data.serialize(InputData{value}).has_value());

        InputMetadata meta{};
        meta.dataCategory = DataCategory::Fundamental;
        meta.typeInfo     = &typeid(int);

        int out = 0;
        auto err = data.deserialize(&out, meta);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::UnserializableType);

        // Fresh node for pop path to avoid modifying shared state.
        NodeData popNode;
        REQUIRE_FALSE(popNode.serialize(InputData{value}).has_value());
        err = popNode.deserializePop(&out, meta);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::UnserializableType);
    }

    TEST_CASE("deserializeIndexed handles empty and missing lengths") {
        InputMetadataT<int> meta{};
        NodeData            empty;
        int                 out = 0;
        auto                err = empty.deserializeIndexed(0, meta, false, &out);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NoObjectFound);

        NodeData withType;
        REQUIRE_FALSE(withType.serialize(InputData{out}).has_value());
        // Drop valueSizes to force missing length metadata by popping front.
        CHECK_FALSE(withType.deserializePop(&out, meta).has_value());
        err = withType.deserializeIndexed(0, meta, false, &out);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NoObjectFound);
    }

    TEST_CASE("popFrontSerialized rejects non-serializable fronts") {
        NodeData nestedNode;
        REQUIRE_FALSE(nestedNode.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        NodeData destination;
        auto     err = nestedNode.popFrontSerialized(destination);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NotSupported);

        NodeData execNode;
        InputMetadataT<int> meta{};
        InputData           input{nullptr, meta};
        input.task = makeLazyTask();
        REQUIRE_FALSE(execNode.serialize(input).has_value());
        err = execNode.popFrontSerialized(destination);
        REQUIRE(err.has_value());
        INFO(err->message.value_or("no message"));
        CHECK(err->code == Error::Code::NotSupported);
    }

    TEST_CASE("append rejects execution or nested payloads") {
        NodeData withExec;
        InputMetadataT<int> meta{};
        InputData           execInput{nullptr, meta};
        execInput.task = makeLazyTask();
        REQUIRE_FALSE(withExec.serialize(execInput).has_value());

        NodeData target;
        auto     err = target.append(withExec);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NotSupported);

        NodeData withNested;
        REQUIRE_FALSE(withNested.serialize(InputData{std::make_unique<PathSpace>()}).has_value());
        err = target.append(withNested);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NotSupported);
    }

    TEST_CASE("peek helpers return null when front is not execution") {
        NodeData node;
        int      value = 1;
        REQUIRE_FALSE(node.serialize(InputData{value}).has_value());
        CHECK_FALSE(node.peekFuture().has_value());
        CHECK_FALSE(node.peekAnyFuture().has_value());
    }

    TEST_CASE("takeNestedAt handles empty and out-of-range slots") {
        NodeData node;
        CHECK(node.takeNestedAt(0) == nullptr);

        auto nested = std::unique_ptr<PathSpaceBase>(std::make_unique<PathSpace>().release());
        auto err    = node.emplaceNestedAt(1, nested);
        REQUIRE(err.has_value());
        CHECK(err->code == Error::Code::NoSuchPath);
    }

    TEST_CASE("borrow wait hook fires before nested removal waits") {
        std::atomic<int> hookCount{0};
        NodeDataTestHelper::setBorrowWaitHook([&]() { hookCount.fetch_add(1, std::memory_order_relaxed); });

        NodeData data;
        REQUIRE_FALSE(data.serialize(InputData{std::make_unique<PathSpace>()}).has_value());

        auto borrowed = data.borrowNestedShared(0);
        REQUIRE(borrowed);

        std::thread remover([&]() { data.takeNestedAt(0); });
        std::this_thread::sleep_for(10ms);
        borrowed.reset();
        remover.join();

        CHECK(hookCount.load() >= 1);
        NodeDataTestHelper::setBorrowWaitHook(nullptr);
    }

    TEST_CASE("fromSerializedValue and frontSerializedValueBytes basic coverage") {
        InputMetadata meta{};
        auto          result = NodeData::fromSerializedValue(meta, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Error::Code::InvalidType);

        NodeData empty;
        CHECK_FALSE(empty.frontSerializedValueBytes().has_value());
    }
}
