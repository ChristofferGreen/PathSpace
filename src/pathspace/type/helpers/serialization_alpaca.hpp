#pragma once
#include "type/DataCategory.hpp"
#include "type/ExecutionCategory.hpp"
#include "type/Serializer.hpp"
#include "type/SlidingBuffer.hpp"
#include "utils/TaggedLogger.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <vector>

#define SERIALIZATION_TYPE uint8_t
#include <alpaca/alpaca.h>

namespace SP {
struct PathSpace;

// ########### Alpaca Serialization ###########

template <typename T>
static auto serialize_alpaca(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    struct Wrapper {
        T wrappedObject;
    };
    Wrapper wrapper{*static_cast<T const*>(objPtr)};

    try {
        // Serialize the object
        std::vector<uint8_t> tempBytes;
        size_t bytesWritten = alpaca::serialize<Wrapper, 1>(wrapper, tempBytes);

        // Store the size of the serialized data
        uint32_t size = static_cast<uint32_t>(bytesWritten);
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + sizeof(size));

        // Append the serialized data
        bytes.insert(bytes.end(), tempBytes.begin(), tempBytes.begin() + bytesWritten);

        sp_log("Object serialized successfully", "INFO");
    } catch (const std::exception& e) {
        sp_log("Serialization failed: " + std::string(e.what()), "ERROR");
        throw;
    }
}

template <typename T>
static auto serialize_alpaca2(void const* objPtr, SP::SlidingBuffer& buffer) -> void {
    try {
        SP::Serializer<T>::serialize(*static_cast<T const*>(objPtr), buffer);
        sp_log("Object serialized successfully", "INFO");
    } catch (const std::exception& e) {
        sp_log("Serialization failed: " + std::string(e.what()), "ERROR");
        throw;
    }
}

template <typename T>
static auto deserialize_alpaca_pop2(void* objPtr, SP::SlidingBuffer& buffer) -> void {
    bool const doPop = true;
    auto expected = SP::Serializer<T>::deserializePop(buffer);
    if (expected.has_value())
        *static_cast<T*>(objPtr) = std::move(expected.value());
    else
        sp_log("Deserialization failed: " + expected.error().message.value_or(""), "ERROR");
}

template <typename T>
static auto deserialize_alpaca_pop(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    if (bytes.size() < sizeof(uint32_t)) {
        sp_log("Not enough data to read size", "ERROR");
        throw std::runtime_error("Not enough data to read size");
    }

    uint32_t size;
    std::memcpy(&size, bytes.data(), sizeof(uint32_t));

    if (bytes.size() < sizeof(uint32_t) + size) {
        sp_log("Not enough data to deserialize object", "ERROR");
        throw std::runtime_error("Not enough data to deserialize object");
    }

    struct Wrapper {
        T wrappedObject;
    };

    std::error_code ec;
    std::vector<uint8_t> deserializeBytes(bytes.begin() + sizeof(uint32_t), bytes.begin() + sizeof(uint32_t) + size);
    auto wrapper = alpaca::deserialize<Wrapper, 1>(deserializeBytes, ec);

    if (ec) {
        sp_log("Deserialization failed: " + ec.message(), "ERROR");
        throw std::runtime_error("Deserialization failed: " + ec.message());
    }

    // Copy the deserialized object to the output
    *static_cast<T*>(objPtr) = std::move(wrapper.wrappedObject);

    // Remove the read data from the input vector
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(uint32_t) + size);

    sp_log("Object deserialized successfully", "INFO");
}

template <typename T>
static auto deserialize_alpaca_const2(void* objPtr, SP::SlidingBuffer const& buffer) -> void {
    auto expected = SP::Serializer<T>::deserialize(buffer);
    if (expected.has_value())
        *static_cast<T*>(objPtr) = std::move(expected.value());
    else
        sp_log("Deserialization failed: " + expected.error().message.value_or(""), "ERROR");
}

template <typename T>
static auto deserialize_alpaca_const(void* objPtr, std::vector<uint8_t> const& bytes) -> void {
    if (bytes.size() < sizeof(uint32_t)) {
        sp_log("Not enough data to read size", "ERROR");
        throw std::runtime_error("Not enough data to read size");
    }

    uint32_t size;
    std::memcpy(&size, bytes.data(), sizeof(uint32_t));

    if (bytes.size() < sizeof(uint32_t) + size) {
        sp_log("Not enough data to deserialize object", "ERROR");
        throw std::runtime_error("Not enough data to deserialize object");
    }

    struct Wrapper {
        T wrappedObject;
    };

    std::error_code ec;
    std::vector<uint8_t> deserializeBytes(bytes.begin() + sizeof(uint32_t), bytes.begin() + sizeof(uint32_t) + size);
    auto wrapper = alpaca::deserialize<Wrapper, 1>(deserializeBytes, ec);

    if (ec) {
        sp_log("Deserialization failed: " + ec.message(), "ERROR");
        throw std::runtime_error("Deserialization failed: " + ec.message());
    }

    // Copy the deserialized object to the output
    *static_cast<T*>(objPtr) = std::move(wrapper.wrappedObject);

    sp_log("Object deserialized successfully", "INFO");
}

template <typename T>
concept AlpacaCompatible = !std::is_pointer_v<T> && requires(T t, std::vector<uint8_t>& v) {
    { serialize_alpaca<T>(static_cast<void const*>(&t), v) };
    { deserialize_alpaca_const<T>(static_cast<void*>(&t), v) };
};

// ########### Fundamental Datatype Serialization ###########

template <typename T>
static auto serialize_fundamental(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    T const& obj = *static_cast<T const*>(objPtr);
    auto const* begin = reinterpret_cast<uint8_t const*>(&obj);
    auto const* end = begin + sizeof(T);
    bytes.insert(bytes.end(), begin, end);
}

