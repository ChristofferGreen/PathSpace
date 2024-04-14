#include "pathspace/serialization/InputData.hpp"

namespace SP {

auto InputData::serialize(std::vector<SERIALIZATION_TYPE> &queue) const -> void {
    this->metadata.serialize(this->obj, queue);
}

auto InputData::deserialize(std::vector<SERIALIZATION_TYPE> &queue) const -> void {
    this->metadata.deserializePop(this->obj, queue);
}

} // namespace SP