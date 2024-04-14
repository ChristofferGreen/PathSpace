#pragma once
#include <functional>
#include <cassert>
#ifdef USE_ALPACA
#define SERIALIZATION_TYPE uint8_t
#include <alpaca/alpaca.h>

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

    static constexpr auto serialize = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &serialize_fundamental<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &serialize_alpaca<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializePop = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &deserialize_fundamental<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &deserialize_alpaca<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserialize = 
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

} // namespace SP
#endif // USE_ALPACA

#ifdef USE_GLAZE
#include "glaze/glaze.hpp"
#define SERIALIZATION_TYPE std::byte
namespace SP {

template<typename T>
static auto serialize(void const *objPtr, std::vector<std::byte> &bytes) -> void {
    T const &obj = *static_cast<T const *>(objPtr);
    glz::write_binary_untagged(obj, bytes);
}

template<typename T>
static auto deserialize_const(void *objPtr, std::vector<std::byte> const &bytes) -> void {
    T* obj = static_cast<T*>(objPtr);
    auto error = glz::read_binary_untagged(obj, bytes);
    assert(!error);
}

template<typename T>
static auto deserialize(void *objPtr, std::vector<std::byte> &bytes) -> void {
    deserialize_const<T>(objPtr, bytes);
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

template<typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    static constexpr auto serialize = 
        []() {
            return &serialize<T>;
        }();

    static constexpr auto deserializePop = 
        []() {
            return &deserialize<T>;
        }();

    static constexpr auto deserialize = 
        []() {
            return &deserialize_const<T>;
        }();
};

} // namespace SP
#endif // USE_GLAZE