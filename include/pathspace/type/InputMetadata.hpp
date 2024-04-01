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

/*
TODO
--------------
* Add a deserialize_read that can deserialize a single item
* Change queue to deque for serialized data so we can have random access, perhaps find internet datastructure that is better
* Add tests that const deserializes multipßle objects making sure the data doesnt change 
*/

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
static auto deserialize_fundamental(void *objPtr, std::vector<uint8_t> &bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    if (bytes.size() < sizeof(T)) {
        return;
    }
    T* obj = static_cast<T*>(objPtr);
    std::copy(bytes.begin(), bytes.begin() + sizeof(T), reinterpret_cast<uint8_t*>(obj));
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

// -----

template<typename T>
static auto serialize(void const *objPtr, ByteQueue &byteQueue) -> void {
    T const &obj = *static_cast<T const *>(objPtr);
    serialize_to_bytequeue(byteQueue, obj);
}

template<typename T>
static auto deserialize(void *objPtr, ByteQueue &byteQueue) -> void {
    T &obj = *static_cast<T*>(objPtr);

    deserialize_from_bytequeue(byteQueue, obj);
}

template<typename T>
static auto deserializeConst(void *objPtr, ByteQueue const &byteQueue) -> void {
    T &obj = *static_cast<T*>(objPtr);
    ByteQueue &bq = *const_cast<ByteQueue*>(&byteQueue);

    deserialize_from_const_bytequeue(bq, obj);
}

template<typename T>
concept SerializableAndDeserializable = !std::is_same_v<T, std::function<void()>> && 
                                        !std::is_same_v<T, void(*)()> &&
                                        !std::is_pointer<T>::value &&
                                        requires(T t, ByteQueue& q) {
    { serialize<T>(&t, q) };
    { deserialize<T>(&t, q) };
};

template<typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    static constexpr auto serializationFuncPtr = 
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&serialize<T>), std::nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &serialize<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto serializationFuncPtr2 = 
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
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&deserialize<T>), std::nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &deserialize<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtr2 = 
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
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&deserializeConst<T>), std::nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &deserializeConst<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtrConst2 = 
        []() {
            if constexpr (std::is_fundamental<T>::value) {
                return &deserialize_fundamental<T>;
            } else if constexpr (AlpacaCompatible<T>) {
                return &deserialize_alpaca<T>;
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
          serializationFuncPtr2(obj.serializationFuncPtr2),
          deserializationFuncPtr(obj.deserializationFuncPtr),
          deserializationFuncPtr2(obj.deserializationFuncPtr2),
          deserializationFuncPtrConst(obj.deserializationFuncPtrConst),
          deserializationFuncPtrConst2(obj.deserializationFuncPtrConst2)
          {}
    std::type_info const *id = nullptr;
    void (*serializationFuncPtr)(void const *obj, ByteQueue&) = nullptr;
    void (*serializationFuncPtr2)(void const *obj, std::vector<uint8_t>&) = nullptr;
    void (*deserializationFuncPtr)(void *obj, ByteQueue&) = nullptr;
    void (*deserializationFuncPtr2)(void *obj, std::vector<uint8_t>&) = nullptr;
    void (*deserializationFuncPtrConst)(void *obj, ByteQueue const&) = nullptr;
    void (*deserializationFuncPtrConst2)(void *obj, std::vector<uint8_t>&) = nullptr;
};

} // namespace SP