#pragma once
#include <queue>

namespace SP {

struct InputDataSerialization {
    InputDataSerialization() = default;
    template<typename T>
    InputDataSerialization(InputMetadata const &metadata, T const &data) {}

    void (*serializationFuncPtr)(std::queue<std::byte>&);
    void (*deserializationFuncPtr)(std::queue<std::byte> const&, void *obj);
};

} // namespace SP