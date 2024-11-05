#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "InOptions.hpp"
#include "InsertReturn.hpp"
#include "core/OutOptions.hpp"
#include "taskpool/Task.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include "type/SlidingBuffer.hpp"

#include <cassert>
#include <deque>
#include <expected>
#include <optional>

namespace SP {

struct NodeData {
    NodeData() = default;
    NodeData(InputData const& inputData, InOptions const& options, InsertReturn& ret);

    auto serialize(const InputData& inputData, const InOptions& options, InsertReturn& ret) -> std::optional<Error>;
    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options) const -> Expected<int>;
    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int>;
    auto empty() const -> bool;

private:
    auto pushType(InputMetadata const& meta) -> void;
    auto popType() -> void;
    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<OutOptions> const& options, bool isExtract)
            -> Expected<int>;
    auto validateInputs(const InputMetadata& inputMetadata) -> Expected<void>;
    auto deserializeExecution(void* obj, const InputMetadata& inputMetadata, const OutOptions& options, bool isExtract) -> Expected<int>;
    auto deserializeData(void* obj, const InputMetadata& inputMetadata, bool isExtract) -> Expected<int>;

    SP::SlidingBuffer data;
    std::deque<std::shared_ptr<Task>> tasks;
    std::deque<ElementType> types;
};

} // namespace SP