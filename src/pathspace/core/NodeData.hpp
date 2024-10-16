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
        pushType(inputData.metadata);
        return std::nullopt;
    }

    auto deserialize(void* obj, const InputMetadata& inputMetadata, std::optional<ExecutionOptions> const& execution) const
            -> Expected<int> {
        if (types.empty())
            return 0;

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

        inputMetadata.deserialize(obj, data);
        return 1;
    }

    Expected<int> deserializePop(void* obj, const InputMetadata& inputMetadata) {
        if (types.empty())
            return 0;

        if (this->types.front().category == DataCategory::ExecutionFunctionPointer
            || this->types.front().category == DataCategory::ExecutionStdFunction) {
            if (this->types.front().typeInfo == inputMetadata.typeInfo) {
                assert(this->tasks.size() > 0);
                this->tasks.front().taskExecutorStdFunction(this->tasks.front(), obj);
                this->tasks.erase(this->tasks.begin());
                popType();
                return 1;
            }
            return 0;
        }

        if (!inputMetadata.deserializePop)
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});

        inputMetadata.deserializePop(obj, data);
        popType();
        return 1;
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<Task> tasks;
    std::vector<ElementType> types;

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