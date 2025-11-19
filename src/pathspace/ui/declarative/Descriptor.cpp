#include <pathspace/ui/declarative/Descriptor.hpp>

#include "DescriptorDetail.hpp"

#include "../BuildersDetail.hpp"
#include "../WidgetDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/SceneUtilities.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace SP::UI::Declarative {
namespace {

namespace Detail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;
namespace DescriptorDetail = SP::UI::Declarative::DescriptorDetail;

struct BucketVisitor {
    DescriptorBucketOptions options;
    std::string authoring_root;

    auto operator()(ButtonDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ButtonPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildButtonPreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(ToggleDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TogglePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildTogglePreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(SliderDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::SliderPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildSliderPreview(descriptor.style,
                                           descriptor.range,
                                           descriptor.state,
                                           preview);
    }

    auto operator()(ListDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ListPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildListPreview(descriptor.style,
                                                std::span<BuilderWidgets::ListItem const>{descriptor.items},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(TreeDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TreePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildTreePreview(descriptor.style,
                                                std::span<BuilderWidgets::TreeNode const>{descriptor.nodes},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(LabelDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        auto drawable_id = std::hash<std::string>{}(authoring_root);
        auto built = SP::UI::Builders::Text::BuildTextBucket(descriptor.text,
                                                              0.0f,
                                                              descriptor.typography.line_height,
                                                              descriptor.typography,
                                                              descriptor.color,
                                                              drawable_id,
                                                              authoring_root + "/label",
                                                              0.0f);
        if (!built) {
            return std::unexpected(DescriptorDetail::MakeDescriptorError("Failed to build label bucket"));
        }
        return built->bucket;
    }

    auto operator()(StackDescriptor const&) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        return SP::UI::Scene::DrawableBucketSnapshot{};
    }

    auto operator()(InputFieldDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        auto bucket = Detail::build_text_field_bucket(descriptor.style,
                                                      descriptor.state,
                                                      authoring_root,
                                                      options.pulsing_highlight);
        return bucket;
    }

    auto operator()(PaintSurfaceDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        SP::UI::Scene::DrawableBucketSnapshot bucket{};
        std::size_t total_points = 0;
        for (auto const& stroke : descriptor.strokes) {
            total_points += stroke.points.size();
        }
        if (total_points == 0) {
            return bucket;
        }

        bucket.stroke_points.reserve(total_points);
        std::vector<std::uint32_t> opaque_indices;
        std::vector<std::uint32_t> alpha_indices;
        auto identity = SP::UI::Scene::MakeIdentityTransform();
        auto base_id = std::hash<std::string>{}(authoring_root);

        for (auto const& stroke : descriptor.strokes) {
            if (stroke.points.empty()) {
                continue;
            }

            auto command_index = static_cast<std::uint32_t>(bucket.command_kinds.size());
            auto point_offset = static_cast<std::uint32_t>(bucket.stroke_points.size());

            float min_x = std::numeric_limits<float>::max();
            float min_y = std::numeric_limits<float>::max();
            float max_x = std::numeric_limits<float>::lowest();
            float max_y = std::numeric_limits<float>::lowest();
            for (auto const& point : stroke.points) {
                SP::UI::Scene::StrokePoint scene_point{point.x, point.y};
                bucket.stroke_points.push_back(scene_point);
                min_x = std::min(min_x, point.x);
                min_y = std::min(min_y, point.y);
                max_x = std::max(max_x, point.x);
                max_y = std::max(max_y, point.y);
            }

            if (min_x > max_x) {
                min_x = 0.0f;
                min_y = 0.0f;
                max_x = 0.0f;
                max_y = 0.0f;
            }

            SP::UI::Scene::StrokeCommand command{};
            command.min_x = min_x;
            command.min_y = min_y;
            command.max_x = max_x;
            command.max_y = max_y;
            command.thickness = std::max(stroke.meta.brush_size, 0.5f);
            command.point_offset = point_offset;
            command.point_count = static_cast<std::uint32_t>(stroke.points.size());
            command.color = stroke.meta.color;

            auto payload_offset = bucket.command_payload.size();
            bucket.command_payload.resize(payload_offset + sizeof(command));
            std::memcpy(bucket.command_payload.data() + payload_offset, &command, sizeof(command));
            bucket.command_kinds.push_back(static_cast<std::uint32_t>(SP::UI::Scene::DrawCommandKind::Stroke));

            bucket.command_offsets.push_back(command_index);
            bucket.command_counts.push_back(1);

            auto drawable_id = base_id ^ stroke.id;
            bucket.drawable_ids.push_back(drawable_id);
            bucket.drawable_fingerprints.push_back(drawable_id);
            bucket.world_transforms.push_back(identity);

            SP::UI::Scene::BoundingBox box{};
            box.min = {min_x, min_y, 0.0f};
            box.max = {max_x, max_y, 0.0f};
            bucket.bounds_boxes.push_back(box);
            bucket.bounds_box_valid.push_back(1);

            auto center_x = (min_x + max_x) * 0.5f;
            auto center_y = (min_y + max_y) * 0.5f;
            auto radius = std::hypot(max_x - center_x, max_y - center_y);
            SP::UI::Scene::BoundingSphere sphere{};
            sphere.center = {center_x, center_y, 0.0f};
            sphere.radius = radius;
            bucket.bounds_spheres.push_back(sphere);

            bucket.layers.push_back(0);
            bucket.z_values.push_back(static_cast<float>(bucket.z_values.size()));
            bucket.material_ids.push_back(0);
            bucket.pipeline_flags.push_back(0);
            bucket.visibility.push_back(1);
            bucket.clip_head_indices.push_back(-1);

            if (stroke.meta.color[3] < 0.999f) {
                alpha_indices.push_back(static_cast<std::uint32_t>(bucket.drawable_ids.size() - 1));
            } else {
                opaque_indices.push_back(static_cast<std::uint32_t>(bucket.drawable_ids.size() - 1));
            }
        }

        bucket.opaque_indices = std::move(opaque_indices);
        bucket.alpha_indices = std::move(alpha_indices);
        return bucket;
    }
};

} // namespace

auto LoadWidgetDescriptor(PathSpace& space,
                          SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<WidgetDescriptor> {
    auto root = widget.getPath();
    auto removed_value = space.read<bool, std::string>(root + "/state/removed");
    if (removed_value) {
        if (*removed_value) {
            return std::unexpected(DescriptorDetail::MakeDescriptorError("Widget removed",
                                                       SP::Error::Code::NoObjectFound));
        }
    } else {
        auto const& error = removed_value.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
    }

    auto kind_value = space.read<std::string, std::string>(root + "/meta/kind");
    if (!kind_value) {
        return std::unexpected(kind_value.error());
    }
    auto kind = DescriptorDetail::KindFromString(*kind_value);
    if (!kind) {
        return std::unexpected(DescriptorDetail::MakeDescriptorError(
            "Unsupported declarative widget kind: " + *kind_value,
            SP::Error::Code::NotSupported));
    }

    auto theme = DescriptorDetail::ResolveThemeForWidget(space, widget);
    if (!theme) {
        return std::unexpected(theme.error());
    }

    WidgetDescriptor descriptor{};
    descriptor.kind = *kind;
    descriptor.widget = widget;

    switch (*kind) {
    case WidgetKind::Button: {
        auto loaded = DescriptorDetail::ReadButtonDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Toggle: {
        auto loaded = DescriptorDetail::ReadToggleDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Slider: {
        auto loaded = DescriptorDetail::ReadSliderDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::List: {
        auto loaded = DescriptorDetail::ReadListDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Tree: {
        auto loaded = DescriptorDetail::ReadTreeDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Label: {
        auto loaded = DescriptorDetail::ReadLabelDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Stack: {
        auto loaded = DescriptorDetail::ReadStackDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::InputField: {
        auto loaded = DescriptorDetail::ReadInputFieldDescriptor(space, widget, theme->theme);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::PaintSurface: {
        auto loaded = DescriptorDetail::ReadPaintSurfaceDescriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    }

    DescriptorDetail::ApplyThemeOverride(*theme, descriptor);
    return descriptor;
}

auto BuildWidgetBucket(WidgetDescriptor const& descriptor,
                       DescriptorBucketOptions const& options)
    -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
    BucketVisitor visitor{
        .options = options,
        .authoring_root = descriptor.widget.getPath(),
    };
    return std::visit(visitor, descriptor.data);
}

} // namespace SP::UI::Declarative
