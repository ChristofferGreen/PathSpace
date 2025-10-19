#include "third_party/doctest.h"

#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

using namespace SP::UI;
namespace UIScene = SP::UI::Scene;

namespace {

template <typename Command>
void append_command(UIScene::DrawableBucketSnapshot& bucket,
                    UIScene::DrawCommandKind kind,
                    Command const& command) {
    auto offset = static_cast<std::uint32_t>(bucket.command_kinds.size());
    bucket.command_kinds.push_back(static_cast<std::uint32_t>(kind));
    auto const* data = reinterpret_cast<std::uint8_t const*>(&command);
    bucket.command_payload.insert(bucket.command_payload.end(), data, data + sizeof(Command));
    (void)offset;
}

UIScene::DrawableBucketSnapshot make_basic_bucket() {
    UIScene::DrawableBucketSnapshot bucket{};
    bucket.drawable_ids = {0x1u, 0x2u};
    bucket.world_transforms.resize(2);
    bucket.bounds_spheres.resize(2);
    bucket.bounds_boxes.resize(2);
    bucket.bounds_box_valid = {1, 1};
    bucket.layers = {0, 0};
    bucket.z_values = {0.0f, 0.1f};
    bucket.material_ids = {0, 0};
    bucket.pipeline_flags = {0, 0};
    bucket.visibility = {1, 1};
    bucket.command_offsets = {0, 1};
    bucket.command_counts = {1, 1};
    bucket.opaque_indices = {0};
    bucket.alpha_indices = {1};
    bucket.clip_head_indices = {-1, -1};

    UIScene::RectCommand rect{};
    rect.min_x = 10.0f;
    rect.min_y = 12.0f;
    rect.max_x = 42.0f;
    rect.max_y = 30.0f;
    rect.color = {0.2f, 0.4f, 0.6f, 0.8f};
    append_command(bucket, UIScene::DrawCommandKind::Rect, rect);

    UIScene::ImageCommand image{};
    image.min_x = 50.0f;
    image.min_y = 18.0f;
    image.max_x = 82.0f;
    image.max_y = 54.0f;
    image.image_fingerprint = 0xABCDEF0102030405ull;
    image.tint = {1.0f, 1.0f, 1.0f, 0.75f};
    append_command(bucket, UIScene::DrawCommandKind::Image, image);

    return bucket;
}

} // namespace

TEST_CASE("HtmlAdapter emits DOM for rect and image") {
    auto bucket = make_basic_bucket();

    Html::Adapter adapter;
    Html::EmitOptions options{};
    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);

    CHECK_FALSE(emitted->used_canvas_fallback);
    CHECK(emitted->dom.find("ps-rect") != std::string::npos);
    CHECK(emitted->dom.find("ps-image") != std::string::npos);
    CHECK(emitted->css.find(".ps-scene") != std::string::npos);
    CHECK(emitted->canvas_commands == "[]");

    REQUIRE(emitted->assets.size() == 1);
    CHECK(emitted->assets.front().logical_path == "images/abcdef0102030405.png");
    CHECK(emitted->assets.front().mime_type == "application/vnd.pathspace.image+ref");
}

TEST_CASE("HtmlAdapter falls back to canvas when DOM budget exceeded") {
    auto bucket = make_basic_bucket();

    Html::Adapter adapter;
    Html::EmitOptions options{};
    options.max_dom_nodes = 1;
    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);

    CHECK(emitted->used_canvas_fallback);
    CHECK(emitted->dom.empty());
    CHECK(emitted->css.empty());
    CHECK(emitted->canvas_commands.find("\"type\":\"rect\"") != std::string::npos);
}

TEST_CASE("HtmlAdapter honours canvas-only preference") {
    auto bucket = make_basic_bucket();

    Html::Adapter adapter;
    Html::EmitOptions options{};
    options.prefer_dom = false;
    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);
    CHECK(emitted->used_canvas_fallback);
    CHECK(emitted->dom.empty());
    CHECK(emitted->canvas_commands.find("\"type\":\"image\"") != std::string::npos);
}
