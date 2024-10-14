#pragma once
#include "core/ExecutionOptions.hpp"
#include "pathspace/taskpool/TaskPool.hpp"
#include "type/helpers/return_type.hpp"

#include <cassert>

#define SERIALIZATION_TYPE uint8_t
#include <alpaca/alpaca.h>

namespace SP {
struct PathSpace;

template <typename T>
concept AlpacaCompatible = !std::is_same_v<T, std::function<void()>> && !std::is_same_v<T, void (*)()> && !std::is_pointer<T>::value
                           && requires(T t, std::vector<uint8_t>& v) {
                                  { serialize_alpaca<T>(&t, v) };
                                  { deserialize_alpaca<T>(&t, v) };
                              };

template <typename T>
static auto serialize_alpaca(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    T const& obj = *static_cast<T const*>(objPtr);
    alpaca::serialize(obj, bytes);
}

template <typename T>
static auto deserialize_alpaca(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    T& obj = *static_cast<T*>(objPtr);
    std::error_code ec;
    auto nbrBytesRead = alpaca::deserialize<T>(obj, bytes, ec);
    if (!ec) {
        if (nbrBytesRead <= bytes.size()) {
            bytes.erase(bytes.begin(), bytes.begin() + nbrBytesRead);
        }
    }
}

template <typename T>
static auto deserialize_alpaca_const(void* objPtr, std::vector<uint8_t> const& bytes) -> void {
    T& obj = *static_cast<T*>(objPtr);
    std::error_code ec;
    alpaca::deserialize<T>(obj, bytes, ec);
}

template <typename T>
static auto serialize_fundamental(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    T const& obj = *static_cast<T const*>(objPtr);
    auto const* begin = reinterpret_cast<uint8_t const*>(&obj);
    auto const* end = begin + sizeof(T);
    bytes.insert(bytes.end(), begin, end);
}

template <typename T>
static auto deserialize_fundamental_const(void* objPtr, std::vector<uint8_t> const& bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    if (bytes.size() < sizeof(T)) {
        return;
    }
    T* obj = static_cast<T*>(objPtr);
    std::copy(bytes.begin(), bytes.begin() + sizeof(T), reinterpret_cast<uint8_t*>(obj));
}

template <typename T>
static auto deserialize_fundamental(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    deserialize_fundamental_const<T>(objPtr, bytes);
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

static auto serialize_std_function(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
}

static auto serialize_function_pointer(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    auto funcPtr = *static_cast<void (**)()>(const_cast<void*>(objPtr));
    auto funcPtrInt = reinterpret_cast<std::uintptr_t>(funcPtr);
    auto const* begin = reinterpret_cast<uint8_t const*>(&funcPtrInt);
    auto const* end = begin + sizeof(funcPtrInt);
    bytes.insert(bytes.end(), begin, end);
}

static auto deserialize_function_pointer(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<void (*)()>(funcPtrInt);
    *static_cast<void (**)()>(objPtr) = funcPtr;
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t));
}

static auto deserialize_function_pointer_const(void* objPtr, std::vector<uint8_t> const& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<void (*)()>(funcPtrInt);
    *static_cast<void (**)()>(objPtr) = funcPtr;
}

static auto deserialize_std_function(void* objPtr, std::vector<uint8_t>& bytes) -> void {
}

static auto deserialize_std_function_const(void* objPtr, std::vector<uint8_t>& bytes) -> void {
}

template <typename T>
static auto deserialize_function_pointer_ret(void* objPtr, std::vector<uint8_t> const& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<T (*)()>(funcPtrInt);
    *static_cast<T (**)()>(objPtr) = funcPtr;
}

template <typename T>
static auto execute_function_pointer(void* const functionPointer, void* returnData, TaskPool* pool) {
    auto exec = [](void* const functionPointer, void* returnData) -> void {
        T* ret = static_cast<T*>(returnData);
        auto funcPtr = reinterpret_cast<T (*)()>(functionPointer);
        *ret = funcPtr();
    };
    if (pool == nullptr) { // Execute in caller thread
        exec(functionPointer, returnData);
    } else {
        // pool->addTask(exec, functionPointer, returnData);
    }
}

template <typename T>
static auto execute_std_function(void* const function, void* returnData, TaskPool* pool) {
}

class PathSpace;

template <typename T>
concept FunctionPointer = requires {
    requires std::is_pointer_v<T>;
    requires std::is_function_v<std::remove_pointer_t<T>>;
};

template <typename T>
concept ExecutionFunctionPointer
        = (std::is_function_v<std::remove_pointer_t<T>> || std::is_member_function_pointer_v<T> || requires(T& t) { +t; })
          && // Unary plus operator, which attempts to convert to function pointer
          requires(T t) {
              t(); // Can be called with no arguments
          };

template <typename T>
concept ExecutionStdFunction = requires(T f) {
    requires std::is_invocable_v<T>;
    requires !std::is_pointer_v<T>;               // Exclude raw function pointers
    requires !std::is_same_v<std::decay_t<T>, T>; // Exclude objects with operator()
};

template <typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    std::type_info const* typeInfo = &typeid(T);
    std::type_info const* returnTypeInfo = ReturnTypeInfo<T>;

    static constexpr DataCategory const category = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return DataCategory::ExecutionFunctionPointer;
        } else if constexpr (FunctionPointer<T>) {
            return DataCategory::FunctionPointer;
        } else if constexpr (ExecutionStdFunction<T>) {
            return DataCategory::ExecutionStdFunction;
        } else if constexpr (std::is_fundamental<T>::value) {
            return DataCategory::Fundamental;
        } else {
            return DataCategory::None;
        }
    }();

    static constexpr auto serialize = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return &serialize_function_pointer;
        } else if constexpr (FunctionPointer<T>) {
            return &serialize_function_pointer;
        } else if constexpr (ExecutionStdFunction<T>) {
            return &serialize_std_function;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &serialize_fundamental<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &serialize_alpaca<T>;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserializePop = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return deserialize_function_pointer;
        } else if constexpr (FunctionPointer<T>) {
            return &deserialize_function_pointer;
        } else if constexpr (ExecutionStdFunction<T>) {
            return &deserialize_std_function;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &deserialize_fundamental<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &deserialize_alpaca<T>;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserialize = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return deserialize_function_pointer_const;
        } else if constexpr (FunctionPointer<T>) {
            return &deserialize_function_pointer_const;
        } else if constexpr (ExecutionStdFunction<T>) {
            return &deserialize_std_function_const;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &deserialize_fundamental_const<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &deserialize_alpaca_const<T>;
        } else {
            return nullptr;
        }
    }();
};

} // namespace SP
