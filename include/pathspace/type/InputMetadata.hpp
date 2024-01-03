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
    T const &obj = *static_cast<std::remove_reference_t<T> const *>(objPtr);

    QueueStreamBuffer qbuf{byteQueue};
    std::ostream os{&qbuf};
    cereal::BinaryOutputArchive oarchive(os);

    oarchive(obj);
}

template<typename T>
static auto deserialize(void *objPtr, std::queue<std::byte> &byteQueue) -> void {
    T &obj = *static_cast<std::remove_reference_t<T> *>(objPtr);

    QueueStreamBuffer qbuf{byteQueue};
    std::istream is(&qbuf);
    cereal::BinaryInputArchive iarchive(is);

    iarchive(obj);
}

template<typename U>
concept SerializableAndDeserializable = !std::is_same_v<U, std::function<void()>> && 
                                        !std::is_same_v<U, void(*)()> &&
                                        !std::is_pointer<U>::value &&
                                        requires(U u, std::queue<std::byte>& q) {
    { serialize<U>(&u, q) };
    { deserialize<U>(&u, q) };
};

template<typename CVRef>
struct InputMetadataT {
    InputMetadataT() = default;
    using T = std::remove_cvref_t<CVRef>;

    static constexpr bool isTriviallyCopyable = std::is_trivially_copyable_v<T>;
    static constexpr bool isFundamental = std::is_fundamental_v<T>;
    static constexpr bool isMoveable = std::is_move_constructible_v<T>;
    static constexpr bool isCopyable = std::is_copy_constructible_v<T>;
    static constexpr bool isDefaultConstructible = std::is_default_constructible_v<T>;
    static constexpr bool isDestructible = std::is_destructible_v<T>;
    static constexpr bool isPolymorphic = std::is_polymorphic_v<T>;
    static constexpr bool isCallable = std::is_invocable_v<T>;
    static constexpr bool isFunctionPointer = std::is_function_v<std::remove_pointer_t<T>> && std::is_pointer_v<T>;
    static constexpr bool isArray = std::is_array_v<T>;
    static constexpr size_t sizeOfType = sizeof(T);
    static constexpr size_t alignmentOf = alignof(T);
    static constexpr size_t arraySize = isArray ? std::extent_v<T> : 0;

    static constexpr auto serializationFuncPtr = 
        []<typename U = T>() -> std::conditional_t<SerializableAndDeserializable<U>, decltype(&serialize<U>), nullptr_t> {
            if constexpr (SerializableAndDeserializable<U>) {
                return &serialize<std::remove_reference_t<U>>;
            } else {
                return nullptr;
            }
        }();

    static constexpr auto deserializationFuncPtr = 
        []<typename U = T>() -> std::conditional_t<SerializableAndDeserializable<U>, decltype(&deserialize<U>), nullptr_t> {
            if constexpr (SerializableAndDeserializable<U>) {
                return &deserialize<std::remove_reference_t<U>>;
            } else {
                return nullptr;
            }
        }();
};

struct InputMetadata {
    InputMetadata() = default;
    template<typename T>
    InputMetadata(InputMetadataT<T> const &obj)
        : isTriviallyCopyable(obj.isTriviallyCopyable),
          isFundamental(obj.isFundamental),
          isMoveable(obj.isMoveable),
          isCopyable(obj.isCopyable),
          isDefaultConstructible(obj.isDefaultConstructible),
          isDestructible(obj.isDestructible),
          isPolymorphic(obj.isPolymorphic),
          isCallable(obj.isCallable),
          isFunctionPointer(obj.isFunctionPointer),
          isArray(obj.isArray),
          arraySize(obj.arraySize),
          sizeOfType(obj.sizeOfType),
          alignmentOf(obj.alignmentOf),
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
    void (*deserializationFuncPtr)(void *obj, std::queue<std::byte>&) = nullptr;
};

} // namespace SP