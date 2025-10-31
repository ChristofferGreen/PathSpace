#include <pathspace/ui/SceneUtilities.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <cmath>
#include <cstring>
#include <utility>

namespace SP::UI::Scene {

namespace {

template <typename Cmd>
auto read_command(std::vector<std::uint8_t> const& payload, std::size_t offset) -> Cmd {
    Cmd command{};
    std::memcpy(&command, payload.data() + offset, sizeof(Cmd));
    return command;
}

template <typename Cmd>
auto write_command(std::vector<std::uint8_t>& payload,
                   std::size_t offset,
                   Cmd const& command) -> void {
    std::memcpy(payload.data() + offset, &command, sizeof(Cmd));
}

} // namespace

auto MakeIdentityTransform() -> Transform {
    Transform transform{};
    for (int index = 0; index < 16; ++index) {
        transform.elements[index] = (index % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto BuildSolidBackground(float width,
                          float height,
                          SolidBackgroundOptions const& options) -> DrawableBucketSnapshot {
    DrawableBucketSnapshot bucket{};
    auto drawable_id = options.drawable_id;
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(options.transform);

    BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {width, height, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    BoundingSphere sphere{};
    sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0]
                              + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(static_cast<std::uint32_t>(options.layer));
    bucket.z_values.push_back(options.z);
    bucket.material_ids.push_back(options.material_id);
    bucket.pipeline_flags.push_back(options.pipeline_flags);
    bucket.visibility.push_back(options.visibility);
    bucket.command_offsets.push_back(0);
    bucket.command_counts.push_back(1);
    bucket.opaque_indices.push_back(0);
    bucket.clip_head_indices.push_back(-1);

    RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = width;
    rect.max_y = height;
    rect.color = options.color;

    bucket.command_kinds.push_back(static_cast<std::uint32_t>(DrawCommandKind::Rect));
    bucket.command_payload.resize(sizeof(RectCommand));
    std::memcpy(bucket.command_payload.data(), &rect, sizeof(RectCommand));

    if (!options.authoring_node_id.empty()) {
        bucket.authoring_map.push_back(DrawableAuthoringMapEntry{
            drawable_id,
            options.authoring_node_id,
            0,
            0});
    }
    auto fingerprint = options.fingerprint.value_or(options.drawable_id);
    bucket.drawable_fingerprints.push_back(fingerprint);
    return bucket;
}

auto TranslateDrawableBucket(DrawableBucketSnapshot& bucket, float dx, float dy) -> void {
    for (auto& sphere : bucket.bounds_spheres) {
        sphere.center[0] += dx;
        sphere.center[1] += dy;
    }
    for (auto& box : bucket.bounds_boxes) {
        box.min[0] += dx;
        box.max[0] += dx;
        box.min[1] += dy;
        box.max[1] += dy;
    }
    std::size_t payload_offset = 0;
    for (auto kind_value : bucket.command_kinds) {
        auto kind = static_cast<DrawCommandKind>(kind_value);
        switch (kind) {
        case DrawCommandKind::Rect: {
            auto command = read_command<RectCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        case DrawCommandKind::RoundedRect: {
            auto command = read_command<RoundedRectCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        case DrawCommandKind::TextGlyphs: {
            auto command = read_command<TextGlyphsCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        default:
            break;
        }
        payload_offset += payload_size_bytes(kind);
    }
}

auto AppendDrawableBucket(DrawableBucketSnapshot& dest,
                          DrawableBucketSnapshot const& src) -> void {
    if (src.drawable_ids.empty()) {
        return;
    }

    auto drawable_base = static_cast<std::uint32_t>(dest.drawable_ids.size());
    auto command_base = static_cast<std::uint32_t>(dest.command_kinds.size());
    auto clip_base = static_cast<std::int32_t>(dest.clip_nodes.size());

    dest.drawable_ids.insert(dest.drawable_ids.end(), src.drawable_ids.begin(), src.drawable_ids.end());
    dest.world_transforms.insert(dest.world_transforms.end(), src.world_transforms.begin(), src.world_transforms.end());
    dest.bounds_spheres.insert(dest.bounds_spheres.end(), src.bounds_spheres.begin(), src.bounds_spheres.end());
    dest.bounds_boxes.insert(dest.bounds_boxes.end(), src.bounds_boxes.begin(), src.bounds_boxes.end());
    dest.bounds_box_valid.insert(dest.bounds_box_valid.end(), src.bounds_box_valid.begin(), src.bounds_box_valid.end());
    dest.layers.insert(dest.layers.end(), src.layers.begin(), src.layers.end());
    dest.z_values.insert(dest.z_values.end(), src.z_values.begin(), src.z_values.end());
    dest.material_ids.insert(dest.material_ids.end(), src.material_ids.begin(), src.material_ids.end());
    dest.pipeline_flags.insert(dest.pipeline_flags.end(), src.pipeline_flags.begin(), src.pipeline_flags.end());
    dest.visibility.insert(dest.visibility.end(), src.visibility.begin(), src.visibility.end());

    for (auto offset : src.command_offsets) {
        dest.command_offsets.push_back(offset + command_base);
    }
    dest.command_counts.insert(dest.command_counts.end(), src.command_counts.begin(), src.command_counts.end());

    dest.command_kinds.insert(dest.command_kinds.end(), src.command_kinds.begin(), src.command_kinds.end());
    dest.command_payload.insert(dest.command_payload.end(), src.command_payload.begin(), src.command_payload.end());

    for (auto index : src.opaque_indices) {
        dest.opaque_indices.push_back(index + drawable_base);
    }
    for (auto index : src.alpha_indices) {
        dest.alpha_indices.push_back(index + drawable_base);
    }

    for (auto const& entry : src.layer_indices) {
        LayerIndices adjusted{entry.layer, {}};
        adjusted.indices.reserve(entry.indices.size());
        for (auto idx : entry.indices) {
            adjusted.indices.push_back(idx + drawable_base);
        }
        dest.layer_indices.push_back(std::move(adjusted));
    }

    for (auto node : src.clip_nodes) {
        if (node.next >= 0) {
            node.next += clip_base;
        }
        dest.clip_nodes.push_back(node);
    }
    for (auto head : src.clip_head_indices) {
        dest.clip_head_indices.push_back(head >= 0 ? head + clip_base : -1);
    }

    dest.authoring_map.insert(dest.authoring_map.end(), src.authoring_map.begin(), src.authoring_map.end());
    dest.drawable_fingerprints.insert(dest.drawable_fingerprints.end(),
                                      src.drawable_fingerprints.begin(),
                                      src.drawable_fingerprints.end());
}

} // namespace SP::UI::Scene
