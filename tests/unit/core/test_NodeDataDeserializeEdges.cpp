#define private public
#include "core/NodeData.hpp"
#undef private

#include "core/NotificationSink.hpp"
#include "task/Task.hpp"
#include "task/TaskPool.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "third_party/doctest.h"

#include <thread>

using namespace SP;

namespace {
auto makeImmediateTask() -> std::shared_ptr<Task> {
    return Task::Create(std::weak_ptr<NotificationSink>{},
                        "/immediate",
                        [] { return 7; },
                        ExecutionCategory::Immediate);
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

} // TEST_SUITE
