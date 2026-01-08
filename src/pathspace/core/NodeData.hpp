#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "type/SlidingBuffer.hpp"
#include "task/Future.hpp"
#include "task/IFutureAny.hpp"
#include "PathSpaceBase.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <mutex>
#include <atomic>

namespace SP {
class InsertReturn;
class InputMetadata;
class InputData;
class Task;
class PathSpaceBase;
class NotificationSink;
class Executor;
struct NodeDataTestHelper;

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData);
    NodeData(NodeData const& other);
    NodeData& operator=(NodeData const& other);
    NodeData(NodeData&&) noexcept;
    NodeData& operator=(NodeData&&) noexcept;
    ~NodeData();

    auto serialize(const InputData& inputData) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata) const -> std::optional<Error>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> std::optional<Error>;
    auto deserializeIndexed(std::size_t matchIndex,
                            InputMetadata const& inputMetadata,
                            bool doPop,
                            void* obj) -> std::optional<Error>;
    auto popFrontSerialized(NodeData& destination) -> std::optional<Error>;
    auto empty() const -> bool;
    // Return a Future aligned with the front task (if present)
    auto peekFuture() const -> std::optional<Future>;
    // Return a type-erased future aligned with the front typed task (if present)
    auto peekAnyFuture() const -> std::optional<FutureAny>;
    [[nodiscard]] auto serializeSnapshot() const -> std::optional<std::vector<std::byte>>;
    static auto        deserializeSnapshot(std::span<const std::byte> bytes) -> std::optional<NodeData>;
    static auto        fromSerializedValue(InputMetadata const& metadata,
                                           std::span<const std::byte> bytes) -> Expected<NodeData>;
    [[nodiscard]] auto hasExecutionPayload() const noexcept -> bool;
    // Nested PathSpace queue helpers
    [[nodiscard]] auto hasNestedSpaces() const noexcept -> bool;
    [[nodiscard]] auto nestedCount() const noexcept -> std::size_t;
    [[nodiscard]] auto nestedAt(std::size_t index) const -> PathSpaceBase*;
    auto takeNestedAt(std::size_t index) -> std::unique_ptr<PathSpaceBase>;
    auto appendNested(std::unique_ptr<PathSpaceBase> space) -> void;
    auto borrowNestedShared(std::size_t index) const -> std::shared_ptr<PathSpaceBase>;
    // Replace an existing placeholder slot (if present) without changing type metadata.
    auto emplaceNestedAt(std::size_t index, std::unique_ptr<PathSpaceBase>& space) -> std::optional<Error>;
    // Rebind existing tasks to a new notification sink/executor (used when a PathSpace is mounted).
    void retargetTasks(std::weak_ptr<NotificationSink> sink, Executor* exec);
    auto append(NodeData const& other) -> std::optional<Error>;
    [[nodiscard]] auto valueCount() const -> std::size_t;
    [[nodiscard]] auto typeSummary() const -> std::deque<ElementType> const& { return types; }
    [[nodiscard]] auto rawBuffer() const -> std::span<std::uint8_t const> { return data.rawData(); }
    [[nodiscard]] auto rawBufferFrontOffset() const -> std::size_t { return data.virtualFront(); }
    [[nodiscard]] auto frontSerializedValueBytes() const
        -> std::optional<std::span<const std::byte>>;
    // Drop any nested payload metadata (used when copying non-ownable nested spaces)
    void dropNestedTypes();

private:
    auto popType() -> void;
    auto pushType(InputMetadata const& meta) -> void;
    auto validateInputs(const InputMetadata& inputMetadata) -> std::optional<Error>;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;

    struct NestedSlot {
        std::unique_ptr<PathSpaceBase> ptr;
        mutable std::atomic<std::size_t> borrows{0};
        std::shared_ptr<NestedSlot> self; // back-reference for aliasing
        mutable std::mutex m;
        mutable std::condition_variable cv;
    };

    SP::SlidingBuffer                  data;
    std::deque<ElementType>            types;
    std::deque<std::shared_ptr<Task>>  tasks;   // NodeData is the primary owner of tasks
    std::deque<Future>                 futures; // Aligned with tasks; lightweight handles for result readiness
    std::deque<FutureAny>              anyFutures; // Aligned with typed tasks; type-erased future handles
    std::deque<std::size_t>            valueSizes; // Serialized byte lengths for non-execution values
    std::vector<std::shared_ptr<NestedSlot>> nestedSlots;

    void removeNestedTypeAt(std::size_t nestedIndex);
    std::optional<Error> validateNested(std::unique_ptr<PathSpaceBase> const& space) const;

    friend struct NodeDataTestHelper; // test hooks

};

struct NodeDataTestHelper {
    static void setBorrowWaitHook(std::function<void()> hook);
    static void setNestedSerializeHook(std::function<std::optional<Error>()> hook);
};

} // namespace SP
