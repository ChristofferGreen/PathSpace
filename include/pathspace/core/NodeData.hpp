#pragma once
#include "pathspace/type/InputData.hpp"
#include "pathspace/core/Error.hpp"

#include <utility>
#include <vector>
#include <typeinfo>

namespace SP {

struct NodeData {
    auto serialize(InputData const &inputData) -> void {
        inputData.serialize(this->data);
        if(this->types.size() && this->types.end()->first==inputData.metadata.id)
            this->types.end()->second++;
        else
            this->types.push_back(std::make_pair(inputData.metadata.id, 1));
    }

    auto deserialize(void *obj, InputMetadata const &inputMetadata) const -> Expected<int> {
        if(this->types.size() && this->types.end()->first==inputMetadata.id) {
            inputMetadata.deserialize(obj, this->data);
            return 1;
        }
        return 0;
    }

    auto deserializePop(void *obj, InputMetadata const &inputMetadata) -> Expected<int> {
        inputMetadata.deserializePop(obj, this->data);
        return 1;
    }

//private:
    std::vector<SERIALIZATION_TYPE> data;
    std::vector<std::pair<std::type_info const * const, uint32_t>> types;
};

}