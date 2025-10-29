#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/FontManager.hpp>

#include <cstdint>
#include <string>

using namespace SP;
using namespace SP::UI;

TEST_CASE("Font resources resolve canonical paths") {
    using namespace SP::UI::Builders::Resources::Fonts;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    auto paths = Resolve(app_view, "DisplaySans", "Regular");
    REQUIRE(paths);

    CHECK(paths->root.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular");
    CHECK(paths->manifest.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/manifest.json");
    CHECK(paths->active_revision.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/active");
    CHECK(paths->builds.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/builds");
    CHECK(paths->inbox.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/inbox");
}

TEST_CASE("FontManager registers font metadata and manifest") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    SP::UI::Builders::Resources::Fonts::RegisterFontParams params{
        .family = "DisplaySans",
        .style = "Regular",
        .manifest_json = std::string{R"({"family":"DisplaySans","style":"Regular"})"},
        .manifest_digest = std::string{"sha256:demo"},
        .initial_revision = 4ull,
    };

    auto registered = manager.register_font(app_view, params);
    REQUIRE(registered);

    auto base = registered->root.getPath();
    auto family_path = base + "/meta/family";
    auto style_path = base + "/meta/style";

    auto family = space.read<std::string, std::string>(family_path);
    REQUIRE(family);
    CHECK(*family == "DisplaySans");

    auto style = space.read<std::string, std::string>(style_path);
    REQUIRE(style);
    CHECK(*style == "Regular");

    auto active = space.read<std::uint64_t, std::string>(registered->active_revision.getPath());
    REQUIRE(active);
    CHECK(*active == params.initial_revision);

    auto digest_path = base + "/meta/manifest_digest";
    auto digest = space.read<std::string, std::string>(digest_path);
    REQUIRE(digest);
    CHECK(*digest == *params.manifest_digest);

    if (params.manifest_json) {
        auto manifest = space.read<std::string, std::string>(registered->manifest.getPath());
        REQUIRE(manifest);
        CHECK(*manifest == *params.manifest_json);
    }

    auto metrics_base = std::string(app_view.getPath()) + "/diagnostics/metrics/fonts";
    auto registered_fonts_path = metrics_base + "/registeredFonts";
    auto registered_count = space.read<std::uint64_t, std::string>(registered_fonts_path);
    REQUIRE(registered_count);
    CHECK(*registered_count >= 1);
}

TEST_CASE("FontManager caches shaped runs and updates metrics") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    manager.set_cache_capacity_for_testing(8);

    Builders::Widgets::TypographyStyle typography{};
    typography.font_family = "PathSpaceSans";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_size = 24.0f;
    typography.font_resource_root = "/system/applications/demo_app/resources/fonts/PathSpaceSans/Regular";
    typography.font_active_revision = 1;

    auto run_first = manager.shape_text(app_view, "Hello", typography);
    CHECK(run_first.glyphs.size() == 5);
    CHECK(run_first.total_advance > 0.0f);

    auto metrics_after_first = manager.metrics();
    CHECK(metrics_after_first.cache_misses == 1);
    CHECK(metrics_after_first.cache_hits == 0);
    CHECK(metrics_after_first.cache_size == 1);

    auto run_second = manager.shape_text(app_view, "Hello", typography);
    CHECK(run_second.glyphs.size() == 5);
    auto metrics_after_second = manager.metrics();
    CHECK(metrics_after_second.cache_hits == 1);
    CHECK(metrics_after_second.cache_misses == 1);

    auto metrics_base = std::string(app_view.getPath()) + "/diagnostics/metrics/fonts";
    auto cache_hits_path = metrics_base + "/cacheHits";
    auto cache_hits = space.read<std::uint64_t, std::string>(cache_hits_path);
    REQUIRE(cache_hits);
    CHECK(*cache_hits >= 1);
}

TEST_CASE("FontManager evicts least recently used cache entries") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    manager.set_cache_capacity_for_testing(2);

    Builders::Widgets::TypographyStyle typography{};
    typography.font_family = "PathSpaceSans";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_size = 18.0f;
    typography.font_resource_root = "/system/applications/demo_app/resources/fonts/PathSpaceSans/Regular";
    typography.font_active_revision = 3;

    (void)manager.shape_text(app_view, "Alpha", typography);
    (void)manager.shape_text(app_view, "Beta", typography);

    auto metrics_pre = manager.metrics();
    CHECK(metrics_pre.cache_size == 2);
    CHECK(metrics_pre.cache_evictions == 0);

    (void)manager.shape_text(app_view, "Gamma", typography);
    auto metrics_post = manager.metrics();
    CHECK(metrics_post.cache_size == 2);
    CHECK(metrics_post.cache_evictions >= 1);
}
