#pragma once

#include "ElementType.hpp"
#include "Error.hpp"
#include "ExecutionOptions.hpp"
#include "InOptions.hpp"
#include "InsertReturn.hpp"
#include "path/ConstructiblePath.hpp"
#include "type/DataCategory.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <cassert>
#include <deque>
#include <expected>
#include <optional>

namespace SP {

class NodeData {
public:
    auto serialize(ConstructiblePath const& path, const InputData& inputData, const InOptions& options, InsertReturn& ret)
            -> std::optional<Error> {
        if (inputData.task.has_value()) {
            this->tasks.push_back(std::move(inputData.task.value()));
            ret.nbrTasksCreated++;
        } else {
            if (!inputData.metadata.serialize)
                return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
            inputData.metadata.serialize(inputData.obj, data);
        }
        pushType(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution) const
            -> Expected<int> {
        return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, execution, false);
    }

    auto deserializePop(void* obj, const InputMetadata& inputMetadata) -> Expected<int> {
        return deserializeImpl(obj, inputMetadata, std::nullopt, true);
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::deque<Task> tasks;
    std::deque<ElementType> types;

    auto deserializeImpl(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution, bool shouldPop)
            -> Expected<int> {
        if (types.empty())
            return 0;

        if (this->types.front().typeInfo != inputMetadata.typeInfo)
            return 0;

        if (this->types.front().category == DataCategory::ExecutionFunctionPointer
            || this->types.front().category == DataCategory::ExecutionStdFunction) {
            assert(!this->tasks.empty());
            this->tasks.front().function(this->tasks.front(), obj);
            if (shouldPop) {
                this->tasks.pop_front();
                popType();
            }
            return 1;
        }

        if (shouldPop) {
            if (!inputMetadata.deserializePop) {
                return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided."});
            }
            inputMetadata.deserializePop(obj, data);
            popType();
        } else {
            if (!inputMetadata.deserialize) {
                return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
            }
            inputMetadata.deserialize(obj, data);
        }
        return 1;
    }

    auto pushType(InputMetadata const& meta) -> void {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo)
                types.back().elements++;
            else
                types.emplace_back(meta.typeInfo, 1, meta.category);
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.category);
        }
    }

    auto popType() -> void {
        if (!this->types.empty()) {
            if (--this->types.front().elements == 0) {
                this->types.erase(this->types.begin());
            }
        }
    }
};

} // namespace SP