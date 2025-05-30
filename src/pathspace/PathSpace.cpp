#include "PathSpace.hpp"
#include "task/TaskPool.hpp"

namespace SP {

PathSpace::PathSpace(TaskPool* pool) {
    sp_log("PathSpace::PathSpace", "Function Called");
    if (this->pool == nullptr)
        this->pool = &TaskPool::Instance();
};

PathSpace::~PathSpace() {
    sp_log("PathSpace::~PathSpace", "Function Called");
    this->shutdown();
}

auto PathSpace::clear() -> void {
    sp_log("PathSpace::clear", "Function Called");
    this->leaf.clear();
    this->waitMap.clear();
}

auto PathSpace::shutdown() -> void {
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    this->waitMap.notifyAll();
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
            space->rootPath = path.startToCurrent();
        }
    }

    this->leaf.in(path, data, ret);

    if (space && ret.nbrSpacesInserted > 0 && data.metadata.dataCategory == DataCategory::UniquePtr) {
        space->root = this->root ? this->root : this;
    }

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0)
        waitMap.notify(path.toStringView());
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
        if (!error.has_value() || (options.doBlock == false))
            return error;
    }

    auto const deadline = std::chrono::system_clock::now() + options.timeout;
    while (true) {
        // Check deadline first
        auto now = std::chrono::system_clock::now();
        if (now >= deadline)
            return Error{Error::Code::Timeout, "Operation timed out waiting for data at path: " + path.toString()};

        // Wait with minimal scope
        auto guard = waitMap.wait(path.toStringView());
        {
            bool success = guard.wait_until(deadline, [&]() {
                error           = this->leaf.out(path, inputMetadata, obj, options.doPop);
                bool haveResult = !error.has_value();
                return haveResult;
            });

            if (success && !error.has_value())
                return error;
        }

        if (std::chrono::system_clock::now() >= deadline)
            return Error{Error::Code::Timeout, "Operation timed out after waking from guard, waiting for data at path: " + path.toString()};
    }
}

auto PathSpace::notify(std::string const& notificationPath) -> void {
    sp_log("PathSpace::notify", "Function Called");
    if (this->root)
        this->root->waitMap.notify(this->rootPath + notificationPath);
    else
        this->waitMap.notify(notificationPath);
}

} // namespace SP