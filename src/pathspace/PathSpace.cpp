#include "PathSpace.hpp"
#include "task/TaskPool.hpp"
#include "core/PathSpaceContext.hpp"
#include "log/TaggedLogger.hpp"

namespace SP {

PathSpace::PathSpace(TaskPool* pool) {
    sp_log("PathSpace::PathSpace", "Function Called");
    // Ensure a non-null executor without relying on a global singleton.
    // If no pool is provided, create and own a TaskPool and use it as the executor.
    if (!pool) {
        this->ownedPool = std::make_unique<TaskPool>();
        this->pool = this->ownedPool.get();
    } else {
        this->pool = pool;
    }
    Executor* exec = static_cast<Executor*>(this->pool);
    this->context_ = std::make_shared<PathSpaceContext>(exec);
    this->setExecutor(exec);
};

PathSpace::PathSpace(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::PathSpace(context)", "Function Called");
    this->context_ = std::move(context);

        this->prefix = std::move(prefix);
    // Ensure we have an executor via context
    if (this->context_ && this->context_->executor()) {
        this->setExecutor(this->context_->executor());
    }
}
void PathSpace::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    sp_log("PathSpace::adoptContextAndPrefix", "Function Called");
    this->context_ = std::move(context);
    this->prefix = std::move(prefix);
    if (this->context_ && this->context_->executor()) {
        this->setExecutor(this->context_->executor());
    }
}
void PathSpace::setOwnedPool(TaskPool* p) {
    // Optional helper: if transferring ownership explicitly, adopt and manage the pool lifetime.
    if (p) {
        this->ownedPool.reset(p);
        this->pool = this->ownedPool.get();
        this->setExecutor(static_cast<Executor*>(this->pool));
    }
}

PathSpace::~PathSpace() {
    sp_log("PathSpace::~PathSpace", "Function Called");
    this->shutdown();
    // If we own a TaskPool instance, ensure worker threads are stopped before destruction.
    if (this->ownedPool) {
        this->ownedPool->shutdown();
        this->ownedPool.reset();
        this->pool = nullptr;
    }
}

auto PathSpace::clear() -> void {
    sp_log("PathSpace::clear", "Function Called");
    this->leaf.clear();
    this->context_->clearWaits();
}

auto PathSpace::shutdown() -> void {
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    this->context_->invalidateSink();
    this->context_->notifyAll();
    sp_log("PathSpace::shutdown Notified all waiters", "PathSpaceShutdown");
    this->leaf.clear();
    sp_log("PathSpace::shutdown Cleared paths", "PathSpaceShutdown");
}

auto PathSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    PathSpace* space = nullptr;
    if (data.metadata.dataCategory == DataCategory::UniquePtr) {
        if (data.metadata.typeInfo == &typeid(std::unique_ptr<PathSpace>)) {
            space           = reinterpret_cast<std::unique_ptr<PathSpace>*>(data.obj)->get();
        }
    }

    this->leaf.in(path, data, ret);

    if (space && ret.nbrSpacesInserted > 0 && data.metadata.dataCategory == DataCategory::UniquePtr) {
        std::string mountPrefix = this->prefix.empty() ? path.toString() : this->prefix + path.toString();
        space->adoptContextAndPrefix(this->context_, std::move(mountPrefix));
    }

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0) {
        if (!this->prefix.empty()) {
            auto notePath = this->prefix + std::string(path.toStringView());
            sp_log("PathSpace::in notify: " + notePath, "PathSpace");
            this->context_->notify(notePath);
        } else {
            auto notePath = std::string(path.toStringView());
            sp_log("PathSpace::in notify: " + notePath, "PathSpace");
            this->context_->notify(notePath);
        }
    }
    return ret;
}

