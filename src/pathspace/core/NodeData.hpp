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
        if(inputData.metadata.serialize) {
            inputData.metadata.serialize(inputData.obj, this->data);
            if(this->types.size() && (this->types.back().first==inputData.metadata.id))
                this->types.back().second++;
            else
                this->types.emplace_back(inputData.metadata.id, 1);
        }
    }

    auto deserialize(void *obj, InputMetadata const &inputMetadata) const -> Expected<int> {
        if(inputMetadata.deserialize) {
            // if if function pointer
            // then execute function pointer and assign to obj
            // inputMetadata.execFunctionPointer(obj, this->data);
            if(this->types.size() && (this->types.back().first==inputMetadata.id)) {
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
    std::vector<std::function<void(ConcretePathString const&, PathSpace&, std::atomic<bool>&, void*)>> executions;
    std::vector<std::pair<std::type_info const * const, uint32_t>> types;
};

}