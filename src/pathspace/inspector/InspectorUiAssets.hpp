#pragma once

#include <string>
#include <string_view>

namespace SP::Inspector {

struct InspectorUiAsset {
    std::string content;
    std::string content_type;
};

[[nodiscard]] auto LoadInspectorUiAsset(std::string const& ui_root,
                                        std::string_view   relative_path) -> InspectorUiAsset;

} // namespace SP::Inspector
