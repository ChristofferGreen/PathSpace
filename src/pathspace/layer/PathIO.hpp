#pragma once
#include "PathSpace.hpp"

namespace SP {

// PathIO is a base class for concrete IO providers (e.g., mice, keyboards).
// It deliberately has no knowledge of specific paths or device classes.
// Concrete subclasses (e.g., PathIOMice, PathIOKeyboard) implement behavior.
// TODO: future "link" class (path aliasing/symlinks) will be implemented separately.
struct PathIO : public PathSpaceBase {
    PathIO() = default;
    virtual auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    virtual auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    virtual auto shutdown() -> void override;
    virtual auto notify(std::string const& notificationPath) -> void override;
};

} // namespace SP