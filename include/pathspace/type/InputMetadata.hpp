#pragma once
#include <cstddef>
#include <functional>
#include <type_traits>
#include <typeinfo>
#include <queue>

#include "pathspace/serialization/QueueStreamBuffer.hpp"

#include <cereal/types/string.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/queue.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/map.hpp>

namespace SP {

template<typename T>
static auto serialize(void const *objPtr, std::queue<std::byte> &byteQueue) -> void {
    T const &obj = *static_cast<T const *>(objPtr);

    QueueStreamBuffer qbuf{byteQueue};
    std::ostream os{&qbuf};
    cereal::BinaryOutputArchive oarchive(os);

    oarchive(obj);
}

template<typename T>
static auto deserialize(void *objPtr, std::queue<std::byte> const &byteQueue) -> void {
    T &obj = *static_cast<T*>(objPtr);
    auto *bq = const_cast<std::queue<std::byte>*>(&byteQueue);

    QueueStreamBuffer qbuf{*bq};
    std::istream is(&qbuf);
    cereal::BinaryInputArchive iarchive(is);

    iarchive(obj);
}

template<typename T>
concept SerializableAndDeserializable = !std::is_same_v<T, std::function<void()>> && 
                                        !std::is_same_v<T, void(*)()> &&
                                        !std::is_pointer<T>::value &&
                                        requires(T t, std::queue<std::byte>& q) {
    { serialize<T>(&t, q) };
    { deserialize<T>(&t, q) };
};

template<typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;
    static constexpr auto serializationFuncPtr = 
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&serialize<T>), nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &serialize<T>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtr = 
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&deserialize<T>), nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &deserialize<T>;
            } else {
                return nullptr;
            }
        }();
};

struct InputMetadata {
    InputMetadata() = default;
    template<typename CVRefT, typename T = std::remove_cvref_t<CVRefT>>
    InputMetadata(InputMetadataT<CVRefT> const &obj)
        : isTriviallyCopyable(std::is_trivially_copyable_v<T>),
          isFundamental(std::is_fundamental_v<T>),
          isMoveable(std::is_move_constructible_v<T>),
          isCopyable(std::is_copy_constructible_v<T>),
          isDefaultConstructible(std::is_default_constructible_v<T>),
          isDestructible(std::is_destructible_v<T>),
          isPolymorphic(std::is_polymorphic_v<T>),
          isCallable(std::is_invocable_v<T>),
          isFunctionPointer(std::is_function_v<std::remove_pointer_t<T>> && std::is_pointer_v<T>),
          isArray(std::is_array_v<T>),
          arraySize(isArray ? std::extent_v<T> : 0),
          sizeOfType(sizeof(T)),
          alignmentOf(alignof(T)),
          id(&typeid(T)),
          serializationFuncPtr(obj.serializationFuncPtr),
          deserializationFuncPtr(obj.deserializationFuncPtr)
          {}

    bool isTriviallyCopyable = false;
    bool isFundamental = false;
    bool isMoveable = false;
    bool isCopyable = false;
    bool isDefaultConstructible = false;
    bool isDestructible = false;
    bool isPolymorphic = false;
    bool isCallable = false;
    bool isFunctionPointer = false;
    bool isArray = false;
    size_t sizeOfType = 0;
    size_t alignmentOf = 0;
    size_t arraySize = 0;
    std::type_info const *id = nullptr;
    void (*serializationFuncPtr)(void const *obj, std::queue<std::byte>&) = nullptr;
    void (*deserializationFuncPtr)(void *obj, std::queue<std::byte> const&) = nullptr;
};

} // namespace SP