template <typename T>
static auto serialize_fundamental2(void const* objPtr, SP::SlidingBuffer& bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    T const& obj = *static_cast<T const*>(objPtr);
    auto const* begin = reinterpret_cast<uint8_t const*>(&obj);
    auto const* end = begin + sizeof(T);
    // bytes.insert(bytes.end(), begin, end);
    bytes.append(begin, sizeof(T));
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
static auto deserialize_fundamental_const2(void* objPtr, SP::SlidingBuffer const& bytes) -> void {
    static_assert(std::is_fundamental_v<T>, "T must be a fundamental type");
    if (bytes.size() < sizeof(T)) {
        return;
    }
    T* obj = static_cast<T*>(objPtr);
    std::copy(bytes.data(), bytes.data() + sizeof(T), reinterpret_cast<uint8_t*>(obj));
}

template <typename T>
static auto deserialize_fundamental_pop(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    deserialize_fundamental_const<T>(objPtr, bytes);
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(T));
}

template <typename T>
static auto deserialize_fundamental_pop2(void* objPtr, SP::SlidingBuffer& bytes) -> void {
    deserialize_fundamental_const2<T>(objPtr, bytes);
    bytes.advance(sizeof(T));
}

static auto serialize_function_pointer(void const* objPtr, std::vector<uint8_t>& bytes) -> void {
    auto funcPtr = *static_cast<void (**)()>(const_cast<void*>(objPtr));
    auto funcPtrInt = reinterpret_cast<std::uintptr_t>(funcPtr);
    auto const* begin = reinterpret_cast<uint8_t const*>(&funcPtrInt);
    auto const* end = begin + sizeof(funcPtrInt);
    bytes.insert(bytes.end(), begin, end);
}

static auto serialize_function_pointer2(void const* objPtr, SP::SlidingBuffer& bytes) -> void {
    auto funcPtr = *static_cast<void (**)()>(const_cast<void*>(objPtr));
    auto funcPtrInt = reinterpret_cast<std::uintptr_t>(funcPtr);
    bytes.append(reinterpret_cast<uint8_t const*>(&funcPtrInt), sizeof(funcPtrInt));
}

static auto deserialize_function_pointer_pop(void* objPtr, std::vector<uint8_t>& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<void (*)()>(funcPtrInt);
    *static_cast<void (**)()>(objPtr) = funcPtr;
    bytes.erase(bytes.begin(), bytes.begin() + sizeof(std::uintptr_t));
}

static auto deserialize_function_pointer_pop2(void* objPtr, SP::SlidingBuffer& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.data(), bytes.data() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<void (*)()>(funcPtrInt);
    *static_cast<void (**)()>(objPtr) = funcPtr;
    bytes.advance(sizeof(std::uintptr_t));
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

static auto deserialize_function_pointer_const2(void* objPtr, SP::SlidingBuffer const& bytes) -> void {
    if (bytes.size() < sizeof(std::uintptr_t)) {
        return;
    }
    std::uintptr_t funcPtrInt;
    std::copy(bytes.data(), bytes.data() + sizeof(std::uintptr_t), reinterpret_cast<uint8_t*>(&funcPtrInt));
    auto funcPtr = reinterpret_cast<void (*)()>(funcPtrInt);
    *static_cast<void (**)()>(objPtr) = funcPtr;
}

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

template <typename T, typename R = void>
concept ExecutionStdFunction = requires(T f) { requires std::is_convertible_v<T, std::function<R()>>; };

template <typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    InputMetadataT() = default;

    static constexpr std::type_info const* typeInfo = []() {
        if constexpr (ExecutionFunctionPointer<T> || ExecutionStdFunction<T>) {
            return &typeid(std::invoke_result_t<T>);
        }
        return &typeid(T);
    }();

    static constexpr DataCategory const dataCategory = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return DataCategory::Execution;
        } else if constexpr (FunctionPointer<T>) {
            return DataCategory::FunctionPointer;
        } else if constexpr (ExecutionStdFunction<T>) {
            return DataCategory::Execution;
        } else if constexpr (std::is_fundamental<T>::value) {
            return DataCategory::Fundamental;
        } else if constexpr (AlpacaCompatible<T>) {
            return DataCategory::SerializationLibraryCompatible;
        } else {
            return DataCategory::None;
        }
    }();

    static constexpr ExecutionCategory const executionCategory = []() {
        if constexpr (ExecutionFunctionPointer<T>) {
            return ExecutionCategory::FunctionPointer;
        } else if constexpr (ExecutionStdFunction<T>) {
            return ExecutionCategory::StdFunction;
        } else {
            return ExecutionCategory::None;
        }
    }();

    static constexpr auto serialize = []() {
        if constexpr (ExecutionFunctionPointer<T> || FunctionPointer<T>) {
            return &serialize_function_pointer2;
        } else if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &serialize_fundamental2<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &serialize_alpaca2<T>;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserialize = []() -> void (*)(void* obj, SP::SlidingBuffer const&) {
        if constexpr (ExecutionFunctionPointer<T> || FunctionPointer<T>) {
            return deserialize_function_pointer_const2;
        } else if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (FunctionPointer<T>) {
            return &deserialize_function_pointer_const2;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &deserialize_fundamental_const2<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &deserialize_alpaca_const2<T>;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserializePop = []() {
        if constexpr (ExecutionFunctionPointer<T> || FunctionPointer<T>) {
            return &deserialize_function_pointer_pop2;
        } else if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (FunctionPointer<T>) {
            return nullptr;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &deserialize_fundamental_pop2<T>;
        } else if constexpr (AlpacaCompatible<T>) {
            return &deserialize_alpaca_pop2<T>;
        } else {
            return nullptr;
        }
    }();
};

} // namespace SP
