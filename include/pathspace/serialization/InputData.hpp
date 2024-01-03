#pragma once
#include "pathspace/type/InputMetadata.hpp"

#include <utility>

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& data) : data(const_cast<void*>(static_cast<const void*>(&data))), metadata(InputMetadataT<T>{}) {}
    
    void serialize(std::queue<std::byte> &queue) const;
    void deserialize(void *obj, std::queue<std::byte> &queue) const;

    void *data = nullptr;
    InputMetadata metadata;
};

} // namespace SP