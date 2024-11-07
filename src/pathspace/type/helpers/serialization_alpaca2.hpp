#pragma once
#include "type/DataCategory.hpp"
#include "type/ExecutionCategory.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/serialization.hpp"
#include "utils/TaggedLogger.hpp"

#include <concepts>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <type_traits>

namespace SP {

// ########### Type Detection Concepts ###########

template <typename T>
concept FundamentalType = std::is_fundamental_v<T>;

template <typename T>
concept FunctionPointerType
        = std::is_function_v<std::remove_pointer_t<std::decay_t<T>>> || std::is_member_function_pointer_v<std::decay_t<T>>;

template <typename T>
concept StdFunctionType = requires(T t) {
    t();
    typename T::result_type;
};

template <typename T>
concept ExecutableType = requires(T& t) {
    +t;  // Must support unary plus for function conversion
    t(); // Must be callable
};

template <typename T>
concept ExecutionFunctionPointer = (FunctionPointerType<T> || requires(T& t) { +t; }) && requires(T t) { t(); };

template <typename T, typename R = void>
concept ExecutionStdFunction = requires(T f) { requires std::is_convertible_v<T, std::function<R()>>; };

template <typename T>
concept AlpacaCompatible = !std::is_pointer_v<T> && requires(T t, SlidingBuffer& buffer) { SP::serialize<T>(t, buffer); };

// Lambda detection type trait
template <typename T>
struct is_lambda {
private:
    template <typename U>
    static std::true_type test(decltype(&U::operator())*);

    template <typename U>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<T>(nullptr))::value && !std::is_function_v<T> && !std::is_member_function_pointer_v<T>;
};

template <typename T>
inline constexpr bool is_lambda_v = is_lambda<std::remove_cvref_t<T>>::value;

// ########### Serialization Helpers ###########

template <typename T>
struct PointerSerializationHelper {
    static void serialize(const void* obj, SlidingBuffer& buffer) {
        auto ptr = *static_cast<T* const*>(const_cast<void*>(obj));
        auto ptrInt = reinterpret_cast<std::uintptr_t>(ptr);
        buffer.append(reinterpret_cast<const uint8_t*>(&ptrInt), sizeof(ptrInt));
    }

    static void deserialize(void* obj, const SlidingBuffer& buffer) {
        if (buffer.size() < sizeof(std::uintptr_t)) {
            throw std::runtime_error("Buffer too small for pointer");
        }
        std::uintptr_t ptrInt;
        std::memcpy(&ptrInt, buffer.data(), sizeof(ptrInt));
        *static_cast<T**>(obj) = reinterpret_cast<T*>(ptrInt);
    }

    static void deserializePop(void* obj, SlidingBuffer& buffer) {
        deserialize(obj, buffer);
        buffer.advance(sizeof(std::uintptr_t));
    }
};

template <typename T>
struct ValueSerializationHelper {
    static void serialize(const void* obj, SlidingBuffer& buffer) {
        if constexpr (is_lambda_v<T>) {
            // Lambdas are not directly serializable
            throw std::runtime_error("Cannot serialize lambda directly");
        } else if constexpr (FundamentalType<T>) {
            buffer.append(reinterpret_cast<const uint8_t*>(obj), sizeof(T));
        } else if constexpr (AlpacaCompatible<T>) {
            try {
                auto* typed_obj = static_cast<const T*>(obj);
                SP::serialize<T>(*typed_obj, buffer);
            } catch (const std::exception& e) {
                sp_log("Serialization failed: " + std::string(e.what()), "ERROR");
                throw;
            }
        }
    }

    static void deserialize(void* obj, const SlidingBuffer& buffer) {
        if constexpr (is_lambda_v<T> || StdFunctionType<T> || ExecutableType<T>) {
            throw std::runtime_error("Cannot deserialize lambda or function object");
        } else if constexpr (FundamentalType<T>) {
            if (buffer.size() < sizeof(T)) {
                throw std::runtime_error("Buffer too small");
            }
            std::memcpy(obj, buffer.data(), sizeof(T));
        } else if constexpr (AlpacaCompatible<T>) {
            auto* typed_obj = static_cast<T*>(obj);
            auto result = SP::deserialize<T>(buffer);
            if (!result) {
                throw std::runtime_error(result.error().message.value_or("Deserialization failed"));
            }
            if constexpr (std::is_copy_assignable_v<T> || std::is_move_assignable_v<T>) {
                *typed_obj = std::move(*result);
            } else {
                throw std::runtime_error("Type is not assignable");
            }
        }
    }

    static void deserializePop(void* obj, SlidingBuffer& buffer) {
        deserialize(obj, buffer);
        if constexpr (FundamentalType<T>) {
            buffer.advance(sizeof(T));
        }
    }
};

