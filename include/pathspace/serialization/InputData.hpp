#pragma once
#include "pathspace/type/InputMetadata.hpp"
#include "InputDataSerialization.hpp"

#include <utility>

namespace SP {

struct InputData {
    template<typename T>
    InputData(T&& data) : data(const_cast<void*>(static_cast<const void*>(&data))), metadata(std::forward<T>(data)), serialization(std::forward<T>(data)) {}
    
    void serialize(void const *obj, std::queue<std::byte> &queue);
    void deserialize(void *obj, std::queue<std::byte> &queue);

    void *data = nullptr;
    InputMetadata metadata;
    InputDataSerialization serialization;
};

} // namespace SP