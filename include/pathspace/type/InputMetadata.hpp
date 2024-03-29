#pragma once
#include <cstddef>
#include <functional>
#include <type_traits>
#include <typeinfo>
#include <queue>

#include "pathspace/utils/ByteQueue.hpp"
#include "pathspace/utils/ByteQueueSerializer.hpp"

#include "glaze/glaze.hpp"

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
* Add tests that const deserializes multiple objects making sure the data doesnt change 
*/

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

    static constexpr auto deserializationFuncPtr = 
        []() -> std::conditional_t<SerializableAndDeserializable<T>, decltype(&deserialize<T>), std::nullptr_t> {
            if constexpr (SerializableAndDeserializable<T>) {
                return &deserialize<T>;
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
    void (*serializationFuncPtr)(void const *obj, ByteQueue&) = nullptr;
    void (*deserializationFuncPtr)(void *obj, ByteQueue&) = nullptr;
    void (*deserializationFuncPtrConst)(void *obj, ByteQueue const&) = nullptr;
};

} // namespace SP