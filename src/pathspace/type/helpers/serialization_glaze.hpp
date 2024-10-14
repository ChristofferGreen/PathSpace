#pragma once
#include "core/ExecutionOptions.hpp"
#include "pathspace/taskpool/TaskPool.hpp"
#include "type/helpers/return_type.hpp"

#include <cassert>

#include "glaze/glaze.hpp"
#define SERIALIZATION_TYPE std::byte
namespace SP {

template <typename T>
static auto serialize_glaze(void const* objPtr, std::vector<std::byte>& bytes) -> void {
    T const& obj = *static_cast<T const*>(objPtr);
    std::vector<std::byte> tmp;
    glz::write_binary_untagged(obj, tmp);
    bytes.insert(bytes.end(), tmp.begin(), tmp.end());
}

template <typename T>
static auto deserialize_glaze(void* objPtr, std::vector<std::byte> const& bytes) -> void {
    T* obj = static_cast<T*>(objPtr);
    auto error = glz::read_binary_untagged(obj, bytes);
    assert(!error);
}

template <typename T>
static auto deserialize_pop_glaze(void* objPtr, std::vector<std::byte>& bytes) -> void {
    deserialize_glaze<T>(objPtr, bytes);
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

template <typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    static constexpr auto serialize = &serialize_glaze<T>;
    static constexpr auto deserialize = &deserialize_glaze<T>;
    static constexpr auto deserializePop = &deserialize_pop_glaze<T>;
};

} // namespace SP