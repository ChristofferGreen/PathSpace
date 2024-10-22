#include "PathSpace.hpp"
#include "core/BlockOptions.hpp"

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
    this->shuttingDown = true;
    this->waitMap.notifyAll();
}

auto PathSpace::in(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
        -> InsertReturn {
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

auto PathSpace::out(ConcretePathStringView const& path,
                    InputMetadata const& inputMetadata,
                    OutOptions const& options,
                    Capabilities const& capabilities,
                    void* obj) -> Expected<int> {
    return this->root.out(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
}

} // namespace SP