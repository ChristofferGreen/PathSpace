#pragma once
#include "Path.hpp"

#include <string_view>

namespace SP {

struct GlobPath : public Path<std::string_view> {

};

} // namespace SP