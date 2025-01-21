#pragma once
#include "PathSpace.hpp"

namespace SP {

struct Permission {
    bool read    = true;
    bool write   = true;
    bool execute = true;
};

struct PathView : public PathSpace {
    PathView(std::shared_ptr<PathSpace> const& space, std::function<Permission(Iterator const&)> permission, std::string root = "")
        : space(space), permission(permission), root(root) {}
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;

private:
    std::string                                root;
    std::function<Permission(Iterator const&)> permission;
    std::shared_ptr<PathSpace>                 space;
};

} // namespace SP