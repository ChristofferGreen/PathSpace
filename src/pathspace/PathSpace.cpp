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

auto PathSpace::in(GlobPathStringView const& path, InputData const& data, In const& options) -> InsertReturn {
    sp_log("PathSpace::in", "Function Called");
    InsertReturn ret;

    this->root.in(PathViewGlob(path.begin(), path.end()), data, options, ret);

    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0 || ret.nbrTasksInserted > 0)
        waitMap.notify(path);
    return ret;
}

auto PathSpace::out(ConcretePathStringView const& path, InputMetadata const& inputMetadata, Out const& options, void* obj, bool const doExtract) -> Expected<int> {
    sp_log("PathSpace::out", "Function Called");
    return this->root.out(PathViewConcrete(path.begin(), path.end()), inputMetadata, obj, options, doExtract);
}

} // namespace SP