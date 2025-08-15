#pragma once
#include "PathSpaceBase.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Leaf.hpp"
#include "core/Out.hpp"

#include "log/TaggedLogger.hpp"
#include "path/Iterator.hpp"
#include "path/utils.hpp"
#include "path/validation.hpp"
#include "task/Future.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include <memory>
#ifdef TYPED_TASKS
#include "task/IFutureAny.hpp"
#endif

namespace SP {

struct TaskPool;
#ifdef PATHSPACE_CONTEXT
class PathSpaceContext;
#endif
class PathSpace : public PathSpaceBase {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);
#ifdef PATHSPACE_CONTEXT
    /**
     * @brief Constructs a PathSpace with a shared context and an optional path prefix.
     * @param context Shared runtime context (executor, wait/notify, sink).
     * @param prefix  Optional mount path prefix for this space.
     */
    explicit PathSpace(std::shared_ptr<PathSpaceContext> context, std::string prefix = {});
    /**
     * @brief Adopt a shared context and mount prefix after construction (used for nested insert).
     */
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix);
    // Mark that this PathSpace owns the TaskPool and should shut it down and delete it on destruction.
    void setOwnedPool(TaskPool* p);
#endif
    ~PathSpace();

    virtual auto clear() -> void;

public:
    // Peek a Future aligned with the execution at a concrete path (if present).
    // Returns std::nullopt if the path is not concrete, not found, or not an execution node.
    auto peekFuture(std::string_view pathIn) const -> std::optional<Future> {
        Iterator const it{pathIn};
        return this->leaf.peekFuture(it);
    }

protected:
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error>;
    virtual auto shutdown() -> void;
    virtual auto notify(std::string const& notificationPath) -> void;
#ifdef TYPED_TASKS
    // Expose typed future peek to PathSpaceBase::readFuture
    std::optional<FutureAny> typedPeekFuture(std::string_view pathIn) const override {
        Iterator const it{pathIn};
        return this->leaf.peekAnyFuture(it);
    }
#endif


#ifdef PATHSPACE_CONTEXT
    // Optional mount prefix used when a shared context is present
    std::string prefix;
#endif
    TaskPool*   pool = nullptr;
    // If non-null, this PathSpace owns the TaskPool and will shut it down and destroy it at teardown.
    std::unique_ptr<TaskPool> ownedPool;
    Leaf        leaf;
};

} // namespace SP