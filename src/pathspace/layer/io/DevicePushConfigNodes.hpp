#pragma once

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace SP {

/**
 * DevicePushConfigNodes — shared helper that backs `/config/push/<node>` nodes for
 * PathIO providers. Providers can delegate insert/read handling to this helper
 * so every device exposes the same push enable, throttling, telemetry, and
 * subscriber bookkeeping schema.
 */
class DevicePushConfigNodes {
public:
    struct OutResult {
        bool handled = false;
        std::optional<Error> error;
    };

    [[nodiscard]] std::optional<InsertReturn>
    handleInsert(std::string const& tail, InputData const& data);

    [[nodiscard]] OutResult
    handleRead(std::string const& tail,
               InputMetadata const& metadata,
               void* obj) const;

    [[nodiscard]] bool isConfigPath(std::string const& tail) const;

private:
    static bool matchesSuffix(std::string const& tail, std::string_view suffix);
    static std::optional<std::string> subscriberName(std::string const& tail);
    static bool expectBool(InputData const& data, bool& out);
    static bool expectBool(InputMetadata const& metadata);
    static bool expectUint32(InputData const& data, std::uint32_t& out);
    static bool expectUint32(InputMetadata const& metadata);

private:
    std::atomic<bool> push_enabled_{false};
    std::atomic<bool> telemetry_enabled_{false};
    std::atomic<std::uint32_t> rate_limit_hz_{240};
    std::atomic<std::uint32_t> max_queue_{256};

    mutable std::mutex subscribers_mutex_;
    std::unordered_map<std::string, bool> subscribers_;
};

} // namespace SP
