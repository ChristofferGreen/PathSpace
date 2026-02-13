#pragma once

#include "PathSpaceBase.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SP {

// SnapshotCachedPathSpace - optional read-optimized snapshot cache layered on top of a backing space.
// - Reads may be served from a serialized snapshot when the path is not dirty.
// - Mutations that go through this layer mark paths dirty so reads fall back to the backing space.
// - A background worker can rebuild the snapshot after a debounce interval.
class SnapshotCachedPathSpace final : public PathSpaceBase {
public:
    struct SnapshotOptions {
        bool enabled = false;
        std::chrono::milliseconds rebuildDebounce{200};
        std::size_t maxDirtyRoots = 128;
        bool allowSynchronousRebuild = false;
    };

    struct SnapshotMetrics {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t rebuilds = 0;
        std::size_t rebuildFailures = 0;
        std::chrono::milliseconds lastRebuildMs{0};
        std::size_t bytes = 0;
    };

    explicit SnapshotCachedPathSpace(std::shared_ptr<PathSpaceBase> backing);
    ~SnapshotCachedPathSpace() override;

    auto setSnapshotOptions(SnapshotOptions options) -> void;
    [[nodiscard]] auto snapshotEnabled() const noexcept -> bool;
    [[nodiscard]] auto snapshotMetrics() const -> SnapshotMetrics;
    auto rebuildSnapshotNow() -> void;

    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;
    auto visit(PathVisitor const& visitor, VisitOptions const& options = {}) -> Expected<void> override;
    auto spanPackConst(std::span<const std::string> paths,
                       InputMetadata const& metadata,
                       Out const& options,
                       SpanPackConstCallback const& fn) const -> Expected<void> override;
    auto spanPackMut(std::span<const std::string> paths,
                     InputMetadata const& metadata,
                     Out const& options,
                     SpanPackMutCallback const& fn) const -> Expected<void> override;
    auto packInsert(std::span<const std::string> paths,
                    InputMetadata const& metadata,
                    std::span<void const* const> values) -> InsertReturn override;
    auto packInsertSpans(std::span<const std::string> paths,
                         std::span<SpanInsertSpec const> specs) -> InsertReturn override;

protected:
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;
    auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> override;
    std::optional<FutureAny> typedPeekFuture(std::string_view pathIn) const override;

private:
    struct SnapshotState;
    void markSnapshotDirty(std::string_view pathView);
    void markSnapshotDirty(Iterator const& path);
    auto trySnapshotRead(Iterator const& path,
                         InputMetadata const& inputMetadata,
                         Out const& options,
                         void* obj) -> bool;
    auto rebuildSnapshot(std::shared_ptr<SnapshotState> const& state) -> void;
    auto startSnapshotWorker() -> void;
    auto stopSnapshotWorker() -> void;

    std::shared_ptr<PathSpaceBase> backing;
    std::shared_ptr<SnapshotState> snapshotState;
};

} // namespace SP
