#pragma once
#include "PathSpaceBase.hpp"

#include "core/Error.hpp"
#include "core/InsertReturn.hpp"
#include "core/Leaf.hpp"
#include "core/Out.hpp"


#include "path/Iterator.hpp"


#include "task/Future.hpp"
#include "task/Task.hpp"
#include "type/InputData.hpp"
#include "type/InputMetadata.hpp"
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include "task/IFutureAny.hpp"

namespace SP {

struct TaskPool;
class PathSpaceContext;
class PathSpace : public PathSpaceBase {
public:
    /**
     * @brief Constructs a PathSpace object.
     * @param pool Pointer to a TaskPool for managing asynchronous operations. If nullptr, uses the global instance.
     */
    explicit PathSpace(TaskPool* pool = nullptr);
    /**
     * @brief Constructs a PathSpace with a shared context and an optional path prefix.
     * @param context Shared runtime context (executor, wait/notify, sink).
     * @param prefix  Optional mount path prefix for this space.
     */
    explicit PathSpace(std::shared_ptr<PathSpaceContext> context, std::string prefix = {});

    ~PathSpace();

    virtual auto clear() -> void;

protected:
    /**
     * Adopt a shared context and mount prefix after construction.
     * Protected: used internally when mounting nested spaces; not part of the public API.
     * External callers should not invoke this directly—mounting is coordinated by insert/in().
     */
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;
    // Protected helper: transfer ownership of a TaskPool so PathSpace manages its lifetime.
    void setOwnedPool(TaskPool* p);

    // Protected test utilities:
    // - notifyAll(): wakes all waiters across paths via the shared context
    // - shutdownPublic(): cooperatively signals shutdown() for tests; expose via a test-only subclass if needed
    void notifyAll() { this->context_->notifyAll(); }
    void shutdownPublic() { this->shutdown(); }

    // Protected probe: non-blocking peek of a typed Future at a concrete path (if present).
    // Prefer PathSpaceBase::readFuture() for the public API. Returns std::nullopt if not found or not an execution node.
    auto peekFuture(std::string_view pathIn) const -> std::optional<Future> {
        Iterator const it{pathIn};
        return this->leaf.peekFuture(it);
    }

protected:
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    virtual auto shutdown() -> void override;
    virtual auto notify(std::string const& notificationPath) -> void override;
    auto getRootNode() -> Node* override;
    auto listChildrenCanonical(std::string_view canonicalPath) const -> std::vector<std::string> override;
    // Expose typed future peek to PathSpaceBase::readFuture
    std::optional<FutureAny> typedPeekFuture(std::string_view pathIn) const override {
        Iterator const it{pathIn};
        return this->leaf.peekAnyFuture(it);
    }


    // Optional mount prefix used when a shared context is present
    std::string prefix;
    TaskPool*   pool = nullptr;
    // If non-null, this PathSpace owns the TaskPool and will shut it down and destroy it at teardown.
    std::unique_ptr<TaskPool> ownedPool;
    Leaf        leaf;

};

} // namespace SP
