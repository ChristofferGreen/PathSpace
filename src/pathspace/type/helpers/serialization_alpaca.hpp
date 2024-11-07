#pragma once
#include "type/DataCategory.hpp"
#include "type/ExecutionCategory.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/serialization.hpp"
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

// ########### Type Detection Concepts ###########

template <typename T>
concept FunctionPointer = requires {
    requires std::is_pointer_v<T>;
    requires std::is_function_v<std::remove_pointer_t<T>>;
};

template <typename T, typename R = void>
concept ExecutionStdFunction = requires(T f) { requires std::is_convertible_v<T, std::function<R()>>; };

template <typename T>
concept ExecutionFunctionPointer = requires(T t) {
    requires(std::is_function_v<std::remove_pointer_t<T>> || std::is_member_function_pointer_v<T> || requires(T& x) { +x; });
    t();
};

template <typename T>
concept Execution = ExecutionFunctionPointer<T> || ExecutionStdFunction<T>;

template <typename T>
concept FundamentalType = std::is_fundamental_v<T>;

template <typename T>
concept AlpacaCompatible = !std::is_pointer_v<T> && requires(T t, SlidingBuffer& buffer) { SP::serialize<T>(t, buffer); };

// ########### Serialization Helpers ###########

template <typename T>
struct ValueSerializationHelper {
    static auto Serialize(void const* objPtr, SP::SlidingBuffer& bytes) -> void {
        if constexpr (FundamentalType<T>) {
            bytes.append(reinterpret_cast<const uint8_t*>(objPtr), sizeof(T));
        } else if constexpr (AlpacaCompatible<T>) {
            SP::serialize<T>(*static_cast<T const*>(objPtr), bytes);
        }
    }

    static auto Deserialize(void* objPtr, SP::SlidingBuffer const& bytes) -> void {
        if constexpr (FundamentalType<T>) {
            if (bytes.size() < sizeof(T)) {
                throw std::runtime_error("Buffer too small");
            }
            std::memcpy(objPtr, bytes.data(), sizeof(T));
        } else if constexpr (AlpacaCompatible<T>) {
            auto expected = SP::deserialize<T>(bytes);
            if (expected.has_value())
                *static_cast<T*>(objPtr) = std::move(expected.value());
            else
                sp_log("Deserialization failed: " + expected.error().message.value_or(""), "ERROR");
        }
    }

    static auto DeserializePop(void* objPtr, SP::SlidingBuffer& bytes) -> void {
        if constexpr (FundamentalType<T>) {
            ValueSerializationHelper<T>::Deserialize(objPtr, bytes);
            bytes.advance(sizeof(T));
        } else if constexpr (AlpacaCompatible<T>) {
            auto expected = SP::deserialize_pop<T>(bytes);
            if (expected.has_value())
                *static_cast<T*>(objPtr) = std::move(expected.value());
            else
                sp_log("Deserialization failed: " + expected.error().message.value_or(""), "ERROR");
        }
    }
};

// ########### Serialization Traits ###########

template <typename T>
struct AlpacaSerializationTraits {
    AlpacaSerializationTraits() = default;

    static constexpr std::type_info const* typeInfo = []() {
        if constexpr (Execution<T>) {
            return &typeid(std::invoke_result_t<T>);
        }
        return &typeid(T);
    }();

    static constexpr DataCategory const category = []() {
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
        if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &ValueSerializationHelper<T>::Serialize;
        } else if constexpr (AlpacaCompatible<T>) {
            return &ValueSerializationHelper<T>::Serialize;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserialize = []() -> void (*)(void* obj, SP::SlidingBuffer const&) {
        if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &ValueSerializationHelper<T>::Deserialize;
        } else if constexpr (AlpacaCompatible<T>) {
            return &ValueSerializationHelper<T>::Deserialize;
        } else {
            return nullptr;
        }
    }();

    static constexpr auto deserializePop = []() {
        if constexpr (ExecutionStdFunction<T>) {
            return nullptr;
        } else if constexpr (FunctionPointer<T>) {
            return nullptr;
        } else if constexpr (std::is_fundamental<T>::value) {
            return &ValueSerializationHelper<T>::DeserializePop;
        } else if constexpr (AlpacaCompatible<T>) {
            return &ValueSerializationHelper<T>::DeserializePop;
        } else {
            return nullptr;
        }
    }();
};

template <typename CVRefT>
struct InputMetadataT {
    using T = std::remove_cvref_t<CVRefT>;
    using Traits = AlpacaSerializationTraits<T>;

    static constexpr DataCategory dataCategory = Traits::category;
    static constexpr ExecutionCategory executionCategory = Traits::executionCategory;
    static constexpr std::type_info const* typeInfo = Traits::typeInfo;

    static constexpr auto serialize = Traits::serialize;
    static constexpr auto deserialize = Traits::deserialize;
    static constexpr auto deserializePop = Traits::deserializePop;
};

} // namespace SP
