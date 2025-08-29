#pragma once
#include "PathSpace.hpp"

namespace SP {

struct PathFileSystem : public PathSpaceBase {
    PathFileSystem(std::string root = "")
        : root(root) {}
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    virtual auto shutdown() -> void override;
    virtual auto notify(std::string const& notificationPath) -> void override;

private:
    std::string root;
};

} // namespace SP