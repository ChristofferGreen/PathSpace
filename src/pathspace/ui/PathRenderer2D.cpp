#include <pathspace/ui/PathRenderer2D.hpp>

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

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

auto encode_color(Builders::SurfaceDesc const& desc,
                  std::array<float, 4> const& rgba) -> std::array<std::uint8_t, 4> {
    float alpha = clamp_unit(rgba[3]);

    float red = clamp_unit(rgba[0]);
    float green = clamp_unit(rgba[1]);
    float blue = clamp_unit(rgba[2]);

    if (desc.premultiplied_alpha) {
        red *= alpha;
        green *= alpha;
        blue *= alpha;
    }

    return {
        to_byte(red),
        to_byte(green),
        to_byte(blue),
        to_byte(alpha)
    };
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

    auto encoded_clear = encode_color(desc, params.settings.clear_color);
    auto const width = desc.size_px.width;
    auto const height = desc.size_px.height;
    auto const stride = static_cast<std::size_t>(surface.row_stride_bytes());
    bool const is_bgra = (desc.pixel_format == Builders::PixelFormat::BGRA8Unorm
                          || desc.pixel_format == Builders::PixelFormat::BGRA8Unorm_sRGB);

    for (int row = 0; row < height; ++row) {
        auto* rowPtr = staging.data() + static_cast<std::size_t>(row) * stride;
        for (int col = 0; col < width; ++col) {
            auto offset = static_cast<std::size_t>(col) * 4u;
            if (is_bgra) {
                rowPtr[offset + 0] = encoded_clear[2];
                rowPtr[offset + 1] = encoded_clear[1];
                rowPtr[offset + 2] = encoded_clear[0];
            } else {
                rowPtr[offset + 0] = encoded_clear[0];
                rowPtr[offset + 1] = encoded_clear[1];
                rowPtr[offset + 2] = encoded_clear[2];
            }
            rowPtr[offset + 3] = encoded_clear[3];
        }
    }

    std::uint64_t rendered_drawables = 0;
    if (!bucket->drawable_ids.empty()) {
        auto const drawable_count = bucket->drawable_ids.size();
        auto const& bounds = bucket->bounds_boxes;
        auto const& bounds_valid = bucket->bounds_box_valid;
        auto const& visibility = bucket->visibility;

        for (std::size_t i = 0; i < drawable_count; ++i) {
            if (i < visibility.size() && visibility[i] == 0) {
                continue;
            }
            if (i >= bounds.size()) {
                continue;
            }
            if (i < bounds_valid.size() && bounds_valid[i] == 0) {
                continue;
            }

            auto const& box = bounds[i];
            auto const min_x = static_cast<int>(std::floor(box.min[0]));
            auto const min_y = static_cast<int>(std::floor(box.min[1]));
            auto const max_x = static_cast<int>(std::ceil(box.max[0]));
            auto const max_y = static_cast<int>(std::ceil(box.max[1]));

            auto const clipped_min_x = std::clamp(min_x, 0, width);
            auto const clipped_min_y = std::clamp(min_y, 0, height);
            auto const clipped_max_x = std::clamp(max_x, 0, width);
            auto const clipped_max_y = std::clamp(max_y, 0, height);

            if (clipped_min_x >= clipped_max_x || clipped_min_y >= clipped_max_y) {
                continue;
            }

            auto color = encode_color(desc, color_from_drawable(bucket->drawable_ids[i]));

            for (int y = clipped_min_y; y < clipped_max_y; ++y) {
                auto* rowPtr = staging.data() + static_cast<std::size_t>(y) * stride;
                for (int x = clipped_min_x; x < clipped_max_x; ++x) {
                    auto offset = static_cast<std::size_t>(x) * 4u;
                    if (is_bgra) {
                        rowPtr[offset + 0] = color[2];
                        rowPtr[offset + 1] = color[1];
                        rowPtr[offset + 2] = color[0];
                    } else {
                        rowPtr[offset + 0] = color[0];
                        rowPtr[offset + 1] = color[1];
                        rowPtr[offset + 2] = color[2];
                    }
                    rowPtr[offset + 3] = color[3];
                }
            }
            ++rendered_drawables;
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
    auto drawableCountPath = metricsBase + "/drawableCount";
    (void)replace_single<std::uint64_t>(space_, drawableCountPath, rendered_drawables);

    RenderStats stats{};
    stats.frame_index = params.settings.time.frame_index;
    stats.revision = sceneRevision->revision;
    stats.render_ms = render_ms;
    stats.drawable_count = bucket->drawable_ids.size();

    return stats;
}

} // namespace SP::UI
