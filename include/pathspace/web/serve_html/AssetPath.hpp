#pragma once

#include <string_view>

namespace SP::ServeHtml {

[[nodiscard]] bool IsAssetPath(std::string_view value);

} // namespace SP::ServeHtml

