#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "InOptions.hpp"
#include "InsertReturn.hpp"
#include "core/BlockOptions.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/OutOptions.hpp"
#include "taskpool/Task.hpp"
#include "type/DataCategory.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

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

    std::vector<SERIALIZATION_TYPE> data;
    std::deque<std::shared_ptr<Task>> tasks;
    std::deque<ElementType> types;
};

} // namespace SP