#pragma once

#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"
#include "path/Iterator.hpp"
#include "path/UnvalidatedPath.hpp"
#include "task/IFutureAny.hpp"

#include "parallel_hashmap/phmap.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SP {

class PathSpaceTrellis final : public PathSpaceBase {
public:
    explicit PathSpaceTrellis(std::shared_ptr<PathSpaceBase> backing);

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj)
        -> std::optional<Error> override;
    auto notify(std::string const& notificationPath) -> void override;
    auto shutdown() -> void override;
    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override;

    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;

    [[nodiscard]] auto debugSources() const -> std::vector<std::string>;

protected:
    auto getRootNode() -> Node* override;
    [[nodiscard]] auto typedPeekFuture(std::string_view pathIn) const -> std::optional<FutureAny> override;

private:
    using SourceSet = phmap::flat_hash_set<std::string>;

    [[nodiscard]] auto joinWithMount(std::string_view tail) const -> std::string;
    [[nodiscard]] auto stripMount(std::string const& absolute) const -> std::string;
    [[nodiscard]] auto snapshotSources() const -> std::vector<std::string>;
    [[nodiscard]] auto canonicalize(std::string const& path) const -> std::optional<std::string>;
    [[nodiscard]] auto isMoveOnly(InputData const& data) const -> bool;
    [[nodiscard]] auto extractStringPayload(InputData const& data) const -> std::optional<std::string>;

    auto handleRootInsert(InputData const& data) -> InsertReturn;
    auto handleSystemCommand(std::string_view command, InputData const& data) -> InsertReturn;
    auto handleEnable(std::string const& payload) -> InsertReturn;
    auto handleDisable(std::string const& payload) -> InsertReturn;

    [[nodiscard]] auto tryFanOutRead(InputMetadata const& metadata,
                                     Out const& options,
                                     void* obj,
                                     bool doPop) -> std::optional<Error>;
    [[nodiscard]] auto tryFanOutFuture() const -> std::optional<FutureAny>;

    static auto mergeInsertReturn(InsertReturn& target, InsertReturn const& source) -> void;
    static auto isSystemPath(Iterator const& path) -> bool;

    std::shared_ptr<PathSpaceBase> backing_;
    std::string                    mountPrefix_;
    mutable std::shared_mutex      registryMutex_;
    SourceSet                      sourceSet_;
    std::vector<std::string>       sourceOrder_;
    mutable std::atomic<size_t>    roundRobinCursor_{0};
    std::atomic<bool>              isShutdown_{false};
};

} // namespace SP
