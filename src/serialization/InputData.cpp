#include "pathspace/serialization/InputData.hpp"

namespace SP {

void InputData::serialize(void const *obj, std::queue<std::byte> &queue) {
    this->serialization.serializationFuncPtr(obj, queue);
}

void InputData::deserialize(void *obj, std::queue<std::byte> &queue) {
    this->serialization.deserializationFuncPtr(obj, queue);
}


} // namespace SP