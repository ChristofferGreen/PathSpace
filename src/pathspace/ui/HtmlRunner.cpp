#include <pathspace/ui/HtmlRunner.hpp>

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SP::UI::Html {
namespace {

auto identity_transform() -> Scene::Transform {
    Scene::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

template <typename Command>
void append_command(Scene::DrawableBucketSnapshot& bucket,
                    Scene::DrawCommandKind kind,
                    Command const& command) {
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(Command));
    std::memcpy(bucket.command_payload.data() + offset, &command, sizeof(Command));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(kind));
}

} // namespace

auto commands_to_bucket(std::span<CanvasCommand const> commands,
                        CanvasReplayOptions const& options)
    -> SP::Expected<Scene::DrawableBucketSnapshot> {
    Scene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids.reserve(commands.size());
    bucket.world_transforms.reserve(commands.size());
    bucket.bounds_spheres.reserve(commands.size());
    bucket.bounds_boxes.reserve(commands.size());
    bucket.bounds_box_valid.reserve(commands.size());
    bucket.layers.reserve(commands.size());
    bucket.z_values.reserve(commands.size());
    bucket.material_ids.reserve(commands.size());
    bucket.pipeline_flags.reserve(commands.size());
    bucket.visibility.reserve(commands.size());
    bucket.command_offsets.reserve(commands.size());
    bucket.command_counts.reserve(commands.size());
    bucket.clip_head_indices.reserve(commands.size());
    bucket.drawable_fingerprints.reserve(commands.size());

    std::vector<std::uint32_t> opaque_indices;
    std::vector<std::uint32_t> alpha_indices;
    opaque_indices.reserve(commands.size());
    alpha_indices.reserve(commands.size());

    for (std::size_t i = 0; i < commands.size(); ++i) {
        auto const& command = commands[i];

        auto drawable_id = options.base_drawable_id + static_cast<std::uint64_t>(i);
        bucket.drawable_ids.push_back(drawable_id);
        bucket.world_transforms.push_back(identity_transform());

        Scene::BoundingSphere sphere{};
        float center_x = command.x + command.width * 0.5f;
        float center_y = command.y + command.height * 0.5f;
        sphere.center = {center_x, center_y, 0.0f};
        float half_w = command.width * 0.5f;
        float half_h = command.height * 0.5f;
        sphere.radius = std::sqrt(half_w * half_w + half_h * half_h);
        bucket.bounds_spheres.push_back(sphere);

        Scene::BoundingBox box{};
        box.min = {command.x, command.y, 0.0f};
        box.max = {command.x + command.width, command.y + command.height, 0.0f};
        bucket.bounds_boxes.push_back(box);
        bucket.bounds_box_valid.push_back(1);

        bucket.layers.push_back(options.default_layer);
        bucket.z_values.push_back(static_cast<float>(i) * options.z_step);
        bucket.material_ids.push_back(0);
        bucket.pipeline_flags.push_back(0);
        bucket.visibility.push_back(1);
        bucket.clip_head_indices.push_back(-1);
        bucket.drawable_fingerprints.push_back(drawable_id);

        auto command_offset = static_cast<std::uint32_t>(bucket.command_kinds.size());
        bucket.command_offsets.push_back(command_offset);

        auto alpha = command.opacity;
        bool is_alpha = alpha < 0.999f;

        switch (command.type) {
        case CanvasCommandType::Rect: {
            Scene::RectCommand rect{};
            rect.min_x = command.x;
            rect.min_y = command.y;
            rect.max_x = command.x + command.width;
            rect.max_y = command.y + command.height;
            rect.color = command.color;
            append_command(bucket, Scene::DrawCommandKind::Rect, rect);
            bucket.command_counts.push_back(1);
            break;
        }
        case CanvasCommandType::RoundedRect: {
            Scene::RoundedRectCommand rounded{};
            rounded.min_x = command.x;
            rounded.min_y = command.y;
            rounded.max_x = command.x + command.width;
            rounded.max_y = command.y + command.height;
            rounded.radius_top_left = command.corner_radii[0];
            rounded.radius_top_right = command.corner_radii[1];
            rounded.radius_bottom_right = command.corner_radii[2];
            rounded.radius_bottom_left = command.corner_radii[3];
            rounded.color = command.color;
            append_command(bucket, Scene::DrawCommandKind::RoundedRect, rounded);
            bucket.command_counts.push_back(1);
            break;
        }
        default:
            return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                             "Unsupported canvas command for replay"});
        }

        if (is_alpha) {
            alpha_indices.push_back(static_cast<std::uint32_t>(i));
        } else {
            opaque_indices.push_back(static_cast<std::uint32_t>(i));
        }
    }

    bucket.opaque_indices = std::move(opaque_indices);
    bucket.alpha_indices = std::move(alpha_indices);
    return bucket;
}

} // namespace SP::UI::Html
