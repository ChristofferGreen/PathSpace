#include "pathspace/serialization/InputData.hpp"

namespace SP {

void InputData::serialize(std::queue<std::byte> &queue) const {
    this->metadata.serializationFuncPtr(this->obj, queue);
}

void InputData::deserialize(std::queue<std::byte> &queue) const {
    this->metadata.deserializationFuncPtr(this->obj, queue);
}


} // namespace SP