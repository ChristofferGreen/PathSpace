#include "pathspace/serialization/InputData.hpp"

namespace SP {

void InputData::serialize(std::queue<std::byte> &queue) const {
    this->serialization.serializationFuncPtr(this->data, queue);
}

void InputData::deserialize(void *obj, std::queue<std::byte> &queue) const {
    this->serialization.deserializationFuncPtr(obj, queue);
}


} // namespace SP