#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/FontManager.hpp>

#include <cstdint>

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
}
