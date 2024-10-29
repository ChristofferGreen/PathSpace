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
    this->waitMap.notifyAll();
    this->root.clear();
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

auto PathSpace::out(ConcretePathStringView const& path,
                    InputMetadata const& inputMetadata,
                    OutOptions const& options,
                    void* obj,
                    bool const isExtract) -> Expected<int> {
    sp_log("PathSpace::out", "Function Called");
    return this->root.out(path.begin(), path.end(), inputMetadata, obj, options, isExtract);
}

} // namespace SP