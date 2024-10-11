#include "PathSpace.hpp"

namespace SP {

PathSpace::PathSpace(TaskPool* pool) {
    if (this->pool == nullptr)
        this->pool = &TaskPool::Instance();
};

PathSpace::~PathSpace() {
    this->shutdown();
}

auto PathSpace::shutdown() -> void {
    std::unique_lock<std::mutex> lock(this->mutex);
    this->shuttingDown = true;
    this->cv.notify_all();
}

auto PathSpace::inImpl(ConstructiblePath& constructedPath, GlobPathStringView const& path, InputData const& data, InOptions const& options)
        -> InsertReturn {
    InsertReturn ret;
    if (!path.isValid()) {
        ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
        return ret;
    }
    this->root.in(constructedPath, path.begin(), path.end(), InputData{data}, options, ret, this->mutex);
    if (ret.nbrSpacesInserted > 0 || ret.nbrValuesInserted > 0)
        this->cv.notify_all();
    return ret;
};

auto PathSpace::outImpl(ConcretePathStringView const& path,
                        InputMetadata const& inputMetadata,
                        OutOptions const& options,
                        Capabilities const& capabilities,
                        void* obj) -> Expected<int> {
    auto checkAndRead
            = [&]() -> Expected<int> { return this->root.out(path.begin(), path.end(), inputMetadata, obj, options, capabilities); };

    auto result = checkAndRead();
    if (result.has_value() || options.block.value().behavior == BlockOptions::Behavior::DontWait) {
        return result;
    }

    std::unique_lock<std::mutex> lock(this->mutex);
    this->cv.wait(lock, [&]() {
        result = checkAndRead();
        return result.has_value() || this->shuttingDown;
    });

    if (this->shuttingDown) {
        return std::unexpected(Error{Error::Code::Shutdown, "PathSpace is shutting down"});
    }

    return result;
}

} // namespace SP