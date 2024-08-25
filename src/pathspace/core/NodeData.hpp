#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InsertOptions.hpp"
#include "pathspace/type/DataCategory.hpp"
#include "pathspace/type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <expected>
#include <typeinfo>
#include <vector>

namespace SP {

class NodeData {
public:
    void serialize(const InputData& inputData, const InsertOptions& options) {
        if (!inputData.metadata.serialize) {
            return;
        }

        inputData.metadata.serialize(inputData.obj, data);
        updateTypes(inputData.metadata);
        if (inputData.metadata.category == DataCategory::FunctionPointer && options.execution &&
            options.execution->executionTime == ExecutionOptions::ExecutionTime::OnRead) {
            // Handle function pointer serialization
            // serializeExecutionFunctionPointer(inputData);
        } else {
        }
    }

    std::expected<int, Error> deserialize(void* obj, const InputMetadata& inputMetadata) const {
        if (!inputMetadata.deserialize) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
        }

        if (types.empty()) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No type information found."});
        }

        if (types.back().typeInfo != inputMetadata.typeInfo &&
            types.back().category != DataCategory::ExecutionFunctionPointer) {
            return std::unexpected(Error{Error::Code::UnserializableType, "Type mismatch during deserialization."});
        }

        if (types.back().category == DataCategory::ExecutionFunctionPointer) {
            void* funPtr;
            inputMetadata.deserialize(&funPtr, data);
            inputMetadata.executeFunctionPointer(funPtr, obj);
        } else {
            inputMetadata.deserialize(obj, data);
        }
        return 1;
    }

    std::expected<int, Error> deserializePop(void* obj, const InputMetadata& inputMetadata) {
        if (!inputMetadata.deserializePop) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No pop deserialization function provided."});
        }

        inputMetadata.deserializePop(obj, data);
        updateTypesAfterPop();
        return 1;
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<ElementType> types;

    void updateTypes(InputMetadata const& meta) {
        if (!types.empty()) {
            if (types.back().typeInfo == meta.typeInfo) {
                types.back().elements++;
            }
        } else {
            types.emplace_back(meta.typeInfo, 1, meta.category);
        }
    }

    void updateTypesAfterPop() {
        if (!types.empty()) {
            if (--types.back().elements == 0) {
                types.pop_back();
            }
        }
    }
};

} // namespace SP