#pragma once
#include "ElementType.hpp"
#include "Error.hpp"
#include "InsertOptions.hpp"
#include "pathspace/type/InputData.hpp"

#include <typeinfo>
#include <utility>
#include <vector>

namespace SP {
struct NodeData {
    auto serialize(InputData const& inputData, InsertOptions const& options) -> void {
        if (inputData.metadata.serialize) {
            if (inputData.metadata.id == MetadataID::ExecutionFunctionPointer) {
                if (options.execution.has_value() &&
                    options.execution.value().executionTime == ExecutionOptions::ExecutionTime::OnRead) {
                    switch (options.execution.value().executionTime) {
                        case ExecutionOptions::ExecutionTime::OnRead:
                        default:
                            serialize(*this, inputData);
                            break;
                    }
                }
            } else {
                serialize(*this, inputData);
            }
        }
    }

    auto deserialize(void* obj, InputMetadata const& inputMetadata) const -> Expected<int> {
        if (inputMetadata.deserialize) {
            // if if function pointer
            // then execute function pointer and assign to obj
            // inputMetadata.execFunctionPointer(obj, this->data);
            if (this->types.size() && (this->types.back().typeInfo == to_type_info(inputMetadata.id))) {
                inputMetadata.deserialize(obj, this->data);
                return 1;
            }
        }
        return std::unexpected(Error{Error::Code::UnserializableType, "The type can not be deserialised for reading."});
    }

    auto deserializePop(void* obj, InputMetadata const& inputMetadata) -> Expected<int> {
        if (inputMetadata.deserializePop) {
            inputMetadata.deserializePop(obj, this->data);
            return 1;
        }
        return std::unexpected(
                Error{Error::Code::UnserializableType, "The type can not be deserialised for grabbing."});
    }

private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<ElementType> types;

    auto serialize(NodeData& nodeData, InputData const& inputData) -> void {
        inputData.metadata.serialize(inputData.obj, nodeData.data);
        if (nodeData.types.size() && (nodeData.types.back().typeInfo == to_type_info(inputData.metadata.id))) {
            nodeData.types.back().elements++;
        } else {
            nodeData.types.emplace_back(to_type_info(inputData.metadata.id), 1);
        }
    }
};

} // namespace SP