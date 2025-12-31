#include <pathspace/ui/scenegraph/SoftwareTileRenderer.hpp>

#include <pathspace/ui/PathRenderer2DDetail.hpp>
#include <pathspace/ui/PathRenderer2DInternal.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace SP::UI::SceneGraph {
namespace {

[[nodiscard]] auto intersect(IntRect lhs, IntRect rhs) -> std::optional<IntRect> {
    IntRect clipped{
        .min_x = std::max(lhs.min_x, rhs.min_x),
        .min_y = std::max(lhs.min_y, rhs.min_y),
        .max_x = std::min(lhs.max_x, rhs.max_x),
        .max_y = std::min(lhs.max_y, rhs.max_y),
    };
    if (clipped.empty()) {
        return std::nullopt;
    }
    return clipped;
}

[[nodiscard]] auto is_bgra(Runtime::SurfaceDesc const& desc) -> bool {
    using Runtime::PixelFormat;
    return desc.pixel_format == PixelFormat::BGRA8Unorm
           || desc.pixel_format == PixelFormat::BGRA8Unorm_sRGB;
}

[[nodiscard]] auto build_tile_config(Runtime::SurfaceDesc const& desc,
                                     SoftwareTileRendererConfig const& cfg) -> TileGridConfig {
    TileGridConfig grid_cfg{};
    grid_cfg.tile_width = cfg.tile_width;
    grid_cfg.tile_height = cfg.tile_height;
    grid_cfg.max_bucket_size = cfg.max_bucket_size;
    grid_cfg.surface_width = std::max(desc.size_px.width, 0);
    grid_cfg.surface_height = std::max(desc.size_px.height, 0);
    return grid_cfg;
}

[[nodiscard]] auto z_sort_key(RenderCommandStore const& store, CommandId id)
    -> std::pair<int32_t, CommandId> {
    return {store.z(id), id};
}

template <typename T>
[[nodiscard]] auto clamp_to_tile(T const& command, IntRect const& tile) -> T {
    T copy = command;
    copy.min_x = std::max(copy.min_x, static_cast<float>(tile.min_x));
    copy.min_y = std::max(copy.min_y, static_cast<float>(tile.min_y));
    copy.max_x = std::min(copy.max_x, static_cast<float>(tile.max_x));
    copy.max_y = std::min(copy.max_y, static_cast<float>(tile.max_y));
    return copy;
}

template <typename T>
[[nodiscard]] auto with_opacity(T const& command, float opacity) -> T {
    auto copy = command;
    auto const clamped = std::clamp(opacity, 0.0f, 1.0f);
    for (auto& channel : copy.color) {
        channel *= clamped;
    }
    return copy;
}

// Helpers adapted from PathRenderer2DDetailDraw.cpp for glyph rendering within tiles.
auto sample_font_atlas_alpha(FontAtlasData const& atlas, float u, float v) -> float {
    if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.empty()) {
        return 0.0f;
    }
    u = PathRenderer2DDetail::clamp_unit(u);
    v = PathRenderer2DDetail::clamp_unit(v);
    auto max_x = static_cast<float>(atlas.width - 1);
    auto max_y = static_cast<float>(atlas.height - 1);
    auto x = std::clamp(static_cast<int>(std::round(u * max_x)), 0, static_cast<int>(atlas.width - 1));
    auto y = std::clamp(static_cast<int>(std::round(v * max_y)), 0, static_cast<int>(atlas.height - 1));
    auto index = static_cast<std::size_t>(y) * atlas.width + static_cast<std::size_t>(x);
    if (index >= atlas.pixels.size()) {
        return 0.0f;
    }
    return PathRenderer2DDetail::clamp_unit(static_cast<float>(atlas.pixels[index]) / 255.0f);
}

