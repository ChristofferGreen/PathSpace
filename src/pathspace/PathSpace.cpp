#include "PathSpace.hpp"
#include "core/BlockOptions.hpp"
#include <future>

namespace SP {

PathSpace::PathSpace(TaskPool* pool) {
    log("PathSpace::PathSpace", "Function Called");
    if (this->pool == nullptr)
        this->pool = &TaskPool::Instance();
};

PathSpace::~PathSpace() {
    log("PathSpace::~PathSpace", "Function Called");
    this->shutdown();
}

auto PathSpace::clear() -> void {
    log("PathSpace::clear", "Function Called");
    this->root.clear();
    this->waitMap.clear();
}

auto PathSpace::shutdown() -> void {
    log("PathSpace::shutdown", "Function Called");
    size_t count = this->taskToken.getTaskCount();
    if (count > 0) {
        log("PathSpace::shutdown Warning: Found " + std::to_string(count) + " uncleaned tasks at shutdown", "Warning");
    }
    if (this->taskToken.wasEverUsed()) {
        this->taskToken.invalidate(); // Prevent new tasks
        this->waitMap.notifyAll();
        this->root.clear();

        // Double check after clear
        count = this->taskToken.getTaskCount();
        if (count > 0) {
            log("PathSpace::shutdown Error: Still have " + std::to_string(count) + " tasks after clear", "Error");
        }

        auto waitResult = std::async(std::launch::async, [this]() {
            auto start = std::chrono::system_clock::now();
            while (true) {
                if (this->taskToken.getTaskCount() == 0) {
                    return true;
                }
                if (std::chrono::system_clock::now() - start > std::chrono::milliseconds(100)) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        if (waitResult.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout) {
            log("PathSpace::shutdown Warning: Shutdown timed out waiting for tasks", "Warning");
            // Force cleanup
            while (this->taskToken.getTaskCount() > 0) {
                this->taskToken.unregisterTask();
            }
        }
    }
}

auto PathSpace::in(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
        -> InsertReturn {
    log("PathSpace::in", "Function Called");
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
    log("PathSpace::out", "Function Called");
    return this->root.out(path.begin(), path.end(), inputMetadata, obj, options);
}

} // namespace SP