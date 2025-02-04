#pragma once
#include "log/TaggedLogger.hpp"
#include "type/DataCategory.hpp"
#include "type/FunctionCategory.hpp"
#include "type/SlidingBuffer.hpp"
#include "type/serialization.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#define SERIALIZATION_TYPE uint8_t
#include <alpaca/alpaca.h>

namespace SP {
struct PathSpace;

// ########### Type Detection Concepts ###########

template <typename T>
struct is_unique_ptr : std::false_type {};

template <typename T, typename D>
struct is_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};

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
concept StringLiteral = std::is_array_v<std::remove_reference_t<T>> && std::is_same_v<std::remove_all_extents_t<std::remove_reference_t<T>>, char>;

template <typename T>
concept StringType = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

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

template <typename T>
struct StringSerializationHelper {
    static auto Serialize(void const* objPtr, SP::SlidingBuffer& bytes) -> void {
        std::string_view str;
        if constexpr (StringLiteral<T>) {
            str = static_cast<const char*>(objPtr);
        } else if constexpr (StringType<T>) {
            str = *static_cast<T const*>(objPtr);
        }
        uint32_t size = str.size();
        bytes.append(reinterpret_cast<const uint8_t*>(&size), sizeof(size));
        bytes.append(reinterpret_cast<const uint8_t*>(str.data()), size);
    }

    static auto Deserialize(void* objPtr, SP::SlidingBuffer const& bytes) -> void {
        if (bytes.size() < sizeof(uint32_t)) {
            throw std::runtime_error("Buffer too small for string size");
        }

        uint32_t size;
        std::memcpy(&size, bytes.data(), sizeof(size));

        if (bytes.size() < sizeof(size) + size) {
            throw std::runtime_error("Buffer too small for string data");
        }

        if constexpr (std::is_same_v<T, std::string>) {
            auto& str = *static_cast<std::string*>(objPtr);
            str.assign(reinterpret_cast<const char*>(bytes.data() + sizeof(size)), size);
        } else {
            throw std::runtime_error("Can only deserialize to std::string");
        }
    }

    static auto DeserializePop(void* objPtr, SP::SlidingBuffer& bytes) -> void {
        StringSerializationHelper<T>::Deserialize(objPtr, bytes);
        bytes.advance(sizeof(uint32_t) + *reinterpret_cast<const uint32_t*>(bytes.data()));
    }
};

// ########### Serialization Traits ###########

template <typename T>
struct AlpacaSerializationTraits {
    AlpacaSerializationTraits() = default;

    static constexpr std::type_info const* typeInfo = []() {
        if constexpr (Execution<T>)
            return &typeid(std::invoke_result_t<T>);
        else if constexpr (StringLiteral<T> || StringType<T>)
            return &typeid(std::string); // Map all string types to std::string
        else
            return &typeid(T);
    }();

    static constexpr DataCategory const category = []() {
        if constexpr (Execution<T>)
            return DataCategory::Execution;
        else if constexpr (FunctionPointer<T>)
            return DataCategory::FunctionPointer;
        else if constexpr (FundamentalType<T>)
            return DataCategory::Fundamental;
        else if constexpr (StringLiteral<T> || StringType<T>)
            return DataCategory::SerializedData;
        else if constexpr (AlpacaCompatible<T>)
            return DataCategory::SerializationLibraryCompatible;
        else if (is_unique_ptr<T>::value)
            return DataCategory::UniquePtr;
        else
            return DataCategory::None;
    }();

    static constexpr FunctionCategory const functionCategory = []() {
        if constexpr (ExecutionFunctionPointer<T>)
            return FunctionCategory::FunctionPointer;
        else if constexpr (ExecutionStdFunction<T>)
            return FunctionCategory::StdFunction;
        else
            return FunctionCategory::None;
    }();

    static constexpr auto serialize = []() {
        if constexpr (Execution<T> || is_unique_ptr<T>::value)
            return nullptr;
        else if constexpr (StringLiteral<T> || StringType<T>)
            return &StringSerializationHelper<T>::Serialize;
        else if constexpr (FundamentalType<T> || AlpacaCompatible<T>)
            return &ValueSerializationHelper<T>::Serialize;
        else
            return nullptr;
    }();

    static constexpr auto deserialize = []() -> void (*)(void* obj, SP::SlidingBuffer const&) {
        if constexpr (Execution<T> || is_unique_ptr<T>::value)
            return nullptr;
        else if constexpr (std::is_same_v<T, std::string>) // Only allow deserialization to std::string
            return &StringSerializationHelper<T>::Deserialize;
        else if constexpr (FundamentalType<T> || AlpacaCompatible<T>)
            return &ValueSerializationHelper<T>::Deserialize;
        else
            return nullptr;
    }();

    static constexpr auto deserializePop = []() {
        if constexpr (Execution<T> || is_unique_ptr<T>::value)
            return nullptr;
        else if constexpr (std::is_same_v<T, std::string>) // Only allow deserialization to std::string
            return &StringSerializationHelper<T>::DeserializePop;
        else if constexpr (FundamentalType<T> || AlpacaCompatible<T>)
            return &ValueSerializationHelper<T>::DeserializePop;
        else
            return nullptr;
    }();
};

template <typename CVRefT>
struct InputMetadataT {
    using T      = std::remove_cvref_t<CVRefT>;
    using Traits = AlpacaSerializationTraits<T>;

    static constexpr DataCategory          dataCategory     = Traits::category;
    static constexpr FunctionCategory      functionCategory = Traits::functionCategory;
    static constexpr std::type_info const* typeInfo         = Traits::typeInfo;

    static constexpr auto serialize      = Traits::serialize;
    static constexpr auto deserialize    = Traits::deserialize;
    static constexpr auto deserializePop = Traits::deserializePop;
};

} // namespace SP