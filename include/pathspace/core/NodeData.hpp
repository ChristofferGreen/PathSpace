#pragma once
#include "pathspace/type/InputData.hpp"

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

    std::vector<SERIALIZATION_TYPE> data;
    std::vector<std::pair<std::type_info const * const, uint32_t>> types;
};

}