#include "PathSpace.hpp"
#include "core/BlockOptions.hpp"
#include <future>

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
    if (this->taskToken.wasEverUsed()) {
        this->taskToken.invalidate(); // Prevent new tasks
        this->waitMap.notifyAll();

        // Wait for tasks with exponential backoff
        auto waitForTasks = [this](std::chrono::milliseconds maxWait) -> bool {
            auto start = std::chrono::steady_clock::now();
            std::chrono::microseconds wait{100};

            while (true) {
                if (this->taskToken.getTaskCount() == 0) {
                    return true;
                }
                if (std::chrono::steady_clock::now() - start > maxWait) {
                    return false;
                }
                std::this_thread::sleep_for(wait);
                wait *= 2; // Exponential backoff
            }
        };

        // Try graceful shutdown
        if (!waitForTasks(std::chrono::milliseconds(500))) {
            sp_log("PathSpace::shutdown", "Warning: Extended wait for task completion");
            this->root.clear(); // Force cleanup data structures

            // Final wait with logging
            if (!waitForTasks(std::chrono::milliseconds(100))) {
                sp_log("PathSpace::shutdown", "Error: Failed to clean up all tasks");
                // Force cleanup remaining tasks
                while (this->taskToken.getTaskCount() > 0) {
                    this->taskToken.unregisterTask();
                }
            }
        }
    }
}

auto PathSpace::in(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
        -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;
    if (!path.isValid()) {
        ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
        return ret;
    }

    this->root.in(constructedPath, path.begin(), path.end(), data, options, ret);

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksCreated) {
        waitMap.notify(path.getPath()); // ToDo:: Fix glob path situation
    }
    return ret;
}

auto PathSpace::out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, OutOptions const& options, void* obj)
        -> Expected<int> {
    sp_log("PathSpace::out", "Function Called");
    return this->root.out(path.begin(), path.end(), inputMetadata, obj, options);
}

} // namespace SP