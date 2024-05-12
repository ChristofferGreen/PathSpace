#include "pathspace/type/InputData.hpp"

namespace SP {

auto InputData::serialize(std::vector<SERIALIZATION_TYPE> &queue) const -> void {
    this->metadata.serialize(this->obj, queue);
}

} // namespace SP