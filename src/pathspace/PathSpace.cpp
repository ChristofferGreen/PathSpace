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
    this->root.clear();
    this->waitMap.clear();
}

auto PathSpace::shutdown() -> void {
    sp_log("PathSpace::shutdown", "Function Called");
    sp_log("PathSpace::shutdown Starting shutdown", "PathSpaceShutdown");
    this->waitMap.notifyAll();
    sp_log("PathSpace::shutdown Notified all waiters", "PathSpaceShutdown");
    this->root.clear();
    sp_log("PathSpace::shutdown Cleared paths", "PathSpaceShutdown");
}

auto PathSpace::in(PathIterator const& path, InputData const& data) -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    this->root.in(path, data, ret);

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0)
        waitMap.notify(path.toStringView());
    return ret;
}

auto PathSpace::out(PathIterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    sp_log("PathSpace::outBlock", "Function Called");

    std::optional<Error> error;
    // First try entirely outside the loop to minimize lock time
    {
        error = this->root.out(path, inputMetadata, obj, options.doPop);
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
                error           = this->root.out(path, inputMetadata, obj, options.doPop);
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

} // namespace SP