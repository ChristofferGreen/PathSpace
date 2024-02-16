#include "pathspace/serialization/InputData.hpp"

namespace SP {

auto InputData::serialize(ByteQueue &queue) const -> void {
    this->metadata.serializationFuncPtr(this->obj, queue);
}

auto InputData::deserialize(ByteQueue const &queue) const -> void {
    this->metadata.deserializationFuncPtrConst(this->obj, queue);
}

auto InputData::deserialize(ByteQueue &queue) const -> void {
    this->metadata.deserializationFuncPtr(this->obj, queue);
}

} // namespace SP