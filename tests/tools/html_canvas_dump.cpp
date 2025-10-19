#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

namespace {
UIScene::DrawableBucketSnapshot make_bucket() {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {1, 2};
    UIScene::Transform transform{};
    for (std::size_t i = 0; i < transform.elements.size(); ++i) {
        transform.elements[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    bucket.world_transforms = {transform, transform};

    UIScene::BoundingSphere sphere{};
    sphere.center = {30.0f, 20.0f, 0.0f};
    sphere.radius = 36.0f;
    bucket.bounds_spheres = {sphere, sphere};
    UIScene::BoundingBox box{};
    box.min = {12.0f, 8.0f, 0.0f};
    box.max = {40.0f, 28.0f, 0.0f};
    bucket.bounds_boxes = {box, box};
    bucket.bounds_box_valid = {1, 1};

    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 1.0f};
    bucket.material_ids = {0, 0};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.clip_head_indices = {-1, -1};
    bucket.drawable_fingerprints = {0xAAu, 0xBBu};

    UIScene::RectCommand rect{};
    rect.min_x = 12.0f;
    rect.min_y = 8.0f;
    rect.max_x = 40.0f;
    rect.max_y = 24.0f;
    rect.color = {0.2f, 0.4f, 0.7f, 1.0f};
    auto offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rect, sizeof(rect));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::Rect));

    UIScene::RoundedRectCommand rounded{};
    rounded.min_x = 44.0f;
    rounded.min_y = 18.0f;
    rounded.max_x = 70.0f;
    rounded.max_y = 40.0f;
    rounded.radius_top_left = 3.0f;
    rounded.radius_top_right = 2.0f;
    rounded.radius_bottom_right = 4.0f;
    rounded.radius_bottom_left = 1.5f;
    rounded.color = {0.9f, 0.3f, 0.2f, 0.6f};
    offset = bucket.command_payload.size();
    bucket.command_payload.resize(offset + sizeof(UIScene::RoundedRectCommand));
    std::memcpy(bucket.command_payload.data() + offset, &rounded, sizeof(rounded));
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(UIScene::DrawCommandKind::RoundedRect));

    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    return bucket;
}
} // namespace

int main() {
    auto bucket = make_bucket();
    Html::Adapter adapter;
    Html::EmitOptions options{};
    options.prefer_dom = false; // ensure canvas path available
    auto emitted = adapter.emit(bucket, options);
    if (!emitted) {
        std::cerr << "Failed to emit HTML: "
                  << emitted.error().message.value_or("<unspecified>") << "\n";
        return EXIT_FAILURE;
    }

    std::cout << emitted->canvas_commands << std::endl;
    return EXIT_SUCCESS;
}
