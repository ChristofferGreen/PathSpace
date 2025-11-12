#pragma once

#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "path/ConcretePath.hpp"
#include "path/Iterator.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

class PathSpaceTrellis final : public PathSpaceBase {
public:
    explicit PathSpaceTrellis(std::shared_ptr<PathSpaceBase> backing);

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj)
        -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;

private:
    enum class TrellisMode { Queue, Latest };
    enum class TrellisPolicy { RoundRobin, Priority };

    struct TrellisState {
        TrellisMode                 mode{TrellisMode::Queue};
        TrellisPolicy               policy{TrellisPolicy::RoundRobin};
        std::vector<std::string>    sources;
        std::size_t                 roundRobinCursor{0};
        bool                        shuttingDown{false};
        mutable std::mutex          mutex;
        std::size_t                 maxWaitersPerSource{0};
        std::unordered_map<std::string, std::size_t> activeWaiters;
        std::deque<std::string>     readySources;
        std::unordered_set<std::string> readySourceSet;
    };

    struct EnableParseResult {
        std::string                             outputPath;
        TrellisMode                             mode;
        TrellisPolicy                           policy;
        std::vector<std::string>                sources;
        std::size_t                             maxWaitersPerSource;
    };

    auto handleEnable(InputData const& data) -> InsertReturn;
    auto handleDisable(InputData const& data) -> InsertReturn;

    auto parseEnableCommand(InputData const& data) const -> Expected<EnableParseResult>;
    auto parseDisableCommand(InputData const& data) const -> Expected<std::string>;

    static auto canonicalizeAbsolute(std::string const& raw) -> Expected<std::string>;
    static auto canonicalizeSourceList(std::vector<std::string> const& rawSources)
        -> Expected<std::vector<std::string>>;

    auto tryServeQueue(TrellisState& state,
                       std::string const& canonicalOutputPath,
                       InputMetadata const& inputMetadata,
                       Out const& options,
                       void* obj,
                       std::optional<std::string>& servicedSource) -> std::optional<Error>;
    auto serveLatest(TrellisState& state,
                     std::string const& canonicalOutputPath,
                     InputMetadata const& inputMetadata,
                     Out const& options,
                     void* obj,
                     std::optional<std::string>& servicedSource) -> std::optional<Error>;
    auto waitAndServeQueue(TrellisState& state,
                           std::string const& canonicalOutputPath,
                           InputMetadata const& inputMetadata,
                           Out const& options,
                           void* obj,
                           std::chrono::system_clock::time_point deadline,
                           std::optional<std::string>& servicedSource) -> std::optional<Error>;
    auto waitAndServeLatest(TrellisState& state,
                            std::string const& canonicalOutputPath,
                            InputMetadata const& inputMetadata,
                            Out const& options,
                            void* obj,
                            std::chrono::system_clock::time_point deadline,
                            std::optional<std::string>& servicedSource) -> std::optional<Error>;
    auto persistStateLocked(std::string const& canonicalOutputPath, TrellisState const& state)
        -> std::optional<Error>;
    auto erasePersistedState(std::string const& canonicalOutputPath) -> std::optional<Error>;
    auto persistStats(std::string const& canonicalOutputPath, TrellisState const& state)
        -> std::optional<Error>;
    auto eraseStats(std::string const& canonicalOutputPath) -> std::optional<Error>;
    auto updateStats(std::string const& canonicalOutputPath,
                     std::function<void(TrellisStats&)> const& mutate) -> std::optional<Error>;
    void recordServeSuccess(std::string const& canonicalOutputPath,
                            std::string const& sourcePath,
                            bool waited);
    void recordServeError(std::string const& canonicalOutputPath, Error const& error);
    void updateBufferedReadyStats(std::string const& canonicalOutputPath,
                                  std::optional<std::size_t> readyCountOverride = std::nullopt);
    std::optional<std::string> pickReadySource(TrellisState& state);
    bool enqueueReadySource(TrellisState& state, std::string const& source);
    std::size_t bufferedReadyCount(std::string const& canonicalOutputPath);
    void restorePersistedStatesLocked();
    static auto modeToString(TrellisMode mode) -> std::string;
    static auto policyToString(TrellisPolicy policy) -> std::string;
    static auto modeFromString(std::string_view value) -> Expected<TrellisMode>;
    static auto policyFromString(std::string_view value) -> Expected<TrellisPolicy>;
    static auto stateConfigPathFor(std::string const& canonicalOutputPath) -> std::string;
    static auto stateStatsPathFor(std::string const& canonicalOutputPath) -> std::string;
    static auto stateBackpressurePathFor(std::string const& canonicalOutputPath) -> std::string;
    static auto stateBufferedReadyPathFor(std::string const& canonicalOutputPath) -> std::string;
    static auto legacyStateBackpressurePathFor(std::string const& canonicalOutputPath) -> std::string;
    static auto stateRootPathFor(std::string const& canonicalOutputPath) -> std::string;

    auto clearLegacyStateNode(std::string const& canonicalOutputPath,
                              bool removeActiveConfig = false) -> std::optional<Error>;

    std::shared_ptr<PathSpaceBase> backing_;
    mutable std::mutex             statesMutex_;
    std::unordered_map<std::string, std::shared_ptr<TrellisState>> trellis_;
    std::string                                               mountPrefix_;
    bool                                                      persistenceLoaded_{false};
};

} // namespace SP
