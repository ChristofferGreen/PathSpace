#pragma once

#include "core/Error.hpp"
#include "core/NodeData.hpp"
#include "distributed/RemoteMountProtocol.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include "type/TypeMetadataRegistry.hpp"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace SP::Distributed {

namespace detail {

inline auto Base64Encode(std::span<const std::byte> bytes) -> std::string {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

    std::size_t index = 0;
    while (index + 2U < bytes.size()) {
        auto b0 = static_cast<unsigned char>(bytes[index]);
        auto b1 = static_cast<unsigned char>(bytes[index + 1U]);
        auto b2 = static_cast<unsigned char>(bytes[index + 2U]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
        encoded.push_back(kAlphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)]);
        encoded.push_back(kAlphabet[b2 & 0x3FU]);
        index += 3U;
    }

    if (index < bytes.size()) {
        auto b0 = static_cast<unsigned char>(bytes[index]);
        encoded.push_back(kAlphabet[b0 >> 2U]);
        if (index + 1U < bytes.size()) {
            auto b1 = static_cast<unsigned char>(bytes[index + 1U]);
            encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
            encoded.push_back(kAlphabet[(b1 & 0x0FU) << 2U]);
            encoded.push_back('=');
        } else {
            encoded.push_back(kAlphabet[(b0 & 0x03U) << 4U]);
            encoded.push_back('=');
            encoded.push_back('=');
        }
    }
    return encoded;
}

} // namespace detail

template <typename T>
[[nodiscard]] inline Expected<ValuePayload> EncodeExecutionValue(InputData const& data) {
    NodeData exec_node(data);
    if (auto future = exec_node.peekFuture(); future.has_value()) {
        future->wait();
    } else if (auto any_future = exec_node.peekAnyFuture(); any_future.has_value()) {
        any_future->wait();
    }

    T             result{};
    InputMetadata metadata{InputMetadataT<T>{}};
    if (auto error = exec_node.deserialize(&result, metadata); error.has_value()) {
        return std::unexpected(*error);
    }

    InputData value_input{result};
    NodeData  value_node(value_input);
    auto bytes = value_node.frontSerializedValueBytes();
    if (!bytes) {
        return std::unexpected(
            Error{Error::Code::InvalidType, "Unable to encode remote execution result"});
    }
    ValuePayload payload;
    payload.encoding  = std::string{kEncodingTypedSlidingBuffer};
    payload.type_name = metadata.typeInfo ? metadata.typeInfo->name() : "";
    payload.data      = detail::Base64Encode(*bytes);
    return payload;
}

class RemoteExecutionEncoderRegistry {
public:
    using EncoderFn = Expected<ValuePayload> (*)(InputData const&);

    static RemoteExecutionEncoderRegistry& instance() {
        static RemoteExecutionEncoderRegistry registry;
        return registry;
    }

    bool registerEncoder(std::type_index type, EncoderFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return encoders_.emplace(type, fn).second;
    }

    [[nodiscard]] std::optional<EncoderFn> find(std::type_index type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                         it = encoders_.find(type);
        if (it == encoders_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    mutable std::mutex                               mutex_;
    std::unordered_map<std::type_index, EncoderFn>   encoders_;
};

template <typename T>
inline bool RegisterRemoteExecutionEncoder() {
    auto& registry   = RemoteExecutionEncoderRegistry::instance();
    auto  registered = registry.registerEncoder(std::type_index(typeid(T)),
                                                &EncodeExecutionValue<T>);
    [[maybe_unused]] bool metadata_registered =
        TypeMetadataRegistry::instance().registerType<T>();
    return registered;
}

#define PATHSPACE_INTERNAL_REMOTE_ENCODER_CONCAT(a, b) a##b
#define PATHSPACE_REMOTE_ENCODER_CONCAT(a, b) PATHSPACE_INTERNAL_REMOTE_ENCODER_CONCAT(a, b)

#define PATHSPACE_REGISTER_REMOTE_EXECUTION_ENCODER(Type)                                         \
    namespace {                                                                                   \
    struct PATHSPACE_REMOTE_ENCODER_CONCAT(RemoteExecutionEncoderAutoRegister_, __LINE__) {        \
        PATHSPACE_REMOTE_ENCODER_CONCAT(RemoteExecutionEncoderAutoRegister_, __LINE__)() {         \
            ::SP::Distributed::RegisterRemoteExecutionEncoder<Type>();                            \
        }                                                                                         \
    } PATHSPACE_REMOTE_ENCODER_CONCAT(RemoteExecutionEncoderAutoRegisterInstance_, __LINE__);     \
    }

} // namespace SP::Distributed
