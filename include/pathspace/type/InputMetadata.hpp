#pragma once
#include <cstddef>
#include <functional>
#include <type_traits>
#include <typeinfo>
#include <queue>

#include "pathspace/utils/ByteQueue.hpp"
#include "pathspace/utils/ByteQueueSerializer.hpp"

#include <alpaca/alpaca.h>

#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/queue.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/map.hpp>

namespace SP {

template<typename T>
concept AlpacaCompatible = !std::is_same_v<T, std::function<void()>> && 
                                        !std::is_same_v<T, void(*)()> &&
                                        !std::is_pointer<T>::value &&
                                        requires(T t, std::vector<uint8_t>& v) {
    { serialize_alpaca<T>(&t, v) };
    { deserialize_alpaca<T>(&t, v) };
};

template<typename T>
static auto serialize_alpaca(void const *objPtr, std::vector<uint8_t> &bytes) -> void {
    T const &obj = *static_cast<T const *>(objPtr);
    alpaca::serialize(obj, bytes);
}

template<typename T>
static auto deserialize_alpaca(void *objPtr, std::vector<uint8_t> &bytes) -> void {
    T &obj = *static_cast<T*>(objPtr);
    std::error_code ec;
    auto nbrBytesRead = alpaca::deserialize<T>(obj, bytes, ec);
    if (!ec) {
        if (nbrBytesRead <= bytes.size()) {
            bytes.erase(bytes.begin(), bytes.begin() + nbrBytesRead);
        }
    }
}

template<typename T>
static auto deserialize_alpaca_const(void *objPtr, std::vector<uint8_t> const &bytes) -> void {
    T &obj = *static_cast<T*>(objPtr);
    std::error_code ec;
    alpaca::deserialize<T>(obj, bytes, ec);
}

template<typename T>
static auto serialize_fundamental(void const *objPtr, std::vector<uint8_t> &bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    T const &obj = *static_cast<T const *>(objPtr);
    auto const *begin = reinterpret_cast<uint8_t const *>(&obj);
    auto const *end = begin + sizeof(T);
    bytes.insert(bytes.end(), begin, end);
}

template<typename T>
static auto deserialize_fundamental_const(void *objPtr, std::vector<uint8_t> const &bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    if (bytes.size() < sizeof(T)) {
        return;
    }
    T* obj = static_cast<T*>(objPtr);
    std::copy(bytes.begin(), bytes.begin() + sizeof(T), reinterpret_cast<uint8_t*>(obj));
}

template<typename T>
static auto deserialize_fundamental(void *objPtr, std::vector<uint8_t> &bytes) -> void {
    deserialize_fundamental_const<T>(objPtr, bytes);
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

template<typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    static constexpr auto serializationFuncPtr = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &serialize_fundamental<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &serialize_alpaca<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtr = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &deserialize_fundamental<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &deserialize_alpaca<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtrConst = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &deserialize_fundamental_const<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &deserialize_alpaca_const<T>;
            } else {
                return nullptr;
            }
        }();
};

struct InputMetadata {
    InputMetadata() = default;
    template<typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const &obj)
        : id(&typeid(T)),
          serializationFuncPtr(obj.serializationFuncPtr),
          deserializationFuncPtr(obj.deserializationFuncPtr),
          deserializationFuncPtrConst(obj.deserializationFuncPtrConst)
          {}
    std::type_info const *id = nullptr;
    void (*serializationFuncPtr)(void const *obj, std::vector<uint8_t>&) = nullptr;
    void (*deserializationFuncPtr)(void *obj, std::vector<uint8_t>&) = nullptr;
    void (*deserializationFuncPtrConst)(void *obj, std::vector<uint8_t> const&) = nullptr;
};

} // namespace SP