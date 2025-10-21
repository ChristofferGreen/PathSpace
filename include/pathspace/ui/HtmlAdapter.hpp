#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
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

struct Asset;

struct EmitOptions {
    bool prefer_dom = true;
    bool allow_clip_path = true;
    std::size_t max_dom_nodes = 10'000;
    bool allow_canvas_fallback = true;
    std::function<SP::Expected<Asset>(std::string_view logical_path,
                                      std::uint64_t fingerprint,
                                      AssetKind kind)> resolve_asset;
    std::vector<std::string> font_logical_paths;
};

enum class CanvasCommandType : std::uint8_t {
    Rect,
    RoundedRect,
    Image,
    Text,
    Path,
    Mesh,
};

struct CanvasCommand {
    CanvasCommandType type = CanvasCommandType::Rect;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> corner_radii{0.0f, 0.0f, 0.0f, 0.0f};
    std::uint64_t fingerprint = 0;
    std::uint32_t glyph_count = 0;
    std::uint32_t vertex_count = 0;
    float opacity = 1.0f;
    bool has_fingerprint = false;
};

struct Asset {
    std::string logical_path;
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

struct EmitResult {
    std::string dom;
    std::string css;
    std::string canvas_commands;
    bool used_canvas_fallback = false;
    std::vector<Asset> assets;
    std::vector<CanvasCommand> canvas_replay_commands;
};

class Adapter {
public:
    Adapter() = default;
    ~Adapter() = default;

    Adapter(Adapter const&) = delete;
    Adapter& operator=(Adapter const&) = delete;
    Adapter(Adapter&&) noexcept = delete;
    Adapter& operator=(Adapter&&) noexcept = delete;

    [[nodiscard]] auto emit(Scene::DrawableBucketSnapshot const& snapshot,
                            EmitOptions const& options) -> SP::Expected<EmitResult>;
};

} // namespace SP::UI::Html
