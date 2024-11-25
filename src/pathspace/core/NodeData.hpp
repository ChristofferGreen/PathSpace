#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "type/SlidingBuffer.hpp"

#include <deque>
#include <memory>
#include <optional>

namespace SP {
class InsertReturn;
class InputMetadata;
class InputData;
class Task;

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData);

    auto serialize(const InputData& inputData) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata) const -> Expected<int>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int>;
    auto empty() const -> bool;

private:
    auto pushType(InputMetadata const& meta) -> void;
    auto popType() -> void;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int>;
    auto validateInputs(const InputMetadata& inputMetadata) -> Expected<void>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> Expected<int>;

    SP::SlidingBuffer                 data;
    std::deque<std::shared_ptr<Task>> tasks; // NodeData is the primary owner of tasks
    std::deque<ElementType>           types;
};

} // namespace SP