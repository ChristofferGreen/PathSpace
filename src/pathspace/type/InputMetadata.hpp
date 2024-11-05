#pragma once
#include <type_traits>
#include <typeinfo>

#include "pathspace/type/DataCategory.hpp"
#include "pathspace/type/ExecutionCategory.hpp"
#include "pathspace/type/InputMetadataT.hpp"

namespace SP {

struct InputMetadata {
    InputMetadata() = default;
    template <typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const& obj)
        : dataCategory(obj.dataCategory), executionCategory(obj.executionCategory), typeInfo(obj.typeInfo), serialize2(obj.serialize2),
          deserialize2(obj.deserialize2), deserializePop2(obj.deserializePop2) {
    }

    DataCategory dataCategory;
    ExecutionCategory executionCategory;
    std::type_info const* typeInfo = nullptr;
    void (*serialize2)(void const* obj, SP::SlidingBuffer&) = nullptr;
    void (*deserialize2)(void* obj, SP::SlidingBuffer const&) = nullptr;
    void (*deserializePop2)(void* obj, SP::SlidingBuffer&) = nullptr;
};

} // namespace SP