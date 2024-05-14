#pragma once
#include "pathspace/type/InputData.hpp"
#include "pathspace/core/Error.hpp"

#include <utility>
#include <vector>
#include <typeinfo>

namespace SP {

struct NodeData {
    auto serialize(InputData const &inputData) -> void {
        inputData.metadata.serialize(inputData.obj, this->data);
        if(this->types.size() && (this->types.back().first==inputData.metadata.id))
            this->types.back().second++;
        else
            this->types.emplace_back(inputData.metadata.id, 1);
    }

    auto deserialize(void *obj, InputMetadata const &inputMetadata) const -> Expected<int> {
        if(this->types.size() && (this->types.back().first==inputMetadata.id)) {
            inputMetadata.deserialize(obj, this->data);
            return 1;
        }
        return std::unexpected(Error{Error::Code::InvalidType, "The next element is of another type then what was requested."});
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