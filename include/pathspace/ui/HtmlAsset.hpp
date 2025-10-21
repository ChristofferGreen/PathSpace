#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Html {

inline constexpr std::string_view kImageAssetReferenceMime = "application/vnd.pathspace.image+ref";
inline constexpr std::string_view kFontAssetReferenceMime  = "application/vnd.pathspace.font+ref";

enum class AssetKind {
    Image,
    Font,
};

struct Asset {
    std::string logical_path;
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

} // namespace SP::UI::Html