auto sample_font_atlas_rgba(FontAtlasData const& atlas, float u, float v)
    -> PathRenderer2DDetail::LinearPremulColor {
    using PathRenderer2DDetail::LinearPremulColor;
    LinearPremulColor color{};
    if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.empty() || atlas.bytes_per_pixel < 4) {
        return color;
    }
    u = PathRenderer2DDetail::clamp_unit(u);
    v = PathRenderer2DDetail::clamp_unit(v);
    auto max_x = static_cast<float>(atlas.width - 1);
    auto max_y = static_cast<float>(atlas.height - 1);
    auto x = std::clamp(static_cast<int>(std::round(u * max_x)), 0, static_cast<int>(atlas.width - 1));
    auto y = std::clamp(static_cast<int>(std::round(v * max_y)), 0, static_cast<int>(atlas.height - 1));
    auto index = (static_cast<std::size_t>(y) * atlas.width + static_cast<std::size_t>(x))
                 * static_cast<std::size_t>(atlas.bytes_per_pixel);
    if (index + 3 >= atlas.pixels.size()) {
        return color;
    }
    float r = static_cast<float>(atlas.pixels[index + 0]) / 255.0f;
    float g = static_cast<float>(atlas.pixels[index + 1]) / 255.0f;
    float b = static_cast<float>(atlas.pixels[index + 2]) / 255.0f;
    float a = static_cast<float>(atlas.pixels[index + 3]) / 255.0f;
    a = PathRenderer2DDetail::clamp_unit(a);
    color.a = a;
    color.r = PathRenderer2DDetail::clamp_unit(r * a);
    color.g = PathRenderer2DDetail::clamp_unit(g * a);
    color.b = PathRenderer2DDetail::clamp_unit(b * a);
    return color;
}

auto blend_pixel(float* dest, PathRenderer2DDetail::LinearPremulColor const& src) -> void {
    auto const inv_alpha = 1.0f - src.a;
    dest[0] = PathRenderer2DDetail::clamp_unit(src.r + dest[0] * inv_alpha);
    dest[1] = PathRenderer2DDetail::clamp_unit(src.g + dest[1] * inv_alpha);
    dest[2] = PathRenderer2DDetail::clamp_unit(src.b + dest[2] * inv_alpha);
    dest[3] = PathRenderer2DDetail::clamp_unit(src.a + dest[3] * inv_alpha);
}

