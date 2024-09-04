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
        : category(obj.category), typeInfo(obj.typeInfo), serialize(obj.serialize), deserializePop(obj.deserializePop), deserialize(obj.deserialize),
          deserializeFunctionPointer(obj.deserializeFunctionPointer), executeFunctionPointer(obj.executeFunctionPointer) {
    }
    DataCategory category;
    std::type_info const* typeInfo = nullptr;
    void (*serialize)(void const* obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
    void (*serializeFunctionPointer)(void const* obj, std::vector<SERIALIZATION_TYPE>&, std::optional<ExecutionOptions> const& executionOptions) = nullptr;
    void (*deserialize)(void* obj, std::vector<SERIALIZATION_TYPE> const&) = nullptr;
    void (*deserializePop)(void* obj, std::vector<SERIALIZATION_TYPE>&) = nullptr;
    void (*deserializeFunctionPointer)(void* obj, std::vector<SERIALIZATION_TYPE> const&) = nullptr;
    void (*executeFunctionPointer)(void* const functionPointer, void* returnData, TaskPool* pool) = nullptr;
};

} // namespace SP