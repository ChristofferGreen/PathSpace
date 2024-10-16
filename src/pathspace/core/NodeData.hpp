#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InOptions.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/InsertReturn.hpp"
#include "path/ConstructiblePath.hpp"
#include "pathspace/type/DataCategory.hpp"
#include "pathspace/type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <expected>
#include <optional>
#include <vector>

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
        updateTypes(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution) const
            -> Expected<int> {
        if (this->types.front().category == DataCategory::ExecutionFunctionPointer
            || this->types.front().category == DataCategory::ExecutionStdFunction) {
            if (this->types.front().typeInfo == inputMetadata.typeInfo) {
                assert(this->tasks.size() > 0);
                this->tasks.front().taskExecutorStdFunction(this->tasks.front(), obj);
                return 1;
            }
            return 0;
        }

        if (!inputMetadata.deserialize) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
        }

        if (types.empty()) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No type information found."});
        }

        inputMetadata.deserialize(obj, data);
        return 1;
    }

    Expected<int> deserializePop(void* obj, const InputMetadata& inputMetadata) {
        if (!inputMetadata.deserializePop) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided."});
        }

        inputMetadata.deserializePop(obj, data);
        updateTypesAfterPop();
        return 1;
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<Task> tasks;
    std::vector<ElementType> types;

    auto updateTypes(InputMetadata const& meta) -> void {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo) {
                types.back().elements++;
            }
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.category);
        }
    }

    auto updateTypesAfterPop() -> void {
        if (!types.empty()) {
            if (--types.back().elements == 0) {
                types.pop_back();
            }
        }
    }
};

} // namespace SP