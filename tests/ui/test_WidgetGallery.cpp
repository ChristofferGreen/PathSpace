#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/system/Standard.hpp>
#include <pathspace/ui/FontManager.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/runtime/TextRuntime.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace {

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

} // namespace

TEST_CASE("widgets theme load registers fonts") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/theme_font_registration"};
    auto app_root_view = SP::App::AppRootPathView{app_root.getPath()};

    auto selection = SP::UI::Runtime::Widgets::LoadTheme(space, app_root_view, "sunset");
    REQUIRE(selection);

    SP::UI::FontManager manager(space);

    std::array<SP::UI::Runtime::Widgets::TypographyStyle const*, 8> styles{
        &selection->theme.button.typography,
        &selection->theme.slider.label_typography,
        &selection->theme.list.item_typography,
        &selection->theme.tree.label_typography,
        &selection->theme.text_field.typography,
        &selection->theme.text_area.typography,
        &selection->theme.heading,
        &selection->theme.caption,
    };

    for (auto* style : styles) {
        auto resolved = manager.resolve_font(app_root_view,
                                             style->font_family,
                                             style->font_style);
        INFO("resolving font " << style->font_family << "/" << style->font_style);
        REQUIRE_MESSAGE(resolved, SP::describeError(resolved.error()));
        CHECK(resolved->active_revision > 0);
    }
}

TEST_CASE("widget gallery snapshot persists font assets") {
    SP::PathSpace space;
    REQUIRE(SP::System::LaunchStandard(space));

    auto app = SP::App::Create(space, "widget_gallery_font_snapshot");
    REQUIRE(app);
    auto app_root_view = SP::App::AppRootPathView{app->getPath()};

    auto theme_selection = SP::UI::Runtime::Widgets::LoadTheme(space, app_root_view, "");
    REQUIRE(theme_selection);

    SP::Window::CreateOptions window_opts{};
    window_opts.name = "gallery_window";
    window_opts.visible = false;
    auto window = SP::Window::Create(space, app_root_view, window_opts);
    REQUIRE(window);

    SP::Scene::CreateOptions scene_opts{};
    scene_opts.name = "gallery_text_scene";
    auto scene = SP::Scene::Create(space, app_root_view, window->path, scene_opts);
    REQUIRE(scene);

    SP::UI::Runtime::Text::ScopedShapingContext shaping_ctx(space, app_root_view);

    auto heading_params = SP::UI::Runtime::Widgets::LabelBuildParams::Make(
                              "Widget Gallery",
                              theme_selection->theme.heading)
                              .WithOrigin(18.0f, 18.0f)
                              .WithColor(theme_selection->theme.heading_color)
                              .WithDrawable(0xC0FFEE10ull, "gallery/heading", 0.1f);

    auto heading = SP::UI::Runtime::Widgets::BuildLabel(heading_params);
    REQUIRE(heading);

    auto bucket = heading->bucket;
    REQUIRE_FALSE(bucket.font_assets.empty());
    REQUIRE_FALSE(bucket.glyph_vertices.empty());

    for (auto const& asset : bucket.font_assets) {
        CHECK(asset.fingerprint != std::uint64_t{0});
        CHECK_FALSE(asset.resource_root.empty());
    }

    SP::UI::Scene::SceneSnapshotBuilder builder(space, app_root_view, scene->path);
    SP::UI::Scene::SnapshotPublishOptions publish_opts{};
    publish_opts.metadata.author = "WidgetGalleryTest";
    publish_opts.metadata.tool_version = "UITest";
    auto published = builder.publish(publish_opts, bucket);
    REQUIRE(published);

    auto revision_base = std::string(scene->path.getPath()) + "/builds/"
                       + format_revision(*published);

    auto font_assets_bytes = space.read<std::vector<std::uint8_t>>(
        revision_base + "/bucket/font-assets.bin");
    REQUIRE(font_assets_bytes);
    CHECK_FALSE(font_assets_bytes->empty());

    if (auto* artifact_dir = std::getenv("PATHSPACE_TEST_ARTIFACT_DIR")) {
        std::error_code ec;
        std::filesystem::create_directories(artifact_dir, ec);
        auto artifact_path = std::filesystem::path(artifact_dir) / "widget_gallery_font_assets.bin";
        std::ofstream out(artifact_path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<char const*>(font_assets_bytes->data()),
                      static_cast<std::streamsize>(font_assets_bytes->size()));
        }
    }

    auto decoded = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revision_base);
    REQUIRE(decoded);
    REQUIRE_FALSE(decoded->font_assets.empty());
    REQUIRE_FALSE(decoded->glyph_vertices.empty());
    REQUIRE_EQ(decoded->font_assets.size(), bucket.font_assets.size());

    for (std::size_t i = 0; i < bucket.font_assets.size(); ++i) {
        CHECK_EQ(decoded->font_assets[i].resource_root, bucket.font_assets[i].resource_root);
        CHECK_EQ(decoded->font_assets[i].revision, bucket.font_assets[i].revision);
        CHECK_EQ(decoded->font_assets[i].fingerprint, bucket.font_assets[i].fingerprint);
        CHECK_EQ(decoded->font_assets[i].kind, bucket.font_assets[i].kind);
    }
}
