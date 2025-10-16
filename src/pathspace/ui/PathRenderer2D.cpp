#include <pathspace/ui/PathRenderer2D.hpp>

#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace SP::UI {
namespace {

auto make_error(std::string message, SP::Error::Code code) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

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

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

auto to_byte(float value) -> std::uint8_t {
    auto clamped = clamp_unit(value);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

auto set_last_error(PathSpace& space,
                    SP::ConcretePathStringView targetPath,
                    std::string const& message) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, base, message);
}

auto color_from_drawable(std::uint64_t drawableId) -> std::array<float, 4> {
    auto r = static_cast<float>(drawableId & 0xFFu) / 255.0f;
    auto g = static_cast<float>((drawableId >> 8) & 0xFFu) / 255.0f;
    auto b = static_cast<float>((drawableId >> 16) & 0xFFu) / 255.0f;
    if (r == 0.0f && g == 0.0f && b == 0.0f) {
        r = 0.9f;
        g = 0.9f;
        b = 0.9f;
    }
    return {r, g, b, 1.0f};
}

struct LinearPremulColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

auto srgb_to_linear(float value) -> float {
    value = clamp_unit(value);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto linear_to_srgb(float value) -> float {
    value = std::max(0.0f, value);
    value = std::min(1.0f, value);
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

auto make_linear_color(std::array<float, 4> const& rgba) -> LinearPremulColor {
    auto alpha = clamp_unit(rgba[3]);
    auto r = srgb_to_linear(rgba[0]);
    auto g = srgb_to_linear(rgba[1]);
    auto b = srgb_to_linear(rgba[2]);
    return LinearPremulColor{
        .r = clamp_unit(r) * alpha,
        .g = clamp_unit(g) * alpha,
        .b = clamp_unit(b) * alpha,
        .a = alpha,
    };
}

auto needs_srgb_encode(Builders::SurfaceDesc const& desc) -> bool {
    switch (desc.pixel_format) {
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        return true;
    default:
        break;
    }
    return desc.color_space == Builders::ColorSpace::sRGB;
}

void blend_pixel(float* dest, LinearPremulColor const& src) {
    auto const inv_alpha = 1.0f - src.a;
    dest[0] = clamp_unit(src.r + dest[0] * inv_alpha);
    dest[1] = clamp_unit(src.g + dest[1] * inv_alpha);
    dest[2] = clamp_unit(src.b + dest[2] * inv_alpha);
    dest[3] = clamp_unit(src.a + dest[3] * inv_alpha);
}

auto draw_rect_area(float min_x,
                    float min_y,
                    float max_x,
                    float max_y,
                    LinearPremulColor const& color,
                    std::vector<float>& buffer,
                    int width,
                    int height) -> bool {
    auto const clipped_min_x = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto const clipped_min_y = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto const clipped_max_x = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto const clipped_max_y = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (clipped_min_x >= clipped_max_x || clipped_min_y >= clipped_max_y) {
        return false;
    }

    auto const row_stride = static_cast<std::size_t>(width) * 4u;
    for (int y = clipped_min_y; y < clipped_max_y; ++y) {
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (int x = clipped_min_x; x < clipped_max_x; ++x) {
            auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
            blend_pixel(dest, color);
        }
    }
    return true;
}

auto draw_rect_command(Scene::RectCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    auto color = make_linear_color(command.color);
    return draw_rect_area(command.min_x,
                          command.min_y,
                          command.max_x,
                          command.max_y,
                          color,
                          buffer,
                          width,
                          height);
}

auto draw_rounded_rect_command(Scene::RoundedRectCommand const& command,
                               std::vector<float>& buffer,
                               int width,
                               int height) -> bool {
    auto color = make_linear_color(command.color);

    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);

    auto width_f = std::max(0.0f, max_x - min_x);
    auto height_f = std::max(0.0f, max_y - min_y);
    if (width_f <= 0.0f || height_f <= 0.0f) {
        return false;
    }

    auto clamp_positive = [](float value) -> float {
        return std::max(0.0f, value);
    };

    auto radius_tl = clamp_positive(command.radius_top_left);
    auto radius_tr = clamp_positive(command.radius_top_right);
    auto radius_br = clamp_positive(command.radius_bottom_right);
    auto radius_bl = clamp_positive(command.radius_bottom_left);

    auto adjust_pair = [](float& a, float& b, float limit) {
        if (limit <= 0.0f) {
            a = 0.0f;
            b = 0.0f;
            return;
        }
        auto sum = a + b;
        if (sum > limit && sum > 0.0f) {
            auto scale = limit / sum;
            a *= scale;
            b *= scale;
        }
    };

    adjust_pair(radius_tl, radius_tr, width_f);
    adjust_pair(radius_bl, radius_br, width_f);
    adjust_pair(radius_tl, radius_bl, height_f);
    adjust_pair(radius_tr, radius_br, height_f);

    auto min_x_i = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto max_x_i = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto min_y_i = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto max_y_i = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
        return false;
    }

    auto radius_squared = [](float radius) -> float {
        return radius * radius;
    };

    auto blend_if_inside = [&](int x, int y) -> bool {
        auto px = static_cast<float>(x) + 0.5f;
        auto py = static_cast<float>(y) + 0.5f;

        if (px < min_x || px > max_x || py < min_y || py > max_y) {
            return false;
        }

        bool inside = true;

        if (radius_tl > 0.0f && px < (min_x + radius_tl) && py < (min_y + radius_tl)) {
            auto dx = px - (min_x + radius_tl);
            auto dy = py - (min_y + radius_tl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tl);
        } else if (radius_tr > 0.0f && px > (max_x - radius_tr) && py < (min_y + radius_tr)) {
            auto dx = px - (max_x - radius_tr);
            auto dy = py - (min_y + radius_tr);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tr);
        } else if (radius_br > 0.0f && px > (max_x - radius_br) && py > (max_y - radius_br)) {
            auto dx = px - (max_x - radius_br);
            auto dy = py - (max_y - radius_br);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_br);
        } else if (radius_bl > 0.0f && px < (min_x + radius_bl) && py > (max_y - radius_bl)) {
            auto dx = px - (min_x + radius_bl);
            auto dy = py - (max_y - radius_bl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_bl);
        }

        if (!inside) {
            return false;
        }

        auto row_stride = static_cast<std::size_t>(width) * 4u;
        auto base = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 4u;
        auto* dest = buffer.data() + base;
        blend_pixel(dest, color);
        return true;
    };

    bool drawn = false;
    for (int y = min_y_i; y < max_y_i; ++y) {
        for (int x = min_x_i; x < max_x_i; ++x) {
            if (blend_if_inside(x, y)) {
                drawn = true;
            }
        }
    }
    return drawn;
}