auto draw_text_shaped(RenderCommandStore const& store,
                      CommandId id,
                      SoftwareTileRendererPayloads const& payloads,
                      IntRect const& tile,
                      std::vector<float>& linear,
                      int width,
                      int height) -> bool {
    auto cmd = payloads.text(store.payload_handle(id));
    if (!cmd) {
        return false;
    }
    auto const glyphs = payloads.glyph_vertices();
    if (cmd->glyph_offset > glyphs.size()
        || static_cast<std::size_t>(cmd->glyph_offset + cmd->glyph_count) > glyphs.size()) {
        return false;
    }
    auto atlas = payloads.font_atlas(cmd->atlas_fingerprint);
    if (!atlas) {
        return false;
    }

    auto const* glyph_ptr = glyphs.data() + static_cast<std::size_t>(cmd->glyph_offset);
    auto const glyph_count = static_cast<std::size_t>(cmd->glyph_count);

    auto tinted_color = PathRenderer2DDetail::make_linear_straight(cmd->color);
    auto const opacity = std::clamp(store.opacity(id), 0.0f, 1.0f);
    tinted_color.r *= opacity;
    tinted_color.g *= opacity;
    tinted_color.b *= opacity;
    tinted_color.a *= opacity;
    auto base_color = PathRenderer2DDetail::premultiply(tinted_color);
    auto tint_straight = tinted_color;

    auto const uses_color_atlas = (cmd->flags & Scene::kTextGlyphsFlagUsesColorAtlas) != 0u;
    auto const row_stride = static_cast<std::size_t>(width) * 4u;
    bool drawn = false;

    auto clamp_to_surface = [&](int value, int limit) {
        return std::clamp(value, 0, limit);
    };

    for (std::size_t index = 0; index < glyph_count; ++index) {
        auto const& glyph = glyph_ptr[index];
        auto glyph_min_x = std::min(glyph.min_x, glyph.max_x);
        auto glyph_max_x = std::max(glyph.min_x, glyph.max_x);
        auto glyph_min_y = std::min(glyph.min_y, glyph.max_y);
        auto glyph_max_y = std::max(glyph.min_y, glyph.max_y);

        auto width_f = glyph_max_x - glyph_min_x;
        auto height_f = glyph_max_y - glyph_min_y;
        if (width_f <= 0.0f || height_f <= 0.0f) {
            continue;
        }

        auto min_x_i = clamp_to_surface(static_cast<int>(std::floor(glyph_min_x)), width);
        auto max_x_i = clamp_to_surface(static_cast<int>(std::ceil(glyph_max_x)), width);
        auto min_y_i = clamp_to_surface(static_cast<int>(std::floor(glyph_min_y)), height);
        auto max_y_i = clamp_to_surface(static_cast<int>(std::ceil(glyph_max_y)), height);

        min_x_i = std::clamp(min_x_i, tile.min_x, tile.max_x);
        max_x_i = std::clamp(max_x_i, tile.min_x, tile.max_x);
        min_y_i = std::clamp(min_y_i, tile.min_y, tile.max_y);
        max_y_i = std::clamp(max_y_i, tile.min_y, tile.max_y);

        if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
            continue;
        }

        auto u_range = glyph.u1 - glyph.u0;
        auto v_range = glyph.v1 - glyph.v0;
        if (std::fabs(u_range) <= std::numeric_limits<float>::epsilon()
            || std::fabs(v_range) <= std::numeric_limits<float>::epsilon()) {
            continue;
        }

        for (int y = min_y_i; y < max_y_i; ++y) {
            auto base_index = static_cast<std::size_t>(y) * row_stride;
            for (int x = min_x_i; x < max_x_i; ++x) {
                float local_x = (static_cast<float>(x) + 0.5f - glyph_min_x) / width_f;
                float local_y = (static_cast<float>(y) + 0.5f - glyph_min_y) / height_f;
                auto atlas_u = glyph.u0 + u_range * PathRenderer2DDetail::clamp_unit(local_x);
                auto atlas_v = glyph.v0 + v_range * PathRenderer2DDetail::clamp_unit(local_y);

                PathRenderer2DDetail::LinearPremulColor src{};
                if (uses_color_atlas && atlas->format == FontAtlasFormat::Rgba8) {
                    src = sample_font_atlas_rgba(*atlas, atlas_u, atlas_v);
                    if (src.a == 0.0f) {
                        continue;
                    }
                    src.r = PathRenderer2DDetail::clamp_unit(src.r * tint_straight.r);
                    src.g = PathRenderer2DDetail::clamp_unit(src.g * tint_straight.g);
                    src.b = PathRenderer2DDetail::clamp_unit(src.b * tint_straight.b);
                    src.a = PathRenderer2DDetail::clamp_unit(src.a * tint_straight.a);
                } else {
                    auto alpha = sample_font_atlas_alpha(*atlas, atlas_u, atlas_v);
                    if (alpha <= 0.0f) {
                        continue;
                    }
                    src.r = PathRenderer2DDetail::clamp_unit(base_color.r * alpha);
                    src.g = PathRenderer2DDetail::clamp_unit(base_color.g * alpha);
                    src.b = PathRenderer2DDetail::clamp_unit(base_color.b * alpha);
                    src.a = PathRenderer2DDetail::clamp_unit(base_color.a * alpha);
                }

                auto* dest = linear.data() + base_index + static_cast<std::size_t>(x) * 4u;
                blend_pixel(dest, src);
                drawn = true;
            }
        }
    }

    return drawn;
}

