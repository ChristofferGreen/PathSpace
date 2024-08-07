#pragma once
#include <type_traits>
#include <typeinfo>

#include "pathspace/type/InputMetadataT.hpp"

namespace SP {

struct InputMetadata {
    InputMetadata() = default;
    template<typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const &obj)
        : id(obj.id),
          serialize(obj.serialize),
          deserializePop(obj.deserializePop),
          deserialize(obj.deserialize)
          {}
    MetadataID id;
    void (*serialize)(void const *obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
    void (*deserialize)(void *obj, std::vector<SERIALIZATION_TYPE> const&) = nullptr;
    void (*deserializePop)(void *obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
};

} // namespace SP