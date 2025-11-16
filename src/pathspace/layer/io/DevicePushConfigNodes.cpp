#include "layer/io/DevicePushConfigNodes.hpp"

#include <typeinfo>

namespace SP {

namespace {

constexpr std::string_view kEnabledPath = "config/push/enabled";
constexpr std::string_view kTelemetryPath = "config/push/telemetry_enabled";
constexpr std::string_view kRateLimitPath = "config/push/rate_limit_hz";
constexpr std::string_view kMaxQueuePath = "config/push/max_queue";
constexpr std::string_view kSubscribersPrefix = "config/push/subscribers/";

auto makeTypeError(std::string_view path, std::string_view expected)
    -> Error {
    std::string message = "Expected ";
    message.append(expected);
    message.append(" at ");
    message.append(path);
    return Error{Error::Code::InvalidType, std::move(message)};
}

auto makeMalformed(std::string_view path) -> Error {
    std::string message = "Null payload for ";
    message.append(path);
    return Error{Error::Code::MalformedInput, std::move(message)};
}

auto makeUnsupported(std::string_view path) -> Error {
    std::string message = "Unsupported push config path: ";
    message.append(path);
    return Error{Error::Code::InvalidPath, std::move(message)};
}

} // namespace

std::optional<InsertReturn>
DevicePushConfigNodes::handleInsert(std::string const& tail, InputData const& data) {
    if (!isConfigPath(tail)) {
        return std::nullopt;
    }

    InsertReturn ret;

    bool boolValue = false;
    std::uint32_t uintValue = 0;

    if (matchesSuffix(tail, kEnabledPath)) {
        if (!expectBool(data, boolValue)) {
            ret.errors.emplace_back(makeTypeError(kEnabledPath, "bool"));
            return ret;
        }
        push_enabled_.store(boolValue, std::memory_order_release);
        ret.nbrValuesInserted = 1;
        return ret;
    }
    if (matchesSuffix(tail, kTelemetryPath)) {
        if (!expectBool(data, boolValue)) {
            ret.errors.emplace_back(makeTypeError(kTelemetryPath, "bool"));
            return ret;
        }
        telemetry_enabled_.store(boolValue, std::memory_order_release);
        ret.nbrValuesInserted = 1;
        return ret;
    }
    if (matchesSuffix(tail, kRateLimitPath)) {
        if (!expectUint32(data, uintValue)) {
            ret.errors.emplace_back(makeTypeError(kRateLimitPath, "uint32_t"));
            return ret;
        }
        rate_limit_hz_.store(uintValue, std::memory_order_release);
        ret.nbrValuesInserted = 1;
        return ret;
    }
    if (matchesSuffix(tail, kMaxQueuePath)) {
        if (!expectUint32(data, uintValue)) {
            ret.errors.emplace_back(makeTypeError(kMaxQueuePath, "uint32_t"));
            return ret;
        }
        max_queue_.store(uintValue, std::memory_order_release);
        ret.nbrValuesInserted = 1;
        return ret;
    }

    if (auto subscriber = subscriberName(tail)) {
        if (!expectBool(data, boolValue)) {
            ret.errors.emplace_back(makeTypeError("config/push/subscribers/<id>", "bool"));
            return ret;
        }
        {
            std::lock_guard<std::mutex> lg(subscribers_mutex_);
            subscribers_[*subscriber] = boolValue;
        }
        ret.nbrValuesInserted = 1;
        return ret;
    }

    ret.errors.emplace_back(makeUnsupported(tail));
    return ret;
}

DevicePushConfigNodes::OutResult
DevicePushConfigNodes::handleRead(std::string const& tail,
                                  InputMetadata const& metadata,
                                  void* obj) const {
    if (!isConfigPath(tail)) {
        return {};
    }
    if (obj == nullptr) {
        return {true, makeMalformed(tail)};
    }

    if (matchesSuffix(tail, kEnabledPath)) {
        if (!expectBool(metadata)) {
            return {true, makeTypeError(kEnabledPath, "bool")};
        }
        *reinterpret_cast<bool*>(obj) = push_enabled_.load(std::memory_order_acquire);
        return {true, std::nullopt};
    }
    if (matchesSuffix(tail, kTelemetryPath)) {
        if (!expectBool(metadata)) {
            return {true, makeTypeError(kTelemetryPath, "bool")};
        }
        *reinterpret_cast<bool*>(obj) = telemetry_enabled_.load(std::memory_order_acquire);
        return {true, std::nullopt};
    }
    if (matchesSuffix(tail, kRateLimitPath)) {
        if (!expectUint32(metadata)) {
            return {true, makeTypeError(kRateLimitPath, "uint32_t")};
        }
        *reinterpret_cast<std::uint32_t*>(obj) = rate_limit_hz_.load(std::memory_order_acquire);
        return {true, std::nullopt};
    }
    if (matchesSuffix(tail, kMaxQueuePath)) {
        if (!expectUint32(metadata)) {
            return {true, makeTypeError(kMaxQueuePath, "uint32_t")};
        }
        *reinterpret_cast<std::uint32_t*>(obj) = max_queue_.load(std::memory_order_acquire);
        return {true, std::nullopt};
    }

    if (auto subscriber = subscriberName(tail)) {
        if (!expectBool(metadata)) {
            return {true, makeTypeError("config/push/subscribers/<id>", "bool")};
        }
        bool value = false;
        {
            std::lock_guard<std::mutex> lg(subscribers_mutex_);
            auto it = subscribers_.find(*subscriber);
            if (it != subscribers_.end()) {
                value = it->second;
            }
        }
        *reinterpret_cast<bool*>(obj) = value;
        return {true, std::nullopt};
    }

    return {true, makeUnsupported(tail)};
}

bool DevicePushConfigNodes::isConfigPath(std::string const& tail) const {
    return matchesSuffix(tail, kEnabledPath) ||
           matchesSuffix(tail, kTelemetryPath) ||
           matchesSuffix(tail, kRateLimitPath) ||
           matchesSuffix(tail, kMaxQueuePath) ||
           subscriberName(tail).has_value();
}

bool DevicePushConfigNodes::matchesSuffix(std::string const& tail, std::string_view suffix) {
    if (tail == suffix) {
        return true;
    }
    if (tail.size() < suffix.size()) {
        return false;
    }
    auto tailPos = tail.size() - suffix.size();
    if (tail.compare(tailPos, suffix.size(), suffix) != 0) {
        return false;
    }
    if (tailPos == 0) {
        return true;
    }
    return tail[tailPos - 1] == '/';
}

std::optional<std::string>
DevicePushConfigNodes::subscriberName(std::string const& tail) {
    auto pos = tail.rfind(kSubscribersPrefix);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    if (pos != 0 && tail[pos - 1] != '/') {
        return std::nullopt;
    }
    auto start = pos + kSubscribersPrefix.size();
    if (start >= tail.size()) {
        return std::nullopt;
    }
    auto name = tail.substr(start);
    if (name.find('/') != std::string::npos) {
        return std::nullopt;
    }
    return name;
}

bool DevicePushConfigNodes::expectBool(InputData const& data, bool& out) {
    if (data.metadata.typeInfo != &typeid(bool) || data.obj == nullptr) {
        return false;
    }
    out = *reinterpret_cast<bool const*>(data.obj);
    return true;
}

bool DevicePushConfigNodes::expectBool(InputMetadata const& metadata) {
    return metadata.typeInfo == &typeid(bool);
}

bool DevicePushConfigNodes::expectUint32(InputData const& data, std::uint32_t& out) {
    if (data.obj == nullptr) {
        return false;
    }
    if (data.metadata.typeInfo == &typeid(std::uint32_t)) {
        out = *reinterpret_cast<std::uint32_t const*>(data.obj);
        return true;
    }
    if (data.metadata.typeInfo == &typeid(std::int32_t)) {
        auto value = *reinterpret_cast<std::int32_t const*>(data.obj);
        if (value < 0) {
            return false;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    }
    return false;
}

bool DevicePushConfigNodes::expectUint32(InputMetadata const& metadata) {
    return metadata.typeInfo == &typeid(std::uint32_t);
}

} // namespace SP

