#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SP {

struct EnableTrellisCommand {
    std::string              name;
    std::vector<std::string> sources;
    std::string              mode;
    std::string              policy;
};

struct DisableTrellisCommand {
    std::string name;
};

struct TrellisPersistedConfig {
    std::string              name;
    std::vector<std::string> sources;
    std::string              mode;
    std::string              policy;
};

struct TrellisStats {
    std::string              name;
    std::string              mode;
    std::string              policy;
    std::vector<std::string> sources;
    std::uint64_t            sourceCount{0};
    std::uint64_t            servedCount{0};
    std::uint64_t            waitCount{0};
    std::uint64_t            errorCount{0};
    std::uint64_t            backpressureCount{0};
    std::string              lastSource;
    std::int32_t             lastErrorCode{0};
    std::uint64_t            lastUpdateNs{0};
};

struct TrellisTraceEvent {
    std::uint64_t timestampNs{0};
    std::string   message;
};

struct TrellisTraceSnapshot {
    std::vector<TrellisTraceEvent> events;
};

struct TrellisRuntimeWaiterEntry {
    std::string   source;
    std::uint64_t count{0};
};

struct TrellisRuntimeWaiterSnapshot {
    std::vector<TrellisRuntimeWaiterEntry> entries;
};

struct TrellisRuntimeFlags {
    bool shuttingDown{false};
};

} // namespace SP
