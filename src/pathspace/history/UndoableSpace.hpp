#pragma once

#include "PathSpaceBase.hpp"
#include "core/Error.hpp"
#include "history/CowSubtreePrototype.hpp"
#include "path/ConcretePath.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SP {
class PathSpace;
struct Node;
} // namespace SP

namespace SP::History {

struct HistoryOptions {
    std::size_t maxEntries          = 128;
    std::size_t maxBytesRetained    = 0;
    bool        manualGarbageCollect = false;
    bool        allowNestedUndo      = false;
};

struct HistoryStats {
    std::size_t undoCount            = 0;
    std::size_t redoCount            = 0;
    std::size_t bytesRetained        = 0;
    bool        manualGarbageCollect = false;
};

struct TrimStats {
    std::size_t entriesRemoved = 0;
    std::size_t bytesRemoved   = 0;
};

using TrimPredicate = std::function<bool(std::size_t generationIndex)>;

class UndoableSpace : public PathSpaceBase {
public:
    explicit UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults = {});
    ~UndoableSpace() override = default;

    auto enableHistory(SP::ConcretePathStringView root, HistoryOptions opts = {}) -> Expected<void>;
    auto disableHistory(SP::ConcretePathStringView root) -> Expected<void>;
    auto undo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto redo(SP::ConcretePathStringView root, std::size_t steps = 1) -> Expected<void>;
    auto trimHistory(SP::ConcretePathStringView root, TrimPredicate predicate) -> Expected<TrimStats>;
    auto getHistoryStats(SP::ConcretePathStringView root) const -> Expected<HistoryStats>;

    struct RootState;

    class HistoryTransaction {
    public:
        HistoryTransaction(HistoryTransaction&&) noexcept;
        HistoryTransaction& operator=(HistoryTransaction&&) noexcept;
        ~HistoryTransaction();

        HistoryTransaction(HistoryTransaction const&)            = delete;
        HistoryTransaction& operator=(HistoryTransaction const&) = delete;

        auto commit() -> Expected<void>;

    private:
        friend class UndoableSpace;
        HistoryTransaction(UndoableSpace& owner,
                           std::shared_ptr<RootState> rootState);

        UndoableSpace*                    owner_;
        std::shared_ptr<RootState>        rootState_;
        bool                              active_ = true;
    };

    auto beginTransaction(SP::ConcretePathStringView root) -> Expected<HistoryTransaction>;

protected:
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;

private:
    struct MatchedRoot {
        std::shared_ptr<RootState> state;
        std::string                key;
        std::string                relativePath;
    };

    class TransactionGuard {
    public:
        TransactionGuard() = default;
        TransactionGuard(UndoableSpace& owner, std::shared_ptr<RootState> state, bool active);
        TransactionGuard(TransactionGuard&& other) noexcept;
        TransactionGuard& operator=(TransactionGuard&& other) noexcept;
        ~TransactionGuard();

        TransactionGuard(TransactionGuard const&)            = delete;
        TransactionGuard& operator=(TransactionGuard const&) = delete;

        void markDirty();
        auto commit() -> Expected<void>;
        void deactivate();
        explicit operator bool() const { return active_; }

    private:
        void release();

        UndoableSpace*              owner_  = nullptr;
        std::shared_ptr<RootState>  state_;
        bool                        active_ = false;
    };

    auto findRoot(SP::ConcretePathStringView root) const -> std::shared_ptr<RootState>;
    auto findRootByPath(std::string const& path) const -> std::optional<MatchedRoot>;
    auto beginTransactionInternal(std::shared_ptr<RootState> const& state) -> Expected<TransactionGuard>;
    auto commitTransaction(RootState& state) -> Expected<void>;
    auto markTransactionDirty(RootState& state) -> void;

    auto captureSnapshotLocked(RootState& state) -> Expected<CowSubtreePrototype::Snapshot>;
    auto applySnapshotLocked(RootState& state, CowSubtreePrototype::Snapshot const& snapshot) -> Expected<void>;
    auto clearSubtree(Node& node) -> void;

    auto handleControlInsert(MatchedRoot const& matchedRoot, std::string const& command, InputData const& data) -> InsertReturn;
    auto interpretSteps(InputData const& data) const -> std::size_t;

    auto resolveRootNode() -> Node*;

    std::unique_ptr<PathSpaceBase>                     inner_;
    HistoryOptions                                     defaultOptions_;
    mutable std::mutex                                 rootsMutex_;
    std::unordered_map<std::string, std::shared_ptr<RootState>> roots_;
};

} // namespace SP::History
