#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InsertOptions.hpp"
#include "pathspace/type/InputData.hpp"

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

        if (inputData.metadata.id == MetadataID::ExecutionFunctionPointer && options.execution &&
            options.execution->executionTime == ExecutionOptions::ExecutionTime::OnRead) {
            // Handle function pointer serialization
            serializeExecutionFunctionPointer(inputData);
        } else {
            serializeData(inputData);
        }
    }

    std::expected<int, Error> deserialize(void* obj, const InputMetadata& inputMetadata) const {
        if (!inputMetadata.deserialize) {
            return std::unexpected(Error{Error::Code::UnserializableType, "No deserialization function provided."});
        }

        if (types.empty() || types.back().typeInfo != to_type_info(inputMetadata.id)) {
            return std::unexpected(Error{Error::Code::UnserializableType, "Type mismatch during deserialization."});
        }

        inputMetadata.deserialize(obj, data);
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

    void serializeData(const InputData& inputData) {
        inputData.metadata.serialize(inputData.obj, data);
        updateTypes(inputData.metadata.id);
    }

    void serializeExecutionFunctionPointer(const InputData& inputData) {
        // Implement function pointer serialization logic here
        // This is a placeholder and should be implemented based on your specific requirements
    }

    void updateTypes(MetadataID id) {
        if (!types.empty() && types.back().typeInfo == to_type_info(id)) {
            types.back().elements++;
        } else {
            types.emplace_back(to_type_info(id), 1);
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