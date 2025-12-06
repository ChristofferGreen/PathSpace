#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "type/SlidingBuffer.hpp"
#include "task/Future.hpp"
#include "task/IFutureAny.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace SP {
class InsertReturn;
class InputMetadata;
class InputData;
class Task;

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData);

    auto serialize(const InputData& inputData) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata) const -> std::optional<Error>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> std::optional<Error>;
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
    auto append(NodeData const& other) -> std::optional<Error>;
    [[nodiscard]] auto valueCount() const -> std::size_t;
    [[nodiscard]] auto typeSummary() const -> std::deque<ElementType> const& { return types; }
    [[nodiscard]] auto rawBuffer() const -> std::span<std::uint8_t const> { return data.rawData(); }
    [[nodiscard]] auto rawBufferFrontOffset() const -> std::size_t { return data.virtualFront(); }
    [[nodiscard]] auto frontSerializedValueBytes() const
        -> std::optional<std::span<const std::byte>>;

private:
    auto popType() -> void;
    auto pushType(InputMetadata const& meta) -> void;
    auto validateInputs(const InputMetadata& inputMetadata) -> std::optional<Error>;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error>;

    SP::SlidingBuffer                  data;
    std::deque<ElementType>            types;
    std::deque<std::shared_ptr<Task>>  tasks;   // NodeData is the primary owner of tasks
    std::deque<Future>                 futures; // Aligned with tasks; lightweight handles for result readiness
    std::deque<FutureAny>              anyFutures; // Aligned with typed tasks; type-erased future handles
    std::deque<std::size_t>            valueSizes; // Serialized byte lengths for non-execution values

};

} // namespace SP