auto draw_command(Scene::DrawCommandKind kind,
                  RenderCommandStore const& store,
                  CommandId id,
                  SoftwareTileRendererPayloads const& payloads,
                  IntRect const& tile,
                  std::vector<float>& linear,
                  int width,
                  int height) -> bool {
    switch (kind) {
    case Scene::DrawCommandKind::Rect: {
        auto payload = payloads.rect(store.payload_handle(id));
        if (!payload) {
            return false;
        }
        auto const adjusted = with_opacity(*payload, store.opacity(id));
        auto clipped = clamp_to_tile(adjusted, tile);
        return PathRenderer2DDetail::draw_rect_command(clipped, linear, width, height);
    }
    case Scene::DrawCommandKind::RoundedRect: {
        auto payload = payloads.rounded_rect(store.payload_handle(id));
        if (!payload) {
            return false;
        }
        auto const adjusted = with_opacity(*payload, store.opacity(id));
        PathRenderer2DInternal::DamageRect clip{};
        clip.min_x = tile.min_x;
        clip.min_y = tile.min_y;
        clip.max_x = tile.max_x;
        clip.max_y = tile.max_y;
        return PathRenderer2DDetail::draw_rounded_rect_command(
            adjusted, linear, width, height, std::span{&clip, 1});
    }
    case Scene::DrawCommandKind::TextGlyphs: {
        if (draw_text_shaped(store, id, payloads, tile, linear, width, height)) {
            return true;
        }
        auto payload = payloads.text(store.payload_handle(id));
        if (!payload) {
            return false;
        }
        auto const adjusted = with_opacity(*payload, store.opacity(id));
        auto clipped = clamp_to_tile(adjusted, tile);
        return PathRenderer2DDetail::draw_text_glyphs_command(clipped, linear, width, height);
    }
    default:
        break;
    }
    return false;
}

} // namespace

auto SpanPayloadProvider::rect(std::uint64_t handle) const -> std::optional<Scene::RectCommand> {
    if (handle >= rects.size()) {
        return std::nullopt;
    }
    return rects[static_cast<std::size_t>(handle)];
}

auto SpanPayloadProvider::rounded_rect(std::uint64_t handle) const
    -> std::optional<Scene::RoundedRectCommand> {
    if (handle >= rounded_rects.size()) {
        return std::nullopt;
    }
    return rounded_rects[static_cast<std::size_t>(handle)];
}

auto SpanPayloadProvider::text(std::uint64_t handle) const
    -> std::optional<Scene::TextGlyphsCommand> {
    if (handle >= texts.size()) {
        return std::nullopt;
    }
    return texts[static_cast<std::size_t>(handle)];
}

auto SpanPayloadProvider::glyph_vertices() const -> std::span<Scene::TextGlyphVertex const> {
    return glyphs;
}

auto SpanPayloadProvider::font_atlas(std::uint64_t fingerprint) const
    -> std::shared_ptr<FontAtlasData const> {
    auto it = atlases.find(fingerprint);
    if (it == atlases.end()) {
        return nullptr;
    }
    return it->second;
}

SoftwareTileRenderer::SoftwareTileRenderer(PathSurfaceSoftware& surface,
                                           SoftwareTileRendererConfig cfg)
    : surface_(surface)
    , cfg_(cfg) {}

auto SoftwareTileRenderer::configure(SoftwareTileRendererConfig cfg) -> void {
    cfg_ = cfg;
}