template <typename T>
struct FunctionSerializationHelper {
    static void serialize(const void* obj, SlidingBuffer& buffer) {
        auto funcPtr = *static_cast<T* const*>(const_cast<void*>(obj));
        auto funcPtrInt = reinterpret_cast<std::uintptr_t>(funcPtr);
        buffer.append(reinterpret_cast<const uint8_t*>(&funcPtrInt), sizeof(funcPtrInt));
    }

    static void deserialize(void* obj, const SlidingBuffer& buffer) {
        if (buffer.size() < sizeof(std::uintptr_t)) {
            throw std::runtime_error("Buffer too small for function pointer");
        }
        std::uintptr_t funcPtrInt;
        std::memcpy(&funcPtrInt, buffer.data(), sizeof(funcPtrInt));
        *static_cast<T**>(obj) = reinterpret_cast<T*>(funcPtrInt);
    }

    static void deserializePop(void* obj, SlidingBuffer& buffer) {
        deserialize(obj, buffer);
        buffer.advance(sizeof(std::uintptr_t));
    }
};

// ########### Serialization Traits ###########

template <typename T>
struct SerializationTraits {
    static constexpr DataCategory category = []() {
        if constexpr (is_lambda_v<T>)
            return DataCategory::Execution;
        else if constexpr (FundamentalType<T>)
            return DataCategory::Fundamental;
        else if constexpr (FunctionPointerType<T>)
            return DataCategory::FunctionPointer;
        else if constexpr (StdFunctionType<T> || ExecutableType<T>)
            return DataCategory::Execution;
        else if constexpr (AlpacaCompatible<T>)
            return DataCategory::SerializationLibraryCompatible;
        else if constexpr (std::is_pointer_v<T>)
            return DataCategory::FunctionPointer;
        else
            return DataCategory::None;
    }();

    static constexpr ExecutionCategory executionCategory = []() {
        if constexpr (is_lambda_v<T>)
            return ExecutionCategory::StdFunction;
        else if constexpr (FunctionPointerType<T> || ExecutionFunctionPointer<T>)
            return ExecutionCategory::FunctionPointer;
        else if constexpr (StdFunctionType<T> || ExecutionStdFunction<T>)
            return ExecutionCategory::StdFunction;
        else if constexpr (ExecutableType<T>)
            return ExecutionCategory::StdFunction;
        else
            return ExecutionCategory::None;
    }();

    static constexpr std::type_info const* typeInfo = []() {
        if constexpr (is_lambda_v<T> || ExecutableType<T> || ExecutionFunctionPointer<T> || ExecutionStdFunction<T>)
            return &typeid(std::invoke_result_t<T>);
        else if constexpr (std::is_pointer_v<T>)
            return &typeid(std::remove_pointer_t<T>);
        else
            return &typeid(T);
    }();

    static constexpr auto serialize = []() {
        if constexpr (is_lambda_v<T>)
            return nullptr; // Lambdas should be handled by the task system
        else if constexpr (FunctionPointerType<T> || ExecutionFunctionPointer<T>)
            return &FunctionSerializationHelper<T>::serialize;
        else if constexpr (std::is_pointer_v<T>)
            return &PointerSerializationHelper<std::remove_pointer_t<T>>::serialize;
        else
            return &ValueSerializationHelper<T>::serialize;
    }();

    static constexpr auto deserialize = []() {
        if constexpr (is_lambda_v<T>)
            return nullptr; // Lambdas should be handled by the task system
        else if constexpr (FunctionPointerType<T> || ExecutionFunctionPointer<T>)
            return &FunctionSerializationHelper<T>::deserialize;
        else if constexpr (std::is_pointer_v<T>)
            return &PointerSerializationHelper<std::remove_pointer_t<T>>::deserialize;
        else
            return &ValueSerializationHelper<T>::deserialize;
    }();

    static constexpr auto deserializePop = []() {
        if constexpr (is_lambda_v<T>)
            return nullptr; // Lambdas should be handled by the task system
        else if constexpr (FunctionPointerType<T> || ExecutionFunctionPointer<T>)
            return &FunctionSerializationHelper<T>::deserializePop;
        else if constexpr (std::is_pointer_v<T>)
            return &PointerSerializationHelper<std::remove_pointer_t<T>>::deserializePop;
        else
            return &ValueSerializationHelper<T>::deserializePop;
    }();
};

// ########### Input Metadata ###########

template <typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    using Traits = SerializationTraits<T>;

    static constexpr DataCategory dataCategory = Traits::category;
    static constexpr ExecutionCategory executionCategory = Traits::executionCategory;
    static constexpr std::type_info const* typeInfo = Traits::typeInfo;

    static constexpr auto serialize = Traits::serialize;
    static constexpr auto deserialize = Traits::deserialize;
    static constexpr auto deserializePop = Traits::deserializePop;
};

} // namespace SP