#pragma once
#include "PathSpace.hpp"

namespace SP {

struct Permission {
    bool read    = true;
    bool write   = true;
    bool execute = true;
};

struct PathView : public PathSpace {
    PathView(std::function<Permission(Iterator const&)> permission, std::string root = "")
        : permission(permission), root(root) {}
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto clear() -> void override;

private:
    std::string                                root;
    std::function<Permission(Iterator const&)> permission;
};

} // namespace SP