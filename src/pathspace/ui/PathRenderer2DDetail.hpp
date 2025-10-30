#pragma once

#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/ImageCache.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI {

class ProgressiveSurfaceBuffer;

namespace PathRenderer2DInternal {
class DamageRegion;
struct DamageRect;
}

namespace PathRenderer2DDetail {

auto make_error(std::string message, SP::Error::Code code) -> SP::Error;

auto damage_metrics_enabled() -> bool;

auto determine_text_pipeline(Builders::RenderSettings const& settings)
    -> std::pair<PathRenderer2D::TextPipeline, bool>;

auto format_revision(std::uint64_t revision) -> std::string;
auto fingerprint_to_hex(std::uint64_t fingerprint) -> std::string;

auto set_last_error(PathSpace& space,
                    SP::ConcretePathStringView targetPath,
                    std::string const& message,
                    std::uint64_t revision = 0,
                    Builders::Diagnostics::PathSpaceError::Severity severity = Builders::Diagnostics::PathSpaceError::Severity::Recoverable,
                    int code = 3000) -> SP::Expected<void>;

auto pipeline_flags_for(Scene::DrawableBucketSnapshot const& bucket,
                        std::size_t drawable_index) -> std::uint32_t;

auto approximate_drawable_area(Scene::DrawableBucketSnapshot const& bucket,
                               std::uint32_t index) -> double;

auto compute_drawable_bounds(Scene::DrawableBucketSnapshot const& bucket,
                             std::uint32_t index,
                             int width,
                             int height) -> std::optional<PathRenderer2D::DrawableBounds>;

auto bounds_equal(PathRenderer2D::DrawableBounds const& lhs,
                  PathRenderer2D::DrawableBounds const& rhs) -> bool;

auto color_from_drawable(std::uint64_t drawableId) -> std::array<float, 4>;

struct LinearStraightColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct LinearPremulColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

inline constexpr float kSortEpsilon = 1e-5f;

auto clamp_unit(float value) -> float;
auto to_byte(float value) -> std::uint8_t;

auto make_linear_straight(std::array<float, 4> const& rgba) -> LinearStraightColor;
auto premultiply(LinearStraightColor const& straight) -> LinearPremulColor;
auto make_linear_color(std::array<float, 4> const& rgba) -> LinearPremulColor;
auto to_array(LinearPremulColor const& color) -> std::array<float, 4>;
auto to_array(LinearStraightColor const& color) -> std::array<float, 4>;

auto needs_srgb_encode(Builders::SurfaceDesc const& desc) -> bool;
auto encode_linear_color_to_output(LinearPremulColor const& color,
                                   Builders::SurfaceDesc const& desc) -> std::array<float, 4>;

struct EncodeJob {
    int min_x = 0;
    int max_x = 0;
    int start_y = 0;
    int end_y = 0;

    [[nodiscard]] auto empty() const -> bool {
        return min_x >= max_x || start_y >= end_y;
    }
};

struct EncodeContext {
    std::uint8_t* staging = nullptr;
    std::size_t row_stride_bytes = 0;
    float const* linear = nullptr;
    int width = 0;
    int height = 0;
    Builders::SurfaceDesc const* desc = nullptr;
    bool encode_srgb = false;
    bool is_bgra = false;
};

struct EncodeRunStats {
    std::size_t workers_used = 0;
    std::size_t jobs = 0;
};

auto ensure_linear_buffer_capacity(std::vector<float>& buffer,
                                   std::size_t pixel_count) -> bool;

void clear_linear_buffer_for_damage(std::vector<float>& buffer,
                                    PathRenderer2DInternal::DamageRegion const& damage,
                                    LinearPremulColor const& clear_linear,
                                    int width,
                                    int height);

auto build_encode_jobs(PathRenderer2DInternal::DamageRegion const& damage,
                       ProgressiveSurfaceBuffer const* progressive_buffer,
                       std::span<std::size_t const> progressive_dirty_tiles,
                       int width,
                       int height) -> std::vector<EncodeJob>;

auto run_encode_jobs(std::span<EncodeJob const> jobs, EncodeContext const& ctx) -> EncodeRunStats;

auto encode_pixel(float const* linear_premul,
                  Builders::SurfaceDesc const& desc,
                  bool encode_srgb) -> std::array<std::uint8_t, 4>;

auto draw_rect_command(Scene::RectCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height,
                       std::span<PathRenderer2DInternal::DamageRect const> clip_rects = {}) -> bool;

auto draw_rounded_rect_command(Scene::RoundedRectCommand const& command,
                               std::vector<float>& buffer,
                               int width,
                               int height) -> bool;

auto draw_shaped_text_command(Scene::TextGlyphsCommand const& command,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool;

auto draw_text_glyphs_command(Scene::TextGlyphsCommand const& command,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool;

auto draw_path_command(Scene::PathCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool;

auto draw_mesh_command(Scene::MeshCommand const& command,
                       Scene::DrawableBucketSnapshot const& bucket,
                       std::size_t drawable_index,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool;

auto draw_stroke_command(Scene::StrokeCommand const& command,
                         Scene::DrawableBucketSnapshot const& bucket,
                         std::vector<float>& buffer,
                         int width,
                         int height) -> bool;

auto draw_image_command(Scene::ImageCommand const& command,
                        ImageCache::ImageData const& image,
                        LinearStraightColor const& tint,
                        std::vector<float>& buffer,
                        int width,
                        int height) -> bool;

auto draw_fallback_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                              std::size_t drawable_index,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool;

auto pulse_focus_highlight_color(std::array<float, 4> const& srgb,
                                 double time_ms) -> std::array<float, 4>;

auto schedule_focus_pulse_render(PathSpace& space,
                                 SP::ConcretePathStringView targetPath,
                                 Builders::RenderSettings const& settings,
                                 std::optional<Builders::DirtyRectHint> focus_hint,
                                 std::uint64_t frame_index) -> void;

#if defined(__APPLE__) && PATHSPACE_UI_METAL
auto metal_supports_command(Scene::DrawCommandKind kind) -> bool;
#endif

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single(PathSpace& space,
                    std::string const& path,
                    T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

template <typename T>
auto read_struct(std::vector<std::uint8_t> const& payload,
                 std::size_t offset) -> T {
    T value{};
    std::memcpy(&value, payload.data() + offset, sizeof(T));
    return value;
}

auto compute_command_payload_offsets(std::vector<std::uint32_t> const& kinds,
                                     std::vector<std::uint8_t> const& payload)
    -> SP::Expected<std::vector<std::size_t>>;

} // namespace PathRenderer2DDetail

} // namespace SP::UI
