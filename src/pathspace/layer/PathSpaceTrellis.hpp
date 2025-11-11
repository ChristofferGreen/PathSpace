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
    };

    struct EnableParseResult {
        std::string                             outputPath;
        TrellisMode                             mode;
        TrellisPolicy                           policy;
        std::vector<std::string>                sources;
    };

    auto handleEnable(InputData const& data) -> InsertReturn;
    auto handleDisable(InputData const& data) -> InsertReturn;

    auto parseEnableCommand(InputData const& data) const -> Expected<EnableParseResult>;
    auto parseDisableCommand(InputData const& data) const -> Expected<std::string>;

    static auto canonicalizeAbsolute(std::string const& raw) -> Expected<std::string>;
    static auto canonicalizeSourceList(std::vector<std::string> const& rawSources)
        -> Expected<std::vector<std::string>>;

    auto tryServeQueue(TrellisState& state,
                       InputMetadata const& inputMetadata,
                       Out const& options,
                       void* obj) -> std::optional<Error>;
    auto serveLatest(TrellisState& state,
                     InputMetadata const& inputMetadata,
                     Out const& options,
                     void* obj) -> std::optional<Error>;
    auto waitAndServeQueue(TrellisState& state,
                           InputMetadata const& inputMetadata,
                           Out const& options,
                           void* obj,
                           std::chrono::system_clock::time_point deadline) -> std::optional<Error>;
    auto waitAndServeLatest(TrellisState& state,
                            InputMetadata const& inputMetadata,
                            Out const& options,
                            void* obj,
                            std::chrono::system_clock::time_point deadline) -> std::optional<Error>;

    std::shared_ptr<PathSpaceBase> backing_;
    mutable std::mutex             statesMutex_;
    std::unordered_map<std::string, std::shared_ptr<TrellisState>> trellis_;
    std::string                                               mountPrefix_;
};

} // namespace SP
