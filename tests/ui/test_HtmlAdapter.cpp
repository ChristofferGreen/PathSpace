#include "third_party/doctest.h"

#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    CHECK(emitted->assets.front().mime_type == Html::kImageAssetReferenceMime);
}

TEST_CASE("HtmlAdapter resolves assets via callback when provided") {
    auto bucket = make_basic_bucket();

    Html::Adapter adapter;
    Html::EmitOptions options{};
    int image_resolves = 0;
    int font_resolves = 0;
    options.font_logical_paths.push_back("fonts/custom.woff2");
    options.resolve_asset =
        [&](std::string_view logical_path,
            std::uint64_t fingerprint,
            Html::AssetKind kind) -> SP::Expected<Html::Asset> {
            Html::Asset asset{};
            switch (kind) {
            case Html::AssetKind::Image:
                ++image_resolves;
                CHECK(fingerprint == 0xABCDEF0102030405ull);
                CHECK(logical_path == "images/abcdef0102030405.png");
                asset.logical_path = std::string(logical_path);
                asset.mime_type = "image/png";
                asset.bytes = {1u, 2u, 3u, 4u};
                break;
            case Html::AssetKind::Font:
                ++font_resolves;
                CHECK(fingerprint == 0);
                CHECK(logical_path == "fonts/custom.woff2");
                asset.logical_path = std::string(logical_path);
                asset.mime_type = "font/woff2";
                asset.bytes = {5u, 6u, 7u};
                break;
            }
            return asset;
        };

    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);
    CHECK(image_resolves == 1);
    CHECK(font_resolves == 1);
    REQUIRE(emitted->assets.size() == 2);

    auto find_asset = [&](std::string const& logical) -> std::optional<Html::Asset> {
        for (auto const& asset : emitted->assets) {
            if (asset.logical_path == logical) {
                return asset;
            }
        }
        return std::nullopt;
    };

    auto image_asset = find_asset("images/abcdef0102030405.png");
    REQUIRE(image_asset);
    CHECK(image_asset->mime_type == "image/png");
    CHECK(image_asset->bytes == std::vector<std::uint8_t>({1u, 2u, 3u, 4u}));

    auto font_asset = find_asset("fonts/custom.woff2");
    REQUIRE(font_asset);
    CHECK(font_asset->mime_type == "font/woff2");
    CHECK(font_asset->bytes == std::vector<std::uint8_t>({5u, 6u, 7u}));

    CHECK(emitted->css.find("@font-face") != std::string::npos);
    CHECK(emitted->css.find("assets/fonts/custom.woff2") != std::string::npos);
}

TEST_CASE("HtmlAdapter emits fingerprinted font assets from snapshot") {
    auto bucket = make_basic_bucket();
    UIScene::FontAssetReference font{};
    font.drawable_id = bucket.drawable_ids.front();
    font.resource_root = "/system/applications/demo_app/resources/fonts/PathSpaceSans/Regular";
    font.revision = 7;
    font.fingerprint = 0x0102030405060708ull;
    bucket.font_assets.push_back(font);

    Html::Adapter adapter;
    Html::EmitOptions options{};
    int font_resolves = 0;
    int image_resolves = 0;
    options.resolve_asset =
        [&](std::string_view logical_path,
            std::uint64_t fingerprint,
            Html::AssetKind kind) -> SP::Expected<Html::Asset> {
            Html::Asset asset{};
            switch (kind) {
            case Html::AssetKind::Image:
                ++image_resolves;
                asset.logical_path = std::string(logical_path);
                asset.mime_type = "image/png";
                asset.bytes = {1u, 2u, 3u};
                break;
            case Html::AssetKind::Font:
                ++font_resolves;
                CHECK(fingerprint == font.fingerprint);
                CHECK(logical_path == "fonts/0102030405060708.woff2");
                asset.logical_path = std::string(logical_path);
                asset.mime_type = "font/woff2";
                asset.bytes = {9u, 8u, 7u};
                break;
            }
            return asset;
        };

    auto emitted = adapter.emit(bucket, options);
    REQUIRE(emitted);
    CHECK(image_resolves == 1);
    CHECK(font_resolves == 1);
    CHECK(emitted->css.find("@font-face") != std::string::npos);
    CHECK(emitted->css.find("assets/fonts/0102030405060708.woff2") != std::string::npos);
    CHECK(emitted->css.find("PathSpaceSans") != std::string::npos);

    bool found_font_asset = false;
    for (auto const& asset : emitted->assets) {
        if (asset.logical_path == "fonts/0102030405060708.woff2") {
            found_font_asset = true;
            CHECK(asset.bytes == std::vector<std::uint8_t>({9u, 8u, 7u}));
        }
    }
    CHECK(found_font_asset);
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
