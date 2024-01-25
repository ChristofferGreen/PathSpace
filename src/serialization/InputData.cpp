#include "pathspace/serialization/InputData.hpp"

namespace SP {

auto InputData::serialize(std::queue<std::byte> &queue) const -> void {
    this->metadata.serializationFuncPtr(this->obj, queue);
}

auto InputData::deserialize(std::queue<std::byte> &queue) const -> void {
    this->metadata.deserializationFuncPtr(this->obj, queue);
}

auto InputData::serialize(ByteQueue &queue) const -> void {
    this->metadata.serializationFuncPtr2(this->obj, queue);
}

auto InputData::deserialize(ByteQueue const &queue) const -> void {
    this->metadata.deserializationFuncPtr2(this->obj, queue);
}

} // namespace SP