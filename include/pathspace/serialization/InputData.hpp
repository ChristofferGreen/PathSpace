#pragma once
#include "pathspace/type/InputMetadata.hpp"
#include "pathspace/utils/ByteQueue.hpp"

#include <utility>

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& obj) : obj(const_cast<void*>(static_cast<const void*>(&obj))), metadata(InputMetadataT<T>{}) {}
    
    void serialize(std::queue<std::byte> &queue) const;
    void deserialize(std::queue<std::byte> &queue) const;

    void serialize(ByteQueue &queue) const;
    void deserialize(ByteQueue const &queue) const;

    void *obj = nullptr;
    InputMetadata metadata;
};

} // namespace SP