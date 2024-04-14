#include "pathspace/serialization/InputData.hpp"

namespace SP {

auto InputData::serialize(std::vector<uint8_t> &queue) const -> void {
    this->metadata.serializationFuncPtr(this->obj, queue);
}

auto InputData::deserialize(std::vector<uint8_t> &queue) const -> void {
    this->metadata.deserializationFuncPtr(this->obj, queue);
}

} // namespace SP