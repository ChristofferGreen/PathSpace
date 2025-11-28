#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/FontAtlasCache.hpp>
#include <pathspace/ui/FontManager.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace SP;
using namespace SP::UI;

namespace {

auto format_revision(std::uint64_t revision) -> std::string {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llu", static_cast<unsigned long long>(revision));
    return std::string(buffer);
}

}

TEST_CASE("Font resources resolve canonical paths") {
    using namespace SP::UI::Runtime::Resources::Fonts;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    auto paths = Resolve(app_view, "DisplaySans", "Regular");
    REQUIRE(paths);

    CHECK(paths->root.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular");
    CHECK(paths->meta.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/meta");
    CHECK(paths->active_revision.getPath()
          == "/system/applications/demo_app/resources/fonts/DisplaySans/Regular/meta/active_revision");
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
    SP::UI::Runtime::Resources::Fonts::RegisterFontParams params{
        .family = "DisplaySans",
        .style = "Regular",
        .weight = "450",
        .fallback_families = {"system-ui", "serif"},
        .initial_revision = 4ull,
        .atlas_soft_bytes = 6ull * 1024ull * 1024ull,
        .atlas_hard_bytes = 12ull * 1024ull * 1024ull,
        .shaped_run_approx_bytes = 1024ull,
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

    auto weight_path = base + "/meta/weight";
    auto stored_weight = space.read<std::string, std::string>(weight_path);
    REQUIRE(stored_weight);
    CHECK(*stored_weight == "450");

    auto fallback_path = base + "/meta/fallbacks";
    auto stored_fallbacks = space.read<std::vector<std::string>, std::string>(fallback_path);
    REQUIRE(stored_fallbacks);
    std::vector<std::string> expected_fallbacks{"system-ui", "serif"};
    CHECK(*stored_fallbacks == expected_fallbacks);

    auto atlas_soft = space.read<std::uint64_t, std::string>(base + "/meta/atlas/softBytes");
    REQUIRE(atlas_soft);
    CHECK(*atlas_soft == params.atlas_soft_bytes);

    auto atlas_hard = space.read<std::uint64_t, std::string>(base + "/meta/atlas/hardBytes");
    REQUIRE(atlas_hard);
    CHECK(*atlas_hard == params.atlas_hard_bytes);

    auto approx_bytes = space.read<std::uint64_t, std::string>(base + "/meta/atlas/shapedRunApproxBytes");
    REQUIRE(approx_bytes);
    CHECK(*approx_bytes == params.shaped_run_approx_bytes);

    auto active = space.read<std::uint64_t, std::string>(registered->active_revision.getPath());
    REQUIRE(active);
    CHECK(*active == params.initial_revision);

    auto metrics_base = std::string(app_view.getPath()) + "/diagnostics/metrics/fonts";
    auto registered_fonts_path = metrics_base + "/registeredFonts";
   auto registered_count = space.read<std::uint64_t, std::string>(registered_fonts_path);
   REQUIRE(registered_count);
   CHECK(*registered_count >= 1);

    auto revision_base = base + "/builds/" + format_revision(params.initial_revision);
    auto atlas_path = revision_base + "/atlas.bin";
    auto atlas_bytes = space.read<std::vector<std::uint8_t>>(atlas_path);
    REQUIRE(atlas_bytes);

    FontAtlasCache cache;
    auto atlas_data = cache.load(space, atlas_path, params.initial_revision);
    REQUIRE(atlas_data);
    CHECK(!(*atlas_data)->glyphs.empty());
    CHECK((*atlas_data)->width > 0);
    CHECK((*atlas_data)->height > 0);

    auto atlas_meta = space.read<std::string, std::string>(revision_base + "/meta/atlas.json");
    REQUIRE(atlas_meta);
    CHECK_FALSE(atlas_meta->empty());
}

TEST_CASE("FontManager resolves manifest fallback chain") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    Runtime::Resources::Fonts::RegisterFontParams params{
        .family = "PathSpaceSans",
        .style = "Regular",
        .weight = "500",
        .fallback_families = {"system-ui", "PathSpaceSans", "system-ui", "monospace"},
        .initial_revision = 5ull,
    };

    auto registered = manager.register_font(app_view, params);
    REQUIRE(registered);

    auto resolved = manager.resolve_font(app_view, "PathSpaceSans", "Regular");
    REQUIRE(resolved);
    CHECK(resolved->family == "PathSpaceSans");
    CHECK(resolved->style == "Regular");
    CHECK(resolved->weight == "500");
    CHECK(resolved->active_revision == params.initial_revision);
    CHECK(resolved->paths.root.getPath() == registered->root.getPath());

    std::vector<std::string> expected_fallback{"system-ui", "monospace"};
    CHECK(resolved->fallback_chain == expected_fallback);
}

TEST_CASE("FontManager supplies default fallback when metadata omitted") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    Runtime::Resources::Fonts::RegisterFontParams params{
        .family = "DefaultedFont",
        .style = "Regular",
        .weight = "400",
        .fallback_families = {},
        .initial_revision = 1ull,
    };

    auto registered = manager.register_font(app_view, params);
    REQUIRE(registered);

    auto resolved = manager.resolve_font(app_view, "DefaultedFont", "Regular");
    REQUIRE(resolved);
    CHECK(resolved->fallback_chain.size() == 1);
    CHECK(resolved->fallback_chain.front() == "system-ui");
    CHECK(resolved->weight == "400");
}

TEST_CASE("FontManager caches shaped runs and updates metrics") {
    PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    FontManager manager(space);
    manager.set_cache_capacity_for_testing(8);

    Runtime::Widgets::TypographyStyle typography{};
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

    Runtime::Widgets::TypographyStyle typography{};
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
