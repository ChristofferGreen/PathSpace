#include <pathspace/ui/PathRenderer2D.hpp>

#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include "PathRenderer2DInternal.hpp"
#include "PathRenderer2DDetail.hpp"
#include "DrawableUtils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <atomic>
#include <numeric>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <thread>
#include <mutex>
#include <exception>
#include <cctype>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

#if defined(__APPLE__) && PATHSPACE_UI_METAL
#include <pathspace/ui/PathRenderer2DMetal.hpp>
#endif

namespace SP::UI {
using PathRenderer2DInternal::DamageRect;
using PathRenderer2DInternal::DamageRegion;
using PathRenderer2DInternal::DamageComputationOptions;
using PathRenderer2DInternal::DamageComputationResult;
using PathRenderer2DInternal::ProgressiveTileCopyContext;
using PathRenderer2DInternal::ProgressiveTileCopyStats;
using PathRenderer2DInternal::choose_progressive_tile_size;
using PathRenderer2DInternal::copy_progressive_tiles;
using PathRenderer2DInternal::compute_damage;
using namespace PathRenderer2DDetail;


PathRenderer2D::PathRenderer2D(PathSpace& space)
    : space_(space) {}

auto PathRenderer2D::target_cache() -> TargetCache& {
    static TargetCache cache;
    return cache;
}

auto PathRenderer2D::render(RenderParams params) -> SP::Expected<RenderStats> {
    auto const start = std::chrono::steady_clock::now();
    double damage_ms = 0.0;
    double encode_ms = 0.0;
    double progressive_copy_ms = 0.0;
    double publish_ms = 0.0;
    double encode_stall_ms_total = 0.0;
    double encode_stall_ms_max = 0.0;
    std::size_t encode_stall_workers = 0;

    auto app_root = SP::App::derive_app_root(params.target_path);
    if (!app_root) {
        auto message = std::string{"unable to derive application root for target"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(app_root.error());
    }

    auto sceneField = std::string(params.target_path.getPath()) + "/scene";
    auto sceneRel = space_.read<std::string, std::string>(sceneField);
    if (!sceneRel) {
        auto message = std::string{"target missing scene binding"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, sceneRel.error().code));
    }
    if (sceneRel->empty()) {
        auto message = std::string{"target scene binding is empty"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root->getPath()},
                                                       *sceneRel);
    if (!sceneAbsolute) {
        auto message = std::string{"failed to resolve scene path '"} + *sceneRel + "'";
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(sceneAbsolute.error());
    }

    auto sceneRevision = Runtime::Scene::ReadCurrentRevision(space_, Runtime::ScenePath{sceneAbsolute->getPath()});
    if (!sceneRevision) {
        auto message = std::string{"scene has no current revision"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(sceneRevision.error());
    }

    auto revisionBase = std::string(sceneAbsolute->getPath()) + "/builds/" + format_revision(sceneRevision->revision);
    auto bucket = Scene::SceneSnapshotBuilder::decode_bucket(space_, revisionBase);
    if (!bucket) {
        auto message = std::string{"failed to load snapshot bucket for revision "} + std::to_string(sceneRevision->revision);
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(bucket.error());
    }


    auto& surface = params.surface;
    auto const& desc = surface.desc();

    bool has_buffered = surface.has_buffered();
    bool const has_progressive = surface.has_progressive();
    if (!has_buffered && !has_progressive) {
        auto message = std::string{"surface has neither buffered nor progressive storage"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }

    if (params.settings.surface.size_px.width != 0
        && params.settings.surface.size_px.width != desc.size_px.width) {
        auto message = std::string{"render settings width does not match surface descriptor"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }
    if (params.settings.surface.size_px.height != 0
        && params.settings.surface.size_px.height != desc.size_px.height) {
        auto message = std::string{"render settings height does not match surface descriptor"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }

    switch (desc.pixel_format) {
    case Runtime::PixelFormat::RGBA8Unorm:
    case Runtime::PixelFormat::BGRA8Unorm:
    case Runtime::PixelFormat::RGBA8Unorm_sRGB:
    case Runtime::PixelFormat::BGRA8Unorm_sRGB:
        break;
    default: {
        auto message = std::string{"pixel format not supported by PathRenderer2D"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }
    }

    auto const width = desc.size_px.width;
    auto const height = desc.size_px.height;
    if (width <= 0 || height <= 0) {
        auto message = std::string{"surface dimensions must be positive"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }


    auto const pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    int const tile_size_px = std::max(1, desc.progressive_tile_size_px);

    bool metal_active = false;
#if defined(__APPLE__) && PATHSPACE_UI_METAL
    bool prefer_metal_backend = params.backend_kind == SP::UI::Runtime::RendererKind::Metal2D
                                && params.metal_surface != nullptr;
    std::unique_ptr<PathRenderer2DMetal> metal_backend;
    std::array<float, 4> metal_clear_rgba{};
    if (prefer_metal_backend) {
        metal_backend = std::make_unique<PathRenderer2DMetal>();
    }
#endif
    auto target_key = std::string(params.target_path.getPath());
    auto& cache = target_cache();
    std::shared_ptr<TargetCacheEntry> cache_entry;
    {
        std::lock_guard<std::mutex> cache_lock(cache.mutex);
        auto [it, inserted] = cache.entries.try_emplace(target_key, std::make_shared<TargetCacheEntry>());
        cache_entry = it->second;
    }
    auto target_state_lock = std::unique_lock<std::mutex>(cache_entry->mutex);
    auto& state = cache_entry->state;
    auto& material_descriptors = state.material_descriptors;
    material_descriptors.clear();
    if (!bucket->material_ids.empty()) {
        material_descriptors.reserve(bucket->material_ids.size());
    }
    phmap::flat_hash_map<std::uint64_t, MaterialResourceResidency> resource_residency;
    phmap::flat_hash_map<std::uint64_t, std::shared_ptr<FontAtlasData const>> font_atlases;
    phmap::flat_hash_set<std::uint64_t> processed_font_assets;
    if (!bucket->font_assets.empty()) {
        for (auto const& asset : bucket->font_assets) {
            if (asset.resource_root.empty()) {
                continue;
            }
            if (!processed_font_assets.insert(asset.fingerprint).second) {
                continue;
            }
            auto atlas_path = asset.resource_root + "/builds/" + format_revision(asset.revision) + "/atlas.bin";
            auto atlas = font_atlas_cache_.load(space_, atlas_path, asset.fingerprint);
            if (!atlas) {
                auto const& error = atlas.error();
                if (error.code != SP::Error::Code::NoObjectFound
                    && error.code != SP::Error::Code::NoSuchPath) {
                    auto message = error.message.value_or("failed to load font atlas");
                    (void)set_last_error(space_, params.target_path, message);
                }
                continue;
            }
            font_atlases[asset.fingerprint] = *atlas;
            auto const& data = **atlas;
            auto& residency = resource_residency[asset.fingerprint];
            residency.fingerprint = asset.fingerprint;
            residency.cpu_bytes = static_cast<std::uint64_t>(data.pixels.size());
            residency.gpu_bytes = static_cast<std::uint64_t>(data.width)
                                  * static_cast<std::uint64_t>(data.height)
                                  * static_cast<std::uint64_t>(std::max<std::uint32_t>(1u, data.bytes_per_pixel));
            residency.width = data.width;
            residency.height = data.height;
            residency.uses_font_atlas = true;
        }
    }

    std::vector<SP::UI::Runtime::DirtyRectHint> dirty_rect_hints;
    std::vector<SP::UI::Runtime::DirtyRectHint> damage_tile_hints;
    {
        auto hints_path = target_key + "/hints/dirtyRects";
        auto hints = space_.take<std::vector<SP::UI::Runtime::DirtyRectHint>>(hints_path);
        if (hints) {
            dirty_rect_hints = std::move(*hints);
        } else {
            auto const& error = hints.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    bool size_changed = state.desc.size_px.width != desc.size_px.width
                        || state.desc.size_px.height != desc.size_px.height;
    bool format_changed = state.desc.pixel_format != desc.pixel_format
                          || state.desc.color_space != desc.color_space
                          || state.desc.premultiplied_alpha != desc.premultiplied_alpha;

    if (ensure_linear_buffer_capacity(state.linear_buffer, pixel_count)) {
        size_changed = true;
    }
    auto& linear_buffer = state.linear_buffer;

    auto current_clear = params.settings.clear_color;
    auto clear_linear = make_linear_color(current_clear);

#if defined(__APPLE__) && PATHSPACE_UI_METAL
    if (prefer_metal_backend && metal_backend) {
        metal_clear_rgba = encode_linear_color_to_output(clear_linear, desc);
        if (metal_backend->begin_frame(*params.metal_surface, desc, metal_clear_rgba)) {
            metal_active = true;
        } else {
            metal_backend.reset();
        }
    }
#endif

    bool full_repaint = size_changed
                        || format_changed
                        || state.last_revision == 0
                        || state.clear_color != current_clear
                        || metal_active;

    auto const damage_phase_start = std::chrono::steady_clock::now();
    auto const drawable_count = bucket->drawable_ids.size();
    std::vector<std::optional<PathRenderer2D::DrawableBounds>> bounds_by_index(drawable_count);
    PathRenderer2D::DrawableStateMap current_states;
    current_states.reserve(drawable_count);

    auto const& drawable_fingerprints = bucket->drawable_fingerprints;

    bool missing_bounds = false;
    for (std::uint32_t i = 0; i < drawable_count; ++i) {
        auto maybe_bounds = compute_drawable_bounds(*bucket, i, width, height);
        if (maybe_bounds) {
            bounds_by_index[i] = maybe_bounds;
        } else {
            missing_bounds = true;
        }

        PathRenderer2D::DrawableState drawable_state{};
        if (maybe_bounds) {
            drawable_state.bounds = *maybe_bounds;
        }
        if (i < drawable_fingerprints.size()) {
            drawable_state.fingerprint = drawable_fingerprints[i];
        }
        current_states.emplace(bucket->drawable_ids[i], drawable_state);
    }
    if (missing_bounds) {
        full_repaint = true;
    }

    bool const collect_damage_metrics = damage_metrics_enabled();
    std::uint64_t fingerprint_matches_exact = 0;
    std::uint64_t fingerprint_matches_remap = 0;
    std::uint64_t fingerprint_changed = 0;
    std::uint64_t fingerprint_new = 0;
    std::uint64_t fingerprint_removed = 0;
    std::uint64_t damage_rect_count = 0;
    double damage_coverage_ratio = 0.0;
    std::uint64_t progressive_tiles_dirty = 0;
    std::uint64_t progressive_tiles_total = 0;
    std::uint64_t progressive_tiles_skipped = 0;
    std::uint64_t target_texture_bytes = 0;

    DamageComputationOptions damage_options{
        .width = width,
        .height = height,
        .tile_size_px = tile_size_px,
        .force_full_repaint = full_repaint,
        .missing_bounds = missing_bounds,
        .collect_damage_metrics = collect_damage_metrics,
    };

    auto damage_result = compute_damage(damage_options, state.drawable_states, current_states, dirty_rect_hints);
    DamageRegion damage = std::move(damage_result.damage);
    damage_tile_hints = std::move(damage_result.damage_tiles);
    full_repaint = damage_result.full_repaint;

    if (collect_damage_metrics) {
        fingerprint_matches_exact = damage_result.statistics.fingerprint_matches_exact;
        fingerprint_matches_remap = damage_result.statistics.fingerprint_matches_remap;
        fingerprint_changed = damage_result.statistics.fingerprint_changed;
        fingerprint_new = damage_result.statistics.fingerprint_new;
        fingerprint_removed = damage_result.statistics.fingerprint_removed;
        damage_rect_count = damage_result.statistics.damage_rect_count;
        damage_coverage_ratio = damage_result.statistics.damage_coverage_ratio;
    }

    if (!damage_result.hint_rectangles.empty()) {
        if (auto* trace = std::getenv("PATHSPACE_TRACE_DAMAGE")) {
            (void)trace;
            std::cout << "hint rectangles (" << damage_result.hint_rectangles.size() << ")"
                      << std::endl;
            for (auto const& rect : damage_result.hint_rectangles) {
                std::cout << "  hint=" << rect.min_x << ',' << rect.min_y
                          << " -> " << rect.max_x << ',' << rect.max_y << std::endl;
            }
        }
    }


    bool const has_damage = !damage.empty();
    auto const damage_phase_end = std::chrono::steady_clock::now();
    damage_ms = std::chrono::duration<double, std::milli>(damage_phase_end - damage_phase_start).count();

#if defined(__APPLE__) && PATHSPACE_UI_METAL
    if (metal_active) {
        bool metal_supported = true;
        for (auto kind_value : bucket->command_kinds) {
            auto kind = static_cast<Scene::DrawCommandKind>(kind_value);
            if (!metal_supports_command(kind)) {
                metal_supported = false;
                break;
            }
        }
        if (!metal_supported) {
            metal_backend.reset();
            metal_active = false;
        }
    }
#endif

    if (has_damage) {
        if (auto* trace = std::getenv("PATHSPACE_TRACE_DAMAGE")) {
            (void)trace;
            auto rects = damage.rectangles();
            std::cout << "damage rectangles (" << rects.size() << ")" << std::endl;
            for (auto const& rect : rects) {
                std::cout << "  rect=" << rect.min_x << ',' << rect.min_y
                          << " -> " << rect.max_x << ',' << rect.max_y << std::endl;
            }
        }
    }

#if defined(DEBUG_TILE_TRACE)
    if (has_damage) {
        auto rects = damage.rectangles();
        for (auto const& rect : rects) {
            std::cout << "damage rect: " << rect.min_x << ',' << rect.min_y
                      << ' ' << rect.max_x << ',' << rect.max_y << std::endl;
        }
    }
#endif

    ProgressiveSurfaceBuffer* progressive_buffer = nullptr;
    std::vector<std::size_t> progressive_dirty_tiles;
    std::vector<std::uint8_t> local_frame_bytes;
    std::span<std::uint8_t> staging;
    std::span<std::uint8_t const> frame_pixels;

    if (!metal_active) {
        if (has_damage) {
            if (has_buffered) {
                staging = surface.staging_span();
                if (staging.size() < surface.frame_bytes()) {
                    surface.discard_staging();
                    has_buffered = false;
                }
            }
            if (has_buffered) {
                frame_pixels = std::span<std::uint8_t const>{staging.data(), staging.size()};
            } else {
                local_frame_bytes.resize(surface.frame_bytes());
                staging = std::span<std::uint8_t>{local_frame_bytes.data(), local_frame_bytes.size()};
                frame_pixels = std::span<std::uint8_t const>{local_frame_bytes.data(), local_frame_bytes.size()};
            }
        }

        if (has_damage) {
            clear_linear_buffer_for_damage(linear_buffer, damage, clear_linear, width, height);
        }

        if (has_progressive) {
            if (has_damage) {
                auto desired_tile = choose_progressive_tile_size(width, height, damage, full_repaint, surface);
                surface.ensure_progressive_tile_size(desired_tile);
            }
            progressive_buffer = &surface.progressive_buffer();
            if (has_damage) {
                damage.collect_progressive_tiles(*progressive_buffer, progressive_dirty_tiles);
            }
        }
    }
    if (progressive_buffer && collect_damage_metrics) {
        progressive_tiles_total = static_cast<std::uint64_t>(progressive_buffer->tile_count());
        progressive_tiles_dirty = static_cast<std::uint64_t>(progressive_dirty_tiles.size());
        if (progressive_tiles_total > progressive_tiles_dirty) {
            progressive_tiles_skipped = progressive_tiles_total - progressive_tiles_dirty;
        } else {
            progressive_tiles_skipped = 0;
        }
    }

    auto const stride = static_cast<std::size_t>(surface.row_stride_bytes());
    bool const is_bgra = (desc.pixel_format == Runtime::PixelFormat::BGRA8Unorm
                          || desc.pixel_format == Runtime::PixelFormat::BGRA8Unorm_sRGB);

    auto payload_offsets = compute_command_payload_offsets(bucket->command_kinds,
                                                           bucket->command_payload);
    if (!payload_offsets) {
        (void)set_last_error(space_, params.target_path,
                             payload_offsets.error().message.value_or("failed to prepare command payload"));
        return std::unexpected(payload_offsets.error());
    }

    std::uint64_t drawn_total = 0;
    std::uint64_t drawn_opaque = 0;
    std::uint64_t drawn_alpha = 0;
    std::uint64_t culled_drawables = 0;
    std::uint64_t executed_commands = 0;
    std::uint64_t unsupported_commands = 0;
    std::uint64_t opaque_sort_violations = 0;
    std::uint64_t alpha_sort_violations = 0;
    bool has_focus_pulse = false;
    std::optional<SP::UI::Runtime::DirtyRectHint> focus_dirty;
    double approx_area_total = 0.0;
    double approx_area_opaque = 0.0;
    double approx_area_alpha = 0.0;
    std::uint64_t progressive_tiles_updated = 0;
    std::uint64_t progressive_bytes_copied = 0;
    std::size_t progressive_workers_used = 0;
    std::size_t progressive_tile_size_px = 0;
    std::size_t progressive_jobs = 0;
    std::size_t encode_workers_used = 0;
    std::size_t encode_jobs_used = 0;
    auto const [text_pipeline_mode, text_fallback_allowed] = determine_text_pipeline(params.settings);
    std::uint64_t text_command_count = 0;
    std::uint64_t text_fallback_count = 0;

    auto image_asset_prefix = revisionBase + "/assets/images/";

    auto process_drawable = [&](std::uint32_t drawable_index,
                                bool alpha_pass) -> SP::Expected<void> {
        if (drawable_index >= drawable_count) {
            return std::unexpected(make_error("drawable index out of range",
                                              SP::Error::Code::InvalidType));
        }

        if (drawable_index < bucket->visibility.size()
            && bucket->visibility[drawable_index] == 0) {
            ++culled_drawables;
            return {};
        }

        if (!detail::bounding_box_intersects(*bucket, drawable_index, width, height)
            || !detail::bounding_sphere_intersects(*bucket, drawable_index, width, height)) {
            ++culled_drawables;
            return {};
        }

        if (drawable_index >= bucket->command_offsets.size()
            || drawable_index >= bucket->command_counts.size()) {
            return std::unexpected(make_error("command buffer metadata missing",
                                              SP::Error::Code::InvalidType));
        }

        auto command_offset = bucket->command_offsets[drawable_index];
        auto command_count = bucket->command_counts[drawable_index];
        if (static_cast<std::size_t>(command_offset) + command_count
            > bucket->command_kinds.size()) {
            return std::unexpected(make_error("command buffer index out of range",
                                              SP::Error::Code::InvalidType));
        }

        auto material_id = std::uint32_t{0};
        if (drawable_index < bucket->material_ids.size()) {
            material_id = bucket->material_ids[drawable_index];
        }
        auto pipeline_flags = pipeline_flags_for(*bucket, drawable_index);
        bool highlight_pulse = (pipeline_flags & PipelineFlags::HighlightPulse) != 0u;
        if (highlight_pulse) {
            has_focus_pulse = true;
            if (drawable_index < bounds_by_index.size()) {
                auto const& maybe_bounds = bounds_by_index[drawable_index];
                if (maybe_bounds && !maybe_bounds->empty()) {
                    SP::UI::Runtime::DirtyRectHint bounds_hint{};
                    bounds_hint.min_x = static_cast<float>(maybe_bounds->min_x);
                    bounds_hint.min_y = static_cast<float>(maybe_bounds->min_y);
                    bounds_hint.max_x = static_cast<float>(maybe_bounds->max_x);
                    bounds_hint.max_y = static_cast<float>(maybe_bounds->max_y);
                    if (focus_dirty.has_value()) {
                        focus_dirty->min_x = std::min(focus_dirty->min_x, bounds_hint.min_x);
                        focus_dirty->min_y = std::min(focus_dirty->min_y, bounds_hint.min_y);
                        focus_dirty->max_x = std::max(focus_dirty->max_x, bounds_hint.max_x);
                        focus_dirty->max_y = std::max(focus_dirty->max_y, bounds_hint.max_y);
                    } else {
                        focus_dirty = bounds_hint;
                    }
                }
            }
        }
        MaterialDescriptor* material_desc = nullptr;
        {
            auto [it, inserted] = material_descriptors.try_emplace(material_id, MaterialDescriptor{});
            if (inserted) {
                it->second.material_id = material_id;
            }
            it->second.pipeline_flags |= pipeline_flags;
            material_desc = &it->second;
        }

        bool drawable_drawn = false;
        bool fallback_attempted = false;

        bool skip_draw = false;
        if (has_damage) {
            auto const& bounds_opt = bounds_by_index[drawable_index];
            if (bounds_opt && !damage.intersects(*bounds_opt)) {
                skip_draw = true;
            }
        }

        auto record_material_kind = [&](Scene::DrawCommandKind kind) {
            if (!material_desc) {
                return;
            }
            if (material_desc->command_count == 0) {
                material_desc->primary_draw_kind = static_cast<std::uint32_t>(kind);
            }
            material_desc->command_count += 1;
        };

        if (!skip_draw) {
            if (command_count == 0) {
#if defined(__APPLE__) && PATHSPACE_UI_METAL
                if (metal_active && metal_backend) {
                    auto const& maybe_bounds = bounds_by_index[drawable_index];
                    if (maybe_bounds.has_value() && !maybe_bounds->empty()) {
                        PathRenderer2DMetal::Rect gpu_rect{
                            .min_x = static_cast<float>(maybe_bounds->min_x),
                            .min_y = static_cast<float>(maybe_bounds->min_y),
                            .max_x = static_cast<float>(maybe_bounds->max_x),
                            .max_y = static_cast<float>(maybe_bounds->max_y),
                        };
                        auto fallback_color = make_linear_color(color_from_drawable(bucket->drawable_ids[drawable_index]));
                        if (metal_backend->draw_rect(gpu_rect, to_array(fallback_color))) {
                            drawable_drawn = true;
                            fallback_attempted = true;
                        }
                    }
                }
#endif
                if (!drawable_drawn) {
                    drawable_drawn = draw_fallback_bounds_box(*bucket,
                                                              drawable_index,
                                                              linear_buffer,
                                                              width,
                                                              height);
                    fallback_attempted = true;
                    if (!drawable_drawn) {
                        ++culled_drawables;
                        return {};
                    }
                }
            } else {
                for (std::uint32_t cmd = 0; cmd < command_count; ++cmd) {
                    auto command_index = static_cast<std::size_t>(command_offset) + cmd;
                    auto kind = static_cast<Scene::DrawCommandKind>(bucket->command_kinds[command_index]);
                    auto payload_offset = (*payload_offsets)[command_index];
                    auto payload_size = Scene::payload_size_bytes(kind);
                    if (payload_offset + payload_size > bucket->command_payload.size()) {
                        return std::unexpected(make_error("command payload exceeds buffer",
                                                          SP::Error::Code::InvalidType));
                    }

                    switch (kind) {
                    case Scene::DrawCommandKind::Rect: {
                        auto rect = read_struct<Scene::RectCommand>(bucket->command_payload,
                                                                    payload_offset);
                        if (highlight_pulse) {
                            rect.color = pulse_focus_highlight_color(rect.color,
                                                                      params.settings.time.time_ms);
                        }
                        record_material_kind(Scene::DrawCommandKind::Rect);
                        if (material_desc) {
                            material_desc->color_rgba = rect.color;
                            material_desc->uses_image = false;
                        }
                        auto rect_linear = make_linear_color(rect.color);
                        bool handled = false;
#if defined(__APPLE__) && PATHSPACE_UI_METAL
                        if (metal_active && metal_backend) {
                            bool material_bound = true;
                            if (material_desc) {
                                material_bound = metal_backend->bind_material(*material_desc);
                            }
                            if (material_bound) {
                                PathRenderer2DMetal::Rect gpu_rect{
                                    .min_x = rect.min_x,
                                    .min_y = rect.min_y,
                                    .max_x = rect.max_x,
                                    .max_y = rect.max_y,
                                };
                                if (metal_backend->draw_rect(gpu_rect, to_array(rect_linear))) {
                                    drawable_drawn = true;
                                    handled = true;
                                }
                            }
                        }
#endif
                        if (!handled) {
                            auto clip_rects = (full_repaint || !has_damage)
                                                  ? std::span<PathRenderer2DInternal::DamageRect const>{}
                                                  : damage.rectangles();
                            if (draw_rect_command(rect, linear_buffer, width, height, clip_rects)) {
                                drawable_drawn = true;
                                handled = true;
                            }
                        }
                        if (handled) {
                            ++executed_commands;
                        }
                        break;
                    }
                    case Scene::DrawCommandKind::RoundedRect: {
                        auto rounded = read_struct<Scene::RoundedRectCommand>(bucket->command_payload,
                                                                              payload_offset);
                        record_material_kind(Scene::DrawCommandKind::RoundedRect);
                        if (material_desc) {
                            material_desc->color_rgba = rounded.color;
                            material_desc->uses_image = false;
                        }
                        auto rounded_linear = make_linear_color(rounded.color);
                        bool handled = false;
#if defined(__APPLE__) && PATHSPACE_UI_METAL
                        if (metal_active && metal_backend) {
                            bool material_bound = true;
                            if (material_desc) {
                                material_bound = metal_backend->bind_material(*material_desc);
                            }
                            if (material_bound
                                && metal_backend->draw_rounded_rect(rounded, to_array(rounded_linear))) {
                                drawable_drawn = true;
                                handled = true;
                            }
                        }
#endif
                        if (!handled) {
                            if (draw_rounded_rect_command(rounded, linear_buffer, width, height)) {
                                drawable_drawn = true;
                                handled = true;
                            }
                        }
                        if (handled) {
                            ++executed_commands;
                        }
                        break;
                    }
                    case Scene::DrawCommandKind::TextGlyphs: {
                        auto glyphs = read_struct<Scene::TextGlyphsCommand>(bucket->command_payload,
                                                                            payload_offset);
                        ++text_command_count;
                        record_material_kind(Scene::DrawCommandKind::TextGlyphs);
                        if (material_desc) {
                            material_desc->color_rgba = glyphs.color;
                            material_desc->uses_image = false;
                        }
                        auto glyph_linear = make_linear_color(glyphs.color);
                        auto glyph_straight = make_linear_straight(glyphs.color);
                        bool handled = false;
                        auto draw_glyph_quads = [&](bool count_fallback) -> bool {
                            bool drawn = false;
#if defined(__APPLE__) && PATHSPACE_UI_METAL
                            if (metal_active && metal_backend) {
                                bool material_bound = true;
                                if (material_desc) {
                                    material_bound = metal_backend->bind_material(*material_desc);
                                }
                                if (material_bound
                                    && metal_backend->draw_text_quad(glyphs, to_array(glyph_linear))) {
                                    drawable_drawn = true;
                                    drawn = true;
                                }
                            }
#endif
                            if (!drawn) {
                                if (draw_text_glyphs_command(glyphs, linear_buffer, width, height)) {
                                    drawable_drawn = true;
                                    drawn = true;
                                }
                            }
                            if (drawn && count_fallback) {
                                ++text_fallback_count;
                            }
                            return drawn;
                        };

                        auto shaped_requested = (text_pipeline_mode == TextPipeline::Shaped);
                        if (shaped_requested) {
                            auto atlas_it = font_atlases.find(glyphs.atlas_fingerprint);
                            if (atlas_it != font_atlases.end()
                                && draw_shaped_text_command(glyphs,
                                                            *bucket,
                                                            atlas_it->second,
                                                            glyph_linear,
                                                            glyph_straight,
                                                            linear_buffer,
                                                            width,
                                                            height)) {
                                drawable_drawn = true;
                                handled = true;
                            } else if (text_fallback_allowed) {
                                handled = draw_glyph_quads(true);
                            }
                        } else {
                            handled = draw_glyph_quads(false);
                        }

                        if (handled) {
                            ++executed_commands;
                        } else {
                            ++unsupported_commands;
                        }
                        break;
                    }
                    case Scene::DrawCommandKind::Path: {
                        auto path_cmd = read_struct<Scene::PathCommand>(bucket->command_payload,
                                                                        payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Path);
                        if (material_desc) {
                            material_desc->color_rgba = path_cmd.fill_color;
                            material_desc->uses_image = false;
                        }
                        if (draw_path_command(path_cmd, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Mesh: {
                        auto mesh_cmd = read_struct<Scene::MeshCommand>(bucket->command_payload,
                                                                        payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Mesh);
                        if (material_desc) {
                            material_desc->color_rgba = mesh_cmd.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_mesh_command(mesh_cmd, *bucket, drawable_index, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Stroke: {
                        auto stroke_cmd = read_struct<Scene::StrokeCommand>(bucket->command_payload,
                                                                            payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Stroke);
                        if (material_desc) {
                            material_desc->color_rgba = stroke_cmd.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_stroke_command(stroke_cmd, *bucket, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Image: {
                        auto image_cmd = read_struct<Scene::ImageCommand>(bucket->command_payload,
                                                                         payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Image);
                        if (material_desc) {
                            material_desc->uses_image = true;
                            material_desc->resource_fingerprint = image_cmd.image_fingerprint;
                            material_desc->tint_rgba = image_cmd.tint;
                        }
                        auto tint_straight = make_linear_straight(image_cmd.tint);
                        auto asset_path = image_asset_prefix + fingerprint_to_hex(image_cmd.image_fingerprint) + ".png";
                        auto texture = image_cache_.load(space_, asset_path, image_cmd.image_fingerprint);
                        if (!texture) {
                            auto const error = texture.error();
                            if (error.code != SP::Error::Code::NoObjectFound
                                && error.code != SP::Error::Code::NoSuchPath) {
                                auto message = error.message.value_or("failed to load image asset");
                                (void)set_last_error(space_, params.target_path, message);
                            }
                            ++unsupported_commands;
                            break;
                        }
                        auto const& image_data = *texture;
                        bool handled = false;
#if defined(__APPLE__) && PATHSPACE_UI_METAL
                        if (metal_active && metal_backend) {
                            bool material_bound = true;
                            if (material_desc) {
                                material_bound = metal_backend->bind_material(*material_desc);
                            }
                            if (material_bound
                                && metal_backend->draw_image(image_cmd,
                                                              image_data->width,
                                                              image_data->height,
                                                              image_data->pixels.data(),
                                                              image_data->pixels.size(),
                                                              to_array(tint_straight))) {
                                drawable_drawn = true;
                                handled = true;
                            }
                        }
#endif
                        if (!handled) {
                            if (draw_image_command(image_cmd,
                                                   *image_data,
                                                   tint_straight,
                                                   linear_buffer,
                                                   width,
                                                   height)) {
                                drawable_drawn = true;
                                handled = true;
                            }
                        }
                        auto& residency = resource_residency[image_cmd.image_fingerprint];
                        residency.fingerprint = image_cmd.image_fingerprint;
                        residency.uses_image = true;
                        residency.width = image_data->width;
                        residency.height = image_data->height;
                        residency.cpu_bytes = static_cast<std::uint64_t>(image_data->pixels.size()) * sizeof(float);
                        residency.gpu_bytes = static_cast<std::uint64_t>(image_data->width)
                                              * static_cast<std::uint64_t>(image_data->height)
                                              * 4u;
                        if (handled) {
                            ++executed_commands;
                        } else {
                            ++unsupported_commands;
                        }
                        break;
                    }
                    default:
                        ++unsupported_commands;
                        break;
                    }
                }
            }
        } else {
            drawable_drawn = true;
        }

        if (!drawable_drawn) {
            if (!fallback_attempted) {
                drawable_drawn = draw_fallback_bounds_box(*bucket,
                                                          drawable_index,
                                                          linear_buffer,
                                                          width,
                                                          height);
                fallback_attempted = true;
            }
            if (!drawable_drawn) {
                ++culled_drawables;
                return {};
            }
        }

        ++drawn_total;
        if (alpha_pass) {
            ++drawn_alpha;
        } else {
            ++drawn_opaque;
        }
        auto area = approximate_drawable_area(*bucket, drawable_index);
        approx_area_total += area;
        if (alpha_pass) {
            approx_area_alpha += area;
        } else {
            approx_area_opaque += area;
        }
        if (drawable_drawn && material_desc) {
            material_desc->drawable_count += 1;
        }
        return {};
    };

    std::vector<std::uint32_t> fallback_opaque;
    std::vector<std::uint32_t> fallback_alpha;
    if (bucket->opaque_indices.empty() && bucket->alpha_indices.empty()) {
        fallback_opaque.reserve(drawable_count);
        fallback_alpha.reserve(drawable_count);
        for (std::uint32_t i = 0; i < drawable_count; ++i) {
            auto flags = pipeline_flags_for(*bucket, i);
            if (PipelineFlags::is_alpha_pass(flags)) {
                fallback_alpha.push_back(i);
            } else {
                fallback_opaque.push_back(i);
            }
        }
        if (fallback_opaque.empty() && fallback_alpha.empty()) {
            fallback_opaque.resize(drawable_count);
            std::iota(fallback_opaque.begin(), fallback_opaque.end(), 0u);
        }
    }

    auto process_pass = [&](std::vector<std::uint32_t> const& indices,
                            bool alpha_pass,
                            bool explicit_order) -> SP::Expected<void> {
        bool have_prev = false;
        std::uint32_t prev_layer = 0;
        std::uint32_t prev_material = 0;
        std::uint32_t prev_flags = 0;
        float prev_z = 0.0f;

        for (auto drawable_index : indices) {
            if (auto status = process_drawable(drawable_index, alpha_pass); !status) {
                return status;
            }
            if (!explicit_order) {
                continue;
            }
            if (alpha_pass) {
                if (drawable_index >= bucket->layers.size()
                    || drawable_index >= bucket->z_values.size()) {
                    have_prev = false;
                    continue;
                }
                auto layer = bucket->layers[drawable_index];
                auto z = bucket->z_values[drawable_index];
                if (have_prev) {
                    if (layer < prev_layer
                        || (layer == prev_layer && z > prev_z + kSortEpsilon)) {
                        ++alpha_sort_violations;
                    }
                }
                prev_layer = layer;
                prev_z = z;
                have_prev = true;
            } else {
                if (drawable_index >= bucket->layers.size()
                    || drawable_index >= bucket->material_ids.size()
                    || drawable_index >= bucket->z_values.size()) {
                    have_prev = false;
                    continue;
                }
                auto layer = bucket->layers[drawable_index];
                auto material = bucket->material_ids[drawable_index];
                auto flags = pipeline_flags_for(*bucket, drawable_index);
                auto z = bucket->z_values[drawable_index];
                if (have_prev) {
                    bool violation = false;
                    if (layer < prev_layer) {
                        violation = true;
                    } else if (layer == prev_layer) {
                        if (material < prev_material) {
                            violation = true;
                        } else if (material == prev_material) {
                            if (flags < prev_flags) {
                                violation = true;
                            } else if (flags == prev_flags && z < prev_z - kSortEpsilon) {
                                violation = true;
                            }
                        }
                    }
                    if (violation) {
                        ++opaque_sort_violations;
                    }
                }
                prev_layer = layer;
                prev_material = material;
                prev_flags = flags;
                prev_z = z;
                have_prev = true;
            }
        }
        return {};
    };

    if (has_damage) {
        if (!bucket->opaque_indices.empty()) {
            if (auto status = process_pass(bucket->opaque_indices, false, true); !status) {
                (void)set_last_error(space_, params.target_path,
                                     status.error().message.value_or("failed to store present metrics"));
                return std::unexpected(status.error());
            }
        } else if (!fallback_opaque.empty()) {
            if (auto status = process_pass(fallback_opaque, false, false); !status) {
                (void)set_last_error(space_, params.target_path,
                                     status.error().message.value_or("failed to store present metrics"));
                return std::unexpected(status.error());
            }
        }

        if (!bucket->alpha_indices.empty()) {
            if (auto status = process_pass(bucket->alpha_indices, true, true); !status) {
                (void)set_last_error(space_, params.target_path,
                                     status.error().message.value_or("failed to store present metrics"));
                return std::unexpected(status.error());
            }
        } else if (!fallback_alpha.empty()) {
            if (auto status = process_pass(fallback_alpha, true, false); !status) {
                (void)set_last_error(space_, params.target_path,
                                     status.error().message.value_or("failed to store present metrics"));
                return std::unexpected(status.error());
            }
        }
    }

auto const encode_srgb = needs_srgb_encode(desc);
    EncodeRunStats encode_stats{};
    if (!metal_active && has_damage) {
        auto const encode_start = std::chrono::steady_clock::now();
        auto encode_jobs = build_encode_jobs(
            damage,
            progressive_buffer,
            std::span<std::size_t const>{progressive_dirty_tiles.data(), progressive_dirty_tiles.size()},
            width,
            height);

        EncodeContext encode_ctx{
            .staging = staging.data(),
            .row_stride_bytes = stride,
            .linear = linear_buffer.data(),
            .width = width,
            .height = height,
            .desc = &desc,
            .encode_srgb = encode_srgb,
            .is_bgra = is_bgra,
        };

        if (!encode_jobs.empty()) {
            try {
                encode_stats = run_encode_jobs(std::span<EncodeJob const>{encode_jobs.data(), encode_jobs.size()},
                                               encode_ctx);
            } catch (std::exception const& ex) {
                auto message = std::string{"failed to encode surface pixels: "} + ex.what();
                (void)set_last_error(space_, params.target_path, message);
                return std::unexpected(make_error(std::move(message), SP::Error::Code::UnknownError));
            }
        }
        encode_jobs_used = encode_stats.jobs;
        encode_workers_used = encode_stats.workers_used;
        encode_stall_ms_total = encode_stats.stall_ms_total;
        encode_stall_ms_max = encode_stats.stall_ms_max;
        encode_stall_workers = encode_stats.stall_workers;

        auto const encode_end = std::chrono::steady_clock::now();
        encode_ms = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
    }
    if (!has_damage) {
        encode_jobs_used = encode_stats.jobs;
        encode_workers_used = encode_stats.workers_used;
        encode_stall_ms_total = encode_stats.stall_ms_total;
        encode_stall_ms_max = encode_stats.stall_ms_max;
        encode_stall_workers = encode_stats.stall_workers;
    }

    if (!metal_active && has_progressive && has_damage) {
        auto const progressive_start = std::chrono::steady_clock::now();
        auto const row_stride_bytes = surface.row_stride_bytes();
        auto staging_const = std::span<std::uint8_t const>{staging.data(), staging.size()};
        bool const prefer_parallel =
            progressive_buffer->tile_size() > 4
            && progressive_dirty_tiles.size() >= 16;
        progressive_tile_size_px = progressive_buffer->tile_size();
        progressive_jobs = progressive_dirty_tiles.size();

        if (prefer_parallel) {
            ProgressiveTileCopyContext ctx{
                .surface = surface,
                .buffer = *progressive_buffer,
                .staging = staging_const,
                .row_stride_bytes = row_stride_bytes,
                .revision = sceneRevision->revision,
            };
            try {
                auto stats_copy = copy_progressive_tiles(progressive_dirty_tiles, ctx);
                progressive_tiles_updated += stats_copy.tiles_updated;
                progressive_bytes_copied += stats_copy.bytes_copied;
                progressive_workers_used = stats_copy.workers_used;
            } catch (std::exception const& ex) {
                auto message = std::string{"failed to update progressive tiles: "} + ex.what();
                (void)set_last_error(space_, params.target_path, message);
                return std::unexpected(make_error(std::move(message), SP::Error::Code::UnknownError));
            }
            for (auto tile_index : progressive_dirty_tiles) {
                surface.mark_progressive_dirty(tile_index);
            }
        } else {
            progressive_workers_used = progressive_dirty_tiles.empty() ? 0 : 1;
            for (auto tile_index : progressive_dirty_tiles) {
                auto dims = progressive_buffer->tile_dimensions(tile_index);
                if (dims.width <= 0 || dims.height <= 0) {
                    continue;
                }
                auto writer = surface.begin_progressive_tile(tile_index, TilePass::OpaqueInProgress);
                auto tile_pixels = writer.pixels();
                auto const row_pitch = static_cast<std::size_t>(dims.width) * 4u;
                auto const tile_rows = std::max(dims.height, 0);
                for (int row = 0; row < tile_rows; ++row) {
                    auto const src_offset = (static_cast<std::size_t>(dims.y + row) * row_stride_bytes)
                                            + static_cast<std::size_t>(dims.x) * 4u;
                    auto const dst_offset = static_cast<std::size_t>(row) * tile_pixels.stride_bytes;
                    std::memcpy(tile_pixels.data + dst_offset,
                                staging_const.data() + src_offset,
                                row_pitch);
                }
                writer.commit(TilePass::AlphaDone, sceneRevision->revision);
                surface.mark_progressive_dirty(tile_index);
                ++progressive_tiles_updated;
                progressive_bytes_copied += row_pitch * static_cast<std::uint64_t>(tile_rows);
            }
        }
        auto const progressive_end = std::chrono::steady_clock::now();
        progressive_copy_ms = std::chrono::duration<double, std::milli>(progressive_end - progressive_start).count();
    }
    if (progressive_buffer && progressive_tile_size_px == 0) {
        progressive_tile_size_px = progressive_buffer->tile_size();
    }
    if (progressive_jobs == 0) {
        progressive_jobs = progressive_dirty_tiles.size();
    }
    if (progressive_jobs == 0) {
        progressive_workers_used = 0;
    }

    auto const end = std::chrono::steady_clock::now();
    auto render_ms = std::chrono::duration<double, std::milli>(end - start).count();

    PathSurfaceSoftware::FrameInfo const frame_info{
        .frame_index = params.settings.time.frame_index,
        .revision = sceneRevision->revision,
        .render_ms = render_ms,
    };
    auto const publish_start = std::chrono::steady_clock::now();
    if (!metal_active) {
        if (has_buffered && has_damage) {
            surface.publish_buffered_frame(frame_info);
        } else {
            surface.record_frame_info(frame_info);
        }
    } else {
        surface.record_frame_info(frame_info);
    }
    auto const publish_end = std::chrono::steady_clock::now();
    publish_ms = std::chrono::duration<double, std::milli>(publish_end - publish_start).count();

    double approx_surface_pixels = static_cast<double>(pixel_count);
    if (!has_damage && approx_area_total <= 0.0 && state.last_approx_area_total > 0.0) {
        approx_area_total = state.last_approx_area_total;
        approx_area_opaque = state.last_approx_area_opaque;
        approx_area_alpha = state.last_approx_area_alpha;
    }
    double approx_overdraw_factor = 0.0;
    if (approx_surface_pixels > 0.0) {
        approx_overdraw_factor = approx_area_total / approx_surface_pixels;
    }

    auto& material_list = state.material_list;
    material_list.clear();
    material_list.reserve(material_descriptors.size());
    for (auto const& entry : material_descriptors) {
        material_list.push_back(entry.second);
    }
    std::sort(material_list.begin(),
              material_list.end(),
              [](MaterialDescriptor const& lhs, MaterialDescriptor const& rhs) {
                  return lhs.material_id < rhs.material_id;
              });

    std::vector<MaterialResourceResidency> resource_list;
    resource_list.reserve(resource_residency.size());
    for (auto const& entry : resource_residency) {
        resource_list.push_back(entry.second);
    }
    std::sort(resource_list.begin(),
              resource_list.end(),
              [](MaterialResourceResidency const& lhs, MaterialResourceResidency const& rhs) {
                  return lhs.fingerprint < rhs.fingerprint;
              });
    std::uint64_t total_texture_gpu_bytes = 0;
    for (auto const& info : resource_list) {
        total_texture_gpu_bytes += info.gpu_bytes;
    }
    std::uint64_t total_gpu_bytes = total_texture_gpu_bytes;
    bool render_error_recorded = false;

#if PATHSPACE_UI_METAL
    if (params.backend_kind == SP::UI::Runtime::RendererKind::Metal2D && params.metal_surface != nullptr) {
        params.metal_surface->update_material_descriptors(material_list);
        params.metal_surface->update_resource_residency(resource_list);
        bool metal_updated = false;
        std::string metal_error;
#if defined(__APPLE__)
        if (metal_active && metal_backend) {
            metal_updated = metal_backend->finish(*params.metal_surface,
                                                  frame_info.frame_index,
                                                  frame_info.revision);
            if (!metal_updated) {
                metal_error = "failed to encode Metal frame";
            }
        } else {
            if (has_buffered && has_damage) {
                if (auto iosurface_opt = surface.front_iosurface()) {
                    auto iosurface_ref = iosurface_opt->retain_for_external_use();
                    if (iosurface_ref != nullptr) {
                        metal_updated = params.metal_surface->blit_from_iosurface(iosurface_ref,
                                                                                  frame_info.frame_index,
                                                                                  frame_info.revision,
                                                                                  &metal_error);
                        CFRelease(iosurface_ref);
                    }
                }
            }
            if (!metal_updated && has_damage && !frame_pixels.empty()) {
                params.metal_surface->update_from_rgba8(frame_pixels,
                                                        static_cast<std::size_t>(surface.row_stride_bytes()),
                                                        frame_info.frame_index,
                                                        frame_info.revision);
                metal_updated = true;
                metal_error.clear();
            }
            params.metal_surface->present_completed(frame_info.frame_index, frame_info.revision);
        }
#endif
        auto const total_gpu = static_cast<std::uint64_t>(params.metal_surface->resident_gpu_bytes());
        total_gpu_bytes = total_gpu;
        if (total_gpu >= total_texture_gpu_bytes) {
            target_texture_bytes = total_gpu - total_texture_gpu_bytes;
        } else {
            target_texture_bytes = total_gpu;
        }
        if (!metal_updated && !metal_error.empty()) {
            (void)set_last_error(space_,
                                 params.target_path,
                                 metal_error,
                                 frame_info.revision,
                                 Runtime::Diagnostics::PathSpaceError::Severity::Recoverable,
                                 3201);
            render_error_recorded = true;
        }
    }
#endif

    auto metricsBase = std::string(params.target_path.getPath()) + "/output/v1/common";
    if (auto status = replace_single<std::uint64_t>(space_, metricsBase + "/frameIndex", params.settings.time.frame_index); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store frame index");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::uint64_t>(space_, metricsBase + "/revision", sceneRevision->revision); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store revision");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<double>(space_, metricsBase + "/renderMs", render_ms); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store render duration");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space_, metricsBase + "/lastError", std::string{}); !status) {
        return std::unexpected(status.error());
    }
    (void)replace_single<double>(space_, metricsBase + "/damageMs", damage_ms);
    (void)replace_single<double>(space_, metricsBase + "/encodeMs", encode_ms);
    (void)replace_single<double>(space_, metricsBase + "/progressiveCopyMs", progressive_copy_ms);
    (void)replace_single<double>(space_, metricsBase + "/publishMs", publish_ms);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/drawableCount", static_cast<std::uint64_t>(drawable_count));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/opaqueDrawables", drawn_opaque);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/alphaDrawables", drawn_alpha);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/culledDrawables", culled_drawables);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandCount", static_cast<std::uint64_t>(bucket->command_kinds.size()));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandsExecuted", executed_commands);

    if (!render_error_recorded) {
        if (auto status = set_last_error(space_, params.target_path, "", sceneRevision->revision, Runtime::Diagnostics::PathSpaceError::Severity::Info); !status) {
            return std::unexpected(status.error());
        }
    }
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/unsupportedCommands", unsupported_commands);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/opaqueSortViolations", opaque_sort_violations);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/alphaSortViolations", alpha_sort_violations);
    (void)replace_single<double>(space_, metricsBase + "/approxOpaquePixels", approx_area_opaque);
    (void)replace_single<double>(space_, metricsBase + "/approxAlphaPixels", approx_area_alpha);
    (void)replace_single<double>(space_, metricsBase + "/approxDrawablePixels", approx_area_total);
    (void)replace_single<double>(space_, metricsBase + "/approxOverdrawFactor", approx_overdraw_factor);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesUpdated", progressive_tiles_updated);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveBytesCopied", progressive_bytes_copied);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTileSize", static_cast<std::uint64_t>(progressive_tile_size_px));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveWorkersUsed", static_cast<std::uint64_t>(progressive_workers_used));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveJobs", static_cast<std::uint64_t>(progressive_jobs));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/encodeWorkersUsed", static_cast<std::uint64_t>(encode_workers_used));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/encodeJobs", static_cast<std::uint64_t>(encode_jobs_used));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/materialCount",
                                        static_cast<std::uint64_t>(material_list.size()));
    (void)replace_single(space_, metricsBase + "/materialDescriptors", material_list);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/materialResourceCount",
                                        static_cast<std::uint64_t>(resource_list.size()));
    (void)replace_single(space_, metricsBase + "/materialResources", resource_list);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/textCommandCount", text_command_count);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/textFallbackCount", text_fallback_count);
    (void)replace_single<std::string>(space_,
                                      metricsBase + "/textPipeline",
                                      (text_pipeline_mode == TextPipeline::Shaped) ? std::string{"Shaped"}
                                                                                    : std::string{"GlyphQuads"});
    (void)replace_single<bool>(space_, metricsBase + "/textFallbackAllowed", text_fallback_allowed);
    (void)replace_single(space_, metricsBase + "/damageTiles", damage_tile_hints);
    if (collect_damage_metrics) {
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/damageRectangles", damage_rect_count);
        (void)replace_single<double>(space_, metricsBase + "/damageCoverageRatio", damage_coverage_ratio);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintMatchesExact", fingerprint_matches_exact);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintMatchesRemap", fingerprint_matches_remap);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintChanges", fingerprint_changed);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintNew", fingerprint_new);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintRemoved", fingerprint_removed);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesDirty", progressive_tiles_dirty);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesTotal", progressive_tiles_total);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesSkipped", progressive_tiles_skipped);
    }

    state.drawable_states = std::move(current_states);
    state.clear_color = current_clear;
    state.desc = desc;
    state.last_revision = sceneRevision->revision;

    if (has_focus_pulse) {
        schedule_focus_pulse_render(space_,
                                    params.target_path,
                                    params.settings,
                                    focus_dirty,
                                    params.settings.time.frame_index);
    }

    RenderStats stats{};
    stats.frame_index = params.settings.time.frame_index;
    stats.revision = sceneRevision->revision;
    stats.render_ms = render_ms;
    stats.drawable_count = drawn_total;
    stats.damage_ms = damage_ms;
    stats.encode_ms = encode_ms;
    stats.progressive_copy_ms = progressive_copy_ms;
    stats.publish_ms = publish_ms;
    stats.progressive_tiles_updated = progressive_tiles_updated;
    stats.progressive_bytes_copied = progressive_bytes_copied;
    stats.progressive_tile_size = static_cast<std::uint64_t>(progressive_tile_size_px);
    stats.progressive_workers_used = static_cast<std::uint64_t>(progressive_workers_used);
    stats.progressive_jobs = static_cast<std::uint64_t>(progressive_jobs);
    stats.encode_workers_used = static_cast<std::uint64_t>(encode_workers_used);
    stats.encode_jobs = static_cast<std::uint64_t>(encode_jobs_used);
    stats.encode_worker_stall_ms_total = encode_stall_ms_total;
    stats.encode_worker_stall_ms_max = encode_stall_ms_max;
    stats.encode_worker_stall_workers = static_cast<std::uint64_t>(encode_stall_workers);
    if (collect_damage_metrics) {
        stats.progressive_tiles_dirty = progressive_tiles_dirty;
        stats.progressive_tiles_total = progressive_tiles_total;
        stats.progressive_tiles_skipped = progressive_tiles_skipped;
    }
    stats.progressive_tile_diagnostics_enabled = collect_damage_metrics;
    stats.text_command_count = text_command_count;
    stats.text_fallback_count = text_fallback_count;
    stats.text_pipeline = text_pipeline_mode;
    stats.text_fallback_allowed = text_fallback_allowed;
    stats.backend_kind = params.backend_kind;
    stats.materials = material_list;
    stats.resource_residency = std::move(resource_list);
    stats.damage_tiles = std::move(damage_tile_hints);
    auto const surface_bytes = surface.resident_cpu_bytes();
    auto const cache_bytes = image_cache_.resident_bytes() + font_atlas_cache_.resident_bytes();
    auto const reported_texture_bytes = (target_texture_bytes != 0) ? target_texture_bytes : total_texture_gpu_bytes;
    stats.texture_gpu_bytes = reported_texture_bytes;
    stats.resource_cpu_bytes = static_cast<std::uint64_t>(surface_bytes + cache_bytes);
    stats.resource_gpu_bytes = total_gpu_bytes;

    state.last_approx_area_total = approx_area_total;
    state.last_approx_area_opaque = approx_area_opaque;
    state.last_approx_area_alpha = approx_area_alpha;

    (void)replace_single<std::uint64_t>(space_, metricsBase + "/resourceCpuBytes", stats.resource_cpu_bytes);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/resourceGpuBytes", stats.resource_gpu_bytes);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/textureGpuBytes", reported_texture_bytes);

    return stats;
}

} // namespace SP::UI
