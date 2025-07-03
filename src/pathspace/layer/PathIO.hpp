#pragma once
#include "PathSpace.hpp"

namespace SP {

struct Permission {
    bool read    = true;
    bool write   = true;
    bool execute = true;
};

struct PathIO : public PathSpaceBase {
    PathIO()
        : {}
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    virtual auto shutdown() -> void override;
    virtual auto notify(std::string const& notificationPath) -> void override;

private:
};

} // namespace SP