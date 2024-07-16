#pragma once
#include "pathspace/type/InputData.hpp"
#include "pathspace/core/Error.hpp"

#include <functional>
#include <utility>
#include <vector>
#include <typeinfo>

namespace SP {
struct NodeData {
    auto serialize(InputData const &inputData) -> void {
        if (inputData.metadata.serialize) {
            if (inputData.metadata.id==MetadataID::ExecutionFunctionPointer) {
                serializeFunctionPointer(*this, inputData);
            } else {
                serializeRegularData(*this, inputData);
            }
        }
    }

    auto deserialize(void *obj, InputMetadata const &inputMetadata) const -> Expected<int> {
        if(inputMetadata.deserialize) {
            // if if function pointer
            // then execute function pointer and assign to obj
            // inputMetadata.execFunctionPointer(obj, this->data);
            if(this->types.size() && (this->types.back().first==to_type_info(inputMetadata.id))) {
                inputMetadata.deserialize(obj, this->data);
                return 1;
            }
        }
        return std::unexpected(Error{Error::Code::UnserializableType, "The type can not be deserialised for reading."});
    }

    auto deserializePop(void *obj, InputMetadata const &inputMetadata) -> Expected<int> {
        if(inputMetadata.deserializePop) {
            inputMetadata.deserializePop(obj, this->data);
            return 1;
        }
        return std::unexpected(Error{Error::Code::UnserializableType, "The type can not be deserialised for grabbing."});
    }

//private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<std::pair<std::type_info const * const, uint32_t>> types;

    auto serializeFunctionPointer(NodeData& nodeData, InputData const& inputData) -> void {
        //nodeData.executions.emplace_back(inputData.metadata.executeFunctionPointer);
        if (!nodeData.types.empty() && nodeData.types.back().first == nullptr) {
            nodeData.types.back().second++;
        } else {
            nodeData.types.emplace_back(nullptr, 1);
        }
    }

    // Function to serialize regular data
    auto serializeRegularData(NodeData& nodeData, InputData const& inputData) -> void {
        inputData.metadata.serialize(inputData.obj, nodeData.data);
        if (nodeData.types.size() && (nodeData.types.back().first == to_type_info(inputData.metadata.id))) {
            nodeData.types.back().second++;
        } else {
            nodeData.types.emplace_back(to_type_info(inputData.metadata.id), 1);
        }
    }
};

}