#pragma once
#include <functional>
#include <memory>
#include <type_traits>
#include <typeinfo>

#include "type/DataCategory.hpp"
#include "type/FunctionCategory.hpp"
#include "type/InputMetadataT.hpp"

namespace SP {
struct PodPayloadBase;

struct InputMetadata {
    InputMetadata() = default;
    template <typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const& obj)
        : dataCategory(obj.dataCategory),
          functionCategory(obj.functionCategory),
          typeInfo(obj.typeInfo),
          serialize(obj.serialize),
          deserialize(obj.deserialize),
          deserializePop(obj.deserializePop),
          podPreferred(obj.podPreferred) {
    }

    DataCategory          dataCategory;
    FunctionCategory      functionCategory;
    std::type_info const* typeInfo                           = nullptr;
    void (*serialize)(void const* obj, SP::SlidingBuffer&)   = nullptr;
    void (*deserialize)(void* obj, SP::SlidingBuffer const&) = nullptr;
    void (*deserializePop)(void* obj, SP::SlidingBuffer&)    = nullptr;
    bool podPreferred                                        = false;
    // Factory to create a PodPayload for this metadata type when podPreferred is true.
    std::shared_ptr<PodPayloadBase> (*createPodPayload)()    = nullptr;

    // Optional span callbacks (set by PathSpace span overloads; ignored for generic reads).
    std::function<void(void const*, std::size_t)> spanReader;
    std::function<void(void*, std::size_t)>       spanMutator;
};

} // namespace SP
