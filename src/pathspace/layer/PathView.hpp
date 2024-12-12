#pragma once
#include "PathSpace.hpp"

namespace SP {

struct PathView : public PathSpace {
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto clear() -> void override;
};

} // namespace SP