auto PathSpace::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    sp_log("PathSpace::outBlock", "Function Called");
    if (options.isMinimal)
        return this->leaf.out(path, inputMetadata, obj, options.doPop);

    std::optional<Error> error;
    // First try entirely outside the loop to minimize lock time
    {
        error = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!error.has_value()) {
            if (options.doPop) {
                // Popping may unblock other waiters on the same path (more items may be available)
                if (!this->prefix.empty())
                    this->context_->notify(this->prefix + std::string(path.toStringView()));
                else
                    this->context_->notify(path.toStringView());
            }
            return error;
        }
        if (options.doBlock == false)
            return error;
    }

    auto const deadline = std::chrono::system_clock::now() + options.timeout;
    std::string waitPath = this->prefix.empty() ? path.toString() : this->prefix + path.toString();
    sp_log("PathSpace::out waiting on: " + waitPath, "PathSpace");

    // Second immediate re-check to close race between initial read and wait registration
    {
        auto secondTry = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!secondTry.has_value()) {
            // Notify other waiters to re-check after a successful read (pop or non-pop)
            if (!this->prefix.empty()) {
                auto notePath = this->prefix + std::string(path.toStringView());
                sp_log("out(success pre-wait) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            } else {
                auto notePath = std::string(path.toStringView());
                sp_log("out(success pre-wait) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            }
            return std::nullopt;
        }
    }

    while (true) {
        // Check deadline first
        auto now = std::chrono::system_clock::now();
        if (now >= deadline)
            return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};

        // Quick re-check before waiting to avoid sleeping unnecessarily
        {
            auto quick = this->leaf.out(path, inputMetadata, obj, options.doPop);
            if (!quick.has_value()) {
                if (!this->prefix.empty()) {
                    auto notePath = this->prefix + std::string(path.toStringView());
                    sp_log("out(success pre-wait in-loop) notify: " + notePath, "PathSpace");
                    this->context_->notify(notePath);
                } else {
                    auto notePath = std::string(path.toStringView());
                    sp_log("out(success pre-wait in-loop) notify: " + notePath, "PathSpace");
                    this->context_->notify(notePath);
                }
                return std::nullopt;
            }
        }

        // Wait in short slices; never call leaf.out while holding the WatchRegistry lock
        {
            auto guard  = this->context_->wait(waitPath);
            auto now2   = std::chrono::system_clock::now();
            auto remain = deadline - now2;
            auto slice  = std::chrono::milliseconds(20);
            if (remain < slice)
                slice = std::chrono::duration_cast<std::chrono::milliseconds>(remain);
            guard.wait_until(std::chrono::system_clock::now() + slice);
        }

        // After being notified (or slice elapsed), try to read again outside of the registry lock
        auto retry = this->leaf.out(path, inputMetadata, obj, options.doPop);
        if (!retry.has_value()) {
            // Notify other waiters to re-check after a successful read (pop or non-pop)
            if (!this->prefix.empty()) {
                auto notePath = this->prefix + std::string(path.toStringView());
                sp_log("out(success in-loop) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            } else {
                auto notePath = std::string(path.toStringView());
                sp_log("out(success in-loop) notify: " + notePath, "PathSpace");
                this->context_->notify(notePath);
            }
            return std::nullopt;
        } else {
            // Log why retry failed to help diagnose missed-notify or readiness races
            sp_log(std::string("out(retry) still failing, error=")
                       + retry->message.value_or("no-message")
                       + " code=" + std::to_string(static_cast<int>(retry->code)),
                   "PathSpace");
        }
    }
}

auto PathSpace::notify(std::string const& notificationPath) -> void {
    sp_log("PathSpace::notify", "Function Called");
    if (!this->prefix.empty()) {
        auto notePath = this->prefix + notificationPath;
        sp_log("PathSpace::notify forwarding: " + notePath, "PathSpace");
        this->context_->notify(notePath);
    } else {
        sp_log("PathSpace::notify forwarding: " + notificationPath, "PathSpace");
        this->context_->notify(notificationPath);
    }
}

} // namespace SP