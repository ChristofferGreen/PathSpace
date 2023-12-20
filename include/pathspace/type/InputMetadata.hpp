#pragma once
#include <functional>

namespace SP {

struct InputMetadata {
    InputMetadata() = default;
    template<typename T>
    InputMetadata(T const&) 
        : isTriviallyCopyable(std::is_trivially_copyable<T>::value),
          isFundamental(std::is_fundamental<T>::value),
          isReference(std::is_reference<T>::value),
          isMoveable(this->isReference && std::is_move_constructible<T>::value),
          isCopyable(std::is_copy_constructible<T>::value),
          isDefaultConstructible(std::is_default_constructible<T>::value),
          isDestructible(std::is_destructible<T>::value),
          isPolymorphic(std::is_polymorphic<T>::value),
          isCallable(std::is_invocable<T>::value),
          isFunctionPointer(std::is_function<std::remove_pointer_t<T>>::value && std::is_pointer<T>::value),
          isArray(std::is_array<T>::value),
          arraySize(isArray ? std::extent<T>::value : 0),
          sizeOfType(sizeof(T)),
          alignmentOf(std::alignment_of<T>::value)
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
private:
    bool isReference = false; // Does not fully work
};

} // namespace SP