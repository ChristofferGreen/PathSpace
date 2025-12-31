#pragma once

#include <pathspace/ui/scenegraph/RenderCommandStore.hpp>
#include <pathspace/ui/scenegraph/TileGrid.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/FontAtlas.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <parallel_hashmap/phmap.h>

namespace SP::UI::SceneGraph {

struct SoftwareTileRendererConfig {
    int32_t tile_width = 64;
    int32_t tile_height = 64;
    std::size_t max_bucket_size = 256;
    std::size_t max_workers = 0; // 0 => use hardware concurrency
};

struct SoftwareTileRenderStats {
    std::size_t tiles_total = 0;
    std::size_t tiles_dirty = 0;
    std::size_t tiles_rendered = 0;
    std::size_t commands_rendered = 0;
    std::size_t tile_jobs = 0;
    std::size_t workers_used = 0;
    double render_ms = 0.0;
};

class SoftwareTileRendererPayloads {
public:
    virtual ~SoftwareTileRendererPayloads() = default;

    [[nodiscard]] virtual auto rect(std::uint64_t handle) const
        -> std::optional<Scene::RectCommand> = 0;
    [[nodiscard]] virtual auto rounded_rect(std::uint64_t handle) const
        -> std::optional<Scene::RoundedRectCommand> = 0;
    [[nodiscard]] virtual auto text(std::uint64_t handle) const
        -> std::optional<Scene::TextGlyphsCommand> = 0;
    [[nodiscard]] virtual auto glyph_vertices() const -> std::span<Scene::TextGlyphVertex const> = 0;
    [[nodiscard]] virtual auto font_atlas(std::uint64_t fingerprint) const
        -> std::shared_ptr<FontAtlasData const> = 0;
};

class SpanPayloadProvider final : public SoftwareTileRendererPayloads {
public:
    std::span<Scene::RectCommand const> rects{};
    std::span<Scene::RoundedRectCommand const> rounded_rects{};
    std::span<Scene::TextGlyphsCommand const> texts{};
    std::span<Scene::TextGlyphVertex const> glyphs{};
    phmap::flat_hash_map<std::uint64_t, std::shared_ptr<FontAtlasData const>> atlases{};

    [[nodiscard]] auto rect(std::uint64_t handle) const
        -> std::optional<Scene::RectCommand> override;
    [[nodiscard]] auto rounded_rect(std::uint64_t handle) const
        -> std::optional<Scene::RoundedRectCommand> override;
    [[nodiscard]] auto text(std::uint64_t handle) const
        -> std::optional<Scene::TextGlyphsCommand> override;
    [[nodiscard]] auto glyph_vertices() const -> std::span<Scene::TextGlyphVertex const> override;
    [[nodiscard]] auto font_atlas(std::uint64_t fingerprint) const
        -> std::shared_ptr<FontAtlasData const> override;
};

struct TileRenderCommandView {
    IntRect                bbox{};
    int32_t                z = 0;
    float                  opacity = 1.0f;
    Scene::DrawCommandKind kind = Scene::DrawCommandKind::Rect;
    std::uint64_t          payload_handle = 0;
    std::uint64_t          entity_id = 0;
};

struct TileRenderSubmission {
    IntRect                                tile_rect{};
    std::span<TileRenderCommandView const> commands{};
};

struct TileRenderFrameInfo {
    int32_t       surface_width = 0;
    int32_t       surface_height = 0;
    int32_t       tile_width = 0;
    int32_t       tile_height = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t revision = 0;
};

class TileEncoderHooks {
public:
    virtual ~TileEncoderHooks() = default;

    virtual auto begin_frame(TileRenderFrameInfo const& info,
                             SoftwareTileRendererPayloads const& payloads) -> void {}

    // `commands` spans are valid only for the duration of the call.
    virtual auto encode_tile(TileRenderSubmission const& submission,
                             SoftwareTileRendererPayloads const& payloads) -> void = 0;

    virtual auto end_frame(SoftwareTileRenderStats const& stats,
                           SoftwareTileRendererPayloads const& payloads) -> void {}
};

class SoftwareTileRenderer {
public:
    SoftwareTileRenderer(PathSurfaceSoftware& surface, SoftwareTileRendererConfig cfg = {});

    auto configure(SoftwareTileRendererConfig cfg) -> void;

    auto render(RenderCommandStore const& commands,
                SoftwareTileRendererPayloads const& payloads,
                std::span<IntRect const> dirty_overrides = {},
                PathSurfaceSoftware::FrameInfo frame_info = {},
                TileEncoderHooks* hooks = nullptr) -> SoftwareTileRenderStats;

private:
    PathSurfaceSoftware& surface_;
    SoftwareTileRendererConfig cfg_{};
    std::vector<float> linear_;
    int width_ = 0;
    int height_ = 0;
    bool has_previous_frame_ = false;
};

} // namespace SP::UI::SceneGraph
