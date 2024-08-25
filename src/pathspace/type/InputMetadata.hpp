#pragma once
#include <type_traits>
#include <typeinfo>

#include "pathspace/type/DataCategory.hpp"
#include "pathspace/type/InputMetadataT.hpp"

namespace SP {

struct InputMetadata {
    InputMetadata() = default;
    template <typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const& obj)
        : category(obj.category), typeInfo(obj.typeInfo), serialize(obj.serialize), deserializePop(obj.deserializePop),
          deserialize(obj.deserialize) {
    }
    DataCategory category;
    std::type_info const* typeInfo = nullptr;
    void (*serialize)(void const* obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
    void (*deserialize)(void* obj, std::vector<SERIALIZATION_TYPE> const&) = nullptr;
    void (*deserializePop)(void* obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
    void (*executeFunctionPointer)(void const* const functionPointer, void const* const returnData) = nullptr;
};

} // namespace SP