#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "type/SlidingBuffer.hpp"

#include <deque>
#include <memory>
#include <optional>

namespace SP {
class InOptions;
class InsertReturn;
class InputMetadata;
class OutOptions;
class InputData;
class Task;

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData, InOptions const& options);

    auto serialize(const InputData& inputData, const InOptions& options) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options) const -> Expected<int>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int>;
    auto empty() const -> bool;

private:
    auto pushType(InputMetadata const& meta) -> void;
    auto popType() -> void;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract) -> Expected<int>;
    auto validateInputs(const InputMetadata& inputMetadata) -> Expected<void>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, const OutOptions& options, bool isExtract) -> Expected<int>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool isExtract) -> Expected<int>;

    SP::SlidingBuffer                 data;
    std::deque<std::shared_ptr<Task>> tasks; // NodeData is the primary owner of tasks
    std::deque<ElementType>           types;
};

} // namespace SP