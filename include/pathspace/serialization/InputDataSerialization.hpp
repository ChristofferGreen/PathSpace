#pragma once
#include <queue>

#include "QueueStreamBuffer.hpp"

namespace SP {

template<typename T>
static auto serialize(void const *objPtr, std::queue<std::byte> &byteQueue) -> void {
    T const &obj = *static_cast<T const *>(objPtr);

    QueueStreamBuffer qbuf{byteQueue};
    std::ostream os{&qbuf};
    cereal::BinaryOutputArchive oarchive(os);

    oarchive(obj);
}

template<typename T>
static auto deserialize(void *objPtr, std::queue<std::byte> &byteQueue) -> void {
    T &obj = *static_cast<T *>(objPtr);

    QueueStreamBuffer qbuf{byteQueue};
    std::istream is(&qbuf);
    cereal::BinaryInputArchive iarchive(is);

    iarchive(obj);
}

struct InputDataSerialization {
    InputDataSerialization() = default;
    template<typename T>
    InputDataSerialization(T const &data) : serializationFuncPtr(&serialize<T>),
                                            deserializationFuncPtr(&deserialize<T>) {}

    void (*serializationFuncPtr)(void const *obj, std::queue<std::byte>&);
    void (*deserializationFuncPtr)(void *obj, std::queue<std::byte>&);
};

} // namespace SP