auto SoftwareTileRenderer::render(RenderCommandStore const& commands,
                                  SoftwareTileRendererPayloads const& payloads,
                                  std::span<IntRect const> dirty_overrides,
                                  PathSurfaceSoftware::FrameInfo frame_info,
                                  TileEncoderHooks* hooks) -> SoftwareTileRenderStats {
    SoftwareTileRenderStats stats{};
    auto const desc = surface_.desc();
    auto const width = std::max(desc.size_px.width, 0);
    auto const height = std::max(desc.size_px.height, 0);
    auto const pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    bool size_changed = width != width_ || height != height_;
    if (size_changed) {
        has_previous_frame_ = false;
        width_ = width;
        height_ = height;
    }

    auto const start = std::chrono::steady_clock::now();

    auto const grid_cfg = build_tile_config(desc, cfg_);
    TileGrid grid(grid_cfg);

    auto const active_ids = commands.active_ids();
    for (auto id : active_ids) {
        grid.mark_dirty(commands.bbox(id), id);
    }

    auto const tiles = grid.tiles();
    auto tile_indices = grid.dirty_tiles();
    if (!dirty_overrides.empty()) {
        for (auto const& rect : dirty_overrides) {
            auto extra = grid.tiles_for_rect(rect);
            tile_indices.insert(tile_indices.end(), extra.begin(), extra.end());
        }
        std::sort(tile_indices.begin(), tile_indices.end());
        tile_indices.erase(std::unique(tile_indices.begin(), tile_indices.end()), tile_indices.end());
    }

    stats.tiles_total = tiles.size();
    TileRenderFrameInfo hook_frame{
        .surface_width = width,
        .surface_height = height,
        .tile_width = cfg_.tile_width,
        .tile_height = cfg_.tile_height,
        .frame_index = frame_info.frame_index,
        .revision = frame_info.revision,
    };

    struct TileJob {
        std::size_t tile_index = 0;
        IntRect tile_rect{};
        std::vector<CommandId> commands{};
    };

    std::vector<TileJob> jobs;
    jobs.reserve(tile_indices.size());

    for (auto idx : tile_indices) {
        if (idx >= tiles.size()) {
            continue;
        }
        auto const& tile = tiles[idx];
        IntRect tile_rect{
            .min_x = tile.x,
            .min_y = tile.y,
            .max_x = tile.x + tile.width,
            .max_y = tile.y + tile.height,
        };

        if (!dirty_overrides.empty()) {
            bool intersects_override = false;
            for (auto const& override_rect : dirty_overrides) {
                if (intersect(tile_rect, override_rect).has_value()) {
                    intersects_override = true;
                    break;
                }
            }
            if (!intersects_override) {
                continue;
            }
        }

        auto const& bucket = grid.bucket(idx);

        TileJob job{};
        job.tile_index = idx;
        job.tile_rect = tile_rect;
        job.commands.assign(bucket.begin(), bucket.end());
        std::sort(job.commands.begin(), job.commands.end(), [&](CommandId lhs, CommandId rhs) {
            return z_sort_key(commands, lhs) < z_sort_key(commands, rhs);
        });
        jobs.push_back(std::move(job));
    }

    stats.tiles_dirty = jobs.size();
    stats.tile_jobs = jobs.size();

    if (hooks != nullptr) {
        hooks->begin_frame(hook_frame, payloads);
        std::vector<TileRenderCommandView> hook_commands;
        for (auto const& job : jobs) {
            hook_commands.clear();
            hook_commands.reserve(job.commands.size());
            for (auto id : job.commands) {
                hook_commands.push_back(TileRenderCommandView{
                    .bbox = commands.bbox(id),
                    .z = commands.z(id),
                    .opacity = commands.opacity(id),
                    .kind = commands.kind(id),
                    .payload_handle = commands.payload_handle(id),
                    .entity_id = commands.entity_id(id),
                });
            }
            TileRenderSubmission submission{
                .tile_rect = job.tile_rect,
                .commands = hook_commands,
            };
            hooks->encode_tile(submission, payloads);
        }
    }

    PathRenderer2DDetail::ensure_linear_buffer_capacity(linear_, pixel_count);

    bool render_full_frame = !has_previous_frame_ || dirty_overrides.empty();
    if (render_full_frame) {
        std::fill(linear_.begin(), linear_.end(), 0.0f);
        has_previous_frame_ = true;
    } else {
        auto const row_stride = static_cast<std::size_t>(width) * 4u;
        for (auto const& job : jobs) {
            for (int y = job.tile_rect.min_y; y < job.tile_rect.max_y; ++y) {
                auto const offset = static_cast<std::size_t>(y) * row_stride
                                    + static_cast<std::size_t>(job.tile_rect.min_x) * 4u;
                auto const count = static_cast<std::size_t>(job.tile_rect.max_x - job.tile_rect.min_x) * 4u;
                if (offset >= linear_.size()) {
                    continue;
                }
                auto const clamped_count = std::min(count, linear_.size() - offset);
                std::fill_n(linear_.begin() + static_cast<std::ptrdiff_t>(offset),
                            static_cast<std::ptrdiff_t>(clamped_count),
                            0.0f);
            }
        }
    }

    std::atomic<std::size_t> tiles_rendered{0};
    std::atomic<std::size_t> commands_rendered{0};

    auto process_job = [&](TileJob const& job) {
        for (auto id : job.commands) {
            auto const clipped_bbox = intersect(commands.bbox(id), job.tile_rect);
            if (!clipped_bbox) {
                continue;
            }
            if (draw_command(commands.kind(id),
                             commands,
                             id,
                             payloads,
                             *clipped_bbox,
                             linear_,
                             width,
                             height)) {
                commands_rendered.fetch_add(1, std::memory_order_relaxed);
            }
        }
        tiles_rendered.fetch_add(1, std::memory_order_relaxed);
    };

    auto choose_worker_count = [&](std::size_t job_count) -> std::size_t {
        if (job_count <= 1) {
            return job_count;
        }
        auto limit = cfg_.max_workers > 0
                         ? cfg_.max_workers
                         : static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (limit == 0) {
            limit = 1;
        }
        return std::max<std::size_t>(1, std::min(job_count, limit));
    };

    if (jobs.empty()) {
        stats.workers_used = 0;
    } else {
        auto const worker_count = choose_worker_count(jobs.size());
        if (worker_count <= 1) {
            for (auto const& job : jobs) {
                process_job(job);
            }
            stats.workers_used = 1;
        } else {
            std::atomic<std::size_t> next{0};
            auto worker = [&]() {
                while (true) {
                    auto const idx = next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= jobs.size()) {
                        break;
                    }
                    process_job(jobs[idx]);
                }
            };

            std::vector<std::thread> threads;
            threads.reserve(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                threads.emplace_back(worker);
            }
            for (auto& t : threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            stats.workers_used = worker_count;
        }
    }

    stats.tiles_rendered = tiles_rendered.load(std::memory_order_relaxed);
    stats.commands_rendered = commands_rendered.load(std::memory_order_relaxed);

    auto staging = surface_.staging_span();
    PathRenderer2DDetail::EncodeContext encode_ctx{
        .staging = staging.data(),
        .row_stride_bytes = surface_.row_stride_bytes(),
        .linear = linear_.data(),
        .width = width,
        .height = height,
        .desc = &desc,
        .encode_srgb = PathRenderer2DDetail::needs_srgb_encode(desc),
        .is_bgra = is_bgra(desc),
    };

    PathRenderer2DDetail::EncodeJob encode_job{
        .min_x = 0,
        .max_x = width,
        .start_y = 0,
        .end_y = height,
    };
    std::array<PathRenderer2DDetail::EncodeJob, 1> encode_jobs{encode_job};
    PathRenderer2DDetail::run_encode_jobs(encode_jobs, encode_ctx);

    auto const end = std::chrono::steady_clock::now();
    stats.render_ms = std::chrono::duration<double, std::milli>(end - start).count();
    frame_info.render_ms = stats.render_ms;
    surface_.publish_buffered_frame(frame_info);
    has_previous_frame_ = true;
    if (hooks != nullptr) {
        hooks->end_frame(stats, payloads);
    }
    return stats;
}

} // namespace SP::UI::SceneGraph
