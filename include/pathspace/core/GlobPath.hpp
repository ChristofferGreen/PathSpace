#pragma once

#include "pathspace/core/GlobName.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace SP {

struct GlobPath {
    GlobPath(std::string_view const &stringv);

    auto validPath() const -> bool;
    auto currentName() const -> std::optional<GlobName>;
    auto toString() const -> std::string;

    auto moveToNextName() -> void;
private:
    std::string_view stringv;
    std::string_view::const_iterator currentNameIterator;
};

}