auto draw_fallback_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                              std::size_t drawable_index,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return false;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    auto color = make_linear_color(color_from_drawable(bucket.drawable_ids[drawable_index]));
    return draw_rect_area(box.min[0],
                          box.min[1],
                          box.max[0],
                          box.max[1],
                          color,
                          buffer,
                          width,
                          height);
}

auto bounding_box_intersects(Scene::DrawableBucketSnapshot const& bucket,
                             std::size_t drawable_index,
                             int width,
                             int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return true;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return true;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    if (box.max[0] <= 0.0f || box.max[1] <= 0.0f) {
        return false;
    }
    if (box.min[0] >= static_cast<float>(width)
        || box.min[1] >= static_cast<float>(height)) {
        return false;
    }
    if (box.max[0] <= box.min[0] || box.max[1] <= box.min[1]) {
        return false;
    }
    return true;
}

auto bounding_sphere_intersects(Scene::DrawableBucketSnapshot const& bucket,
                                std::size_t drawable_index,
                                int width,
                                int height) -> bool {
    if (drawable_index >= bucket.bounds_spheres.size()) {
        return true;
    }
    auto const& sphere = bucket.bounds_spheres[drawable_index];
    auto const radius = std::max(0.0f, sphere.radius);
    auto const min_x = sphere.center[0] - radius;
    auto const max_x = sphere.center[0] + radius;
    auto const min_y = sphere.center[1] - radius;
    auto const max_y = sphere.center[1] + radius;
    if (max_x <= 0.0f || max_y <= 0.0f) {
        return false;
    }
    if (min_x >= static_cast<float>(width)
        || min_y >= static_cast<float>(height)) {
        return false;
    }
    return true;
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
    -> SP::Expected<std::vector<std::size_t>> {
    std::vector<std::size_t> offsets;
    offsets.reserve(kinds.size());
    std::size_t cursor = 0;
    for (auto kind_value : kinds) {
        auto kind = static_cast<Scene::DrawCommandKind>(kind_value);
        auto payload_size = Scene::payload_size_bytes(kind);
        if (payload_size == 0) {
            return std::unexpected(make_error("unsupported draw command kind",
                                              SP::Error::Code::InvalidType));
        }
        if (cursor + payload_size > payload.size()) {
            return std::unexpected(make_error("command payload truncated",
                                              SP::Error::Code::InvalidType));
        }
        offsets.push_back(cursor);
        cursor += payload_size;
    }
    if (cursor != payload.size()) {
        return std::unexpected(make_error("command payload size mismatch",
                                          SP::Error::Code::InvalidType));
    }
    return offsets;
}

auto encode_pixel(float const* linear_premul,
                  Builders::SurfaceDesc const& desc,
                  bool encode_srgb) -> std::array<std::uint8_t, 4> {
    auto alpha = clamp_unit(linear_premul[3]);

    std::array<float, 3> premul_linear{
        clamp_unit(linear_premul[0]),
        clamp_unit(linear_premul[1]),
        clamp_unit(linear_premul[2]),
    };

    std::array<float, 3> straight_linear{0.0f, 0.0f, 0.0f};
    if (alpha > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            straight_linear[i] = std::clamp(premul_linear[i] / alpha, 0.0f, 1.0f);
        }
    }

    std::array<float, 3> encoded{};
    if (encode_srgb) {
        for (int i = 0; i < 3; ++i) {
            auto value = linear_to_srgb(straight_linear[i]);
            if (desc.premultiplied_alpha) {
                value *= alpha;
            }
            encoded[i] = clamp_unit(value);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            auto value = desc.premultiplied_alpha ? premul_linear[i] : straight_linear[i];
            encoded[i] = clamp_unit(value);
        }
    }

    return {
        to_byte(encoded[0]),
        to_byte(encoded[1]),
        to_byte(encoded[2]),
        to_byte(alpha)
    };
}

} // namespace

PathRenderer2D::PathRenderer2D(PathSpace& space)
    : space_(space) {}

auto PathRenderer2D::render(RenderParams params) -> SP::Expected<RenderStats> {
    auto const start = std::chrono::steady_clock::now();

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

    auto sceneRevision = Builders::Scene::ReadCurrentRevision(space_, Builders::ScenePath{sceneAbsolute->getPath()});
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

    if (!surface.has_buffered()) {
        auto message = std::string{"surface does not expose a buffered frame"};
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
    case Builders::PixelFormat::RGBA8Unorm:
    case Builders::PixelFormat::BGRA8Unorm:
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        break;
    default: {
        auto message = std::string{"pixel format not supported by PathRenderer2D"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }
    }

    auto staging = surface.staging_span();
    if (staging.size() < surface.frame_bytes()) {
        auto message = std::string{"surface staging buffer smaller than expected"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::UnknownError));
    }

    auto const width = desc.size_px.width;
    auto const height = desc.size_px.height;
    if (width <= 0 || height <= 0) {
        auto message = std::string{"surface dimensions must be positive"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }

    auto const pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> linear_buffer(pixel_count * 4u, 0.0f);

    auto clear_linear = make_linear_color(params.settings.clear_color);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto* dest = linear_buffer.data() + i * 4u;
        dest[0] = clear_linear.r;
        dest[1] = clear_linear.g;
        dest[2] = clear_linear.b;
        dest[3] = clear_linear.a;
    }

    auto const stride = static_cast<std::size_t>(surface.row_stride_bytes());
    bool const is_bgra = (desc.pixel_format == Builders::PixelFormat::BGRA8Unorm
                          || desc.pixel_format == Builders::PixelFormat::BGRA8Unorm_sRGB);

    auto payload_offsets = compute_command_payload_offsets(bucket->command_kinds,
                                                           bucket->command_payload);
    if (!payload_offsets) {
        (void)set_last_error(space_, params.target_path,
                             payload_offsets.error().message.value_or("failed to prepare command payload"));
        return std::unexpected(payload_offsets.error());
    }

    auto const drawable_count = bucket->drawable_ids.size();
    std::uint64_t drawn_total = 0;
    std::uint64_t drawn_opaque = 0;
    std::uint64_t drawn_alpha = 0;
    std::uint64_t culled_drawables = 0;
    std::uint64_t executed_commands = 0;

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

        if (!bounding_box_intersects(*bucket, drawable_index, width, height)
            || !bounding_sphere_intersects(*bucket, drawable_index, width, height)) {
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

        bool drawable_drawn = false;

        if (command_count == 0) {
            drawable_drawn = draw_fallback_bounds_box(*bucket,
                                                      drawable_index,
                                                      linear_buffer,
                                                      width,
                                                      height);
            if (!drawable_drawn) {
                ++culled_drawables;
                return {};
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
                if (draw_rect_command(rect, linear_buffer, width, height)) {
                    drawable_drawn = true;
                }
                ++executed_commands;
                break;
            }
            case Scene::DrawCommandKind::RoundedRect: {
                auto rounded = read_struct<Scene::RoundedRectCommand>(bucket->command_payload,
                                                                      payload_offset);
                if (draw_rounded_rect_command(rounded, linear_buffer, width, height)) {
                    drawable_drawn = true;
                }
                ++executed_commands;
                break;
            }
            default:
                break;
            }
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
        return {};
    };

    std::vector<std::uint32_t> fallback_indices;
    if (bucket->opaque_indices.empty() && bucket->alpha_indices.empty()) {
        fallback_indices.resize(drawable_count);
        std::iota(fallback_indices.begin(), fallback_indices.end(), 0u);
    }

    auto process_pass = [&](std::vector<std::uint32_t> const& indices,
                             bool alpha_pass) -> SP::Expected<void> {
        for (auto drawable_index : indices) {
            if (auto status = process_drawable(drawable_index, alpha_pass); !status) {
                return status;
            }
        }
        return {};
    };

    if (!bucket->opaque_indices.empty()) {
        if (auto status = process_pass(bucket->opaque_indices, false); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    } else if (!fallback_indices.empty()) {
        if (auto status = process_pass(fallback_indices, false); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    }

    if (!bucket->alpha_indices.empty()) {
        if (auto status = process_pass(bucket->alpha_indices, true); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    }

    auto const encode_srgb = needs_srgb_encode(desc);
    for (int row = 0; row < height; ++row) {
        auto* row_ptr = staging.data() + static_cast<std::size_t>(row) * stride;
        for (int col = 0; col < width; ++col) {
            auto pixel_index = static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4u
                               + static_cast<std::size_t>(col) * 4u;
            auto encoded = encode_pixel(linear_buffer.data() + pixel_index, desc, encode_srgb);
            auto offset = static_cast<std::size_t>(col) * 4u;
            if (is_bgra) {
                row_ptr[offset + 0] = encoded[2];
                row_ptr[offset + 1] = encoded[1];
                row_ptr[offset + 2] = encoded[0];
            } else {
                row_ptr[offset + 0] = encoded[0];
                row_ptr[offset + 1] = encoded[1];
                row_ptr[offset + 2] = encoded[2];
            }
            row_ptr[offset + 3] = encoded[3];
        }
    }

    auto const end = std::chrono::steady_clock::now();
    auto render_ms = std::chrono::duration<double, std::milli>(end - start).count();

    surface.publish_buffered_frame(PathSurfaceSoftware::FrameInfo{
        .frame_index = params.settings.time.frame_index,
        .revision = sceneRevision->revision,
        .render_ms = render_ms,
    });

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
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/drawableCount", static_cast<std::uint64_t>(drawable_count));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/opaqueDrawables", drawn_opaque);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/alphaDrawables", drawn_alpha);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/culledDrawables", culled_drawables);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandCount", static_cast<std::uint64_t>(bucket->command_kinds.size()));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandsExecuted", executed_commands);

    RenderStats stats{};
    stats.frame_index = params.settings.time.frame_index;
    stats.revision = sceneRevision->revision;
    stats.render_ms = render_ms;
    stats.drawable_count = drawn_total;

    return stats;
}

} // namespace SP::UI
