#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/BuildersDetail.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Theme.hpp>

namespace {

struct ThemeFixture {
    ThemeFixture() {
        auto launched = SP::System::LaunchStandard(space);
        REQUIRE(launched.has_value());
        auto app = SP::App::Create(space, "theme_app");
        REQUIRE(app.has_value());
        app_root = *app;
    }

    ~ThemeFixture() {
        SP::System::ShutdownDeclarativeRuntime(space);
    }

    auto app_root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    SP::PathSpace space;
    SP::App::AppRootPath app_root{std::string{}};
};

} // namespace

TEST_CASE("Theme::Create seeds tokens and value") {
    ThemeFixture fx;
    SP::UI::Declarative::Theme::CreateOptions options{};
    options.name = "Sunset";
    options.set_active = true;
    auto result = SP::UI::Declarative::Theme::Create(fx.space, fx.app_root_view(), options);
    REQUIRE(result.has_value());
    CHECK_EQ(result->canonical_name, "sunset");

    auto button_color = fx.space.read<std::array<float, 4>, std::string>(
        result->edit_root.getPath() + "/colors/button/background");
    REQUIRE(button_color.has_value());

    auto theme_paths =
        SP::UI::Builders::Config::Theme::Resolve(fx.app_root_view(), result->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled =
        fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
    REQUIRE(compiled.has_value());
    CHECK_EQ(compiled->button.background_color[0], doctest::Approx((*button_color)[0]));
}

TEST_CASE("Theme::SetColor updates storage and compiled value") {
    ThemeFixture fx;
    SP::UI::Declarative::Theme::CreateOptions options{};
    options.name = "Custom";
    auto result = SP::UI::Declarative::Theme::Create(fx.space, fx.app_root_view(), options);
    REQUIRE(result.has_value());

    SP::UI::Declarative::Theme::ColorValue magenta{};
    magenta.rgba = {1.0f, 0.0f, 1.0f, 1.0f};
    auto status = SP::UI::Declarative::Theme::SetColor(fx.space,
                                                       fx.app_root_view(),
                                                       result->canonical_name,
                                                       "button/background",
                                                       magenta);
    REQUIRE(status.has_value());

    auto stored = fx.space.read<std::array<float, 4>, std::string>(
        result->edit_root.getPath() + "/colors/button/background");
    REQUIRE(stored.has_value());
    CHECK_EQ((*stored)[0], doctest::Approx(1.0f));
    CHECK_EQ((*stored)[1], doctest::Approx(0.0f));

    auto theme_paths =
        SP::UI::Builders::Config::Theme::Resolve(fx.app_root_view(), result->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled =
        fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
    REQUIRE(compiled.has_value());
    CHECK_EQ(compiled->button.background_color[0], doctest::Approx(1.0f));
    CHECK_EQ(compiled->button.background_color[1], doctest::Approx(0.0f));

    SP::UI::Declarative::Theme::ColorValue readable{};
    readable.rgba = {0.20f, 0.24f, 0.30f, 1.0f};
    status = SP::UI::Declarative::Theme::SetColor(fx.space,
                                                  fx.app_root_view(),
                                                  result->canonical_name,
                                                  "palette/text_on_light",
                                                  readable);
    REQUIRE(status.has_value());
    auto palette_token = fx.space.read<std::array<float, 4>, std::string>(
        result->edit_root.getPath() + "/colors/palette/text_on_light");
    REQUIRE(palette_token.has_value());
    CHECK_EQ((*palette_token)[0], doctest::Approx(readable.rgba[0]));
    CHECK_EQ((*palette_token)[1], doctest::Approx(readable.rgba[1]));
    auto recompiled =
        fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
    REQUIRE(recompiled.has_value());
    CHECK_EQ(recompiled->palette_text_on_light[0], doctest::Approx(readable.rgba[0]));
    CHECK_EQ(recompiled->palette_text_on_light[1], doctest::Approx(readable.rgba[1]));
}

TEST_CASE("Theme::SetColor rejects unknown token") {
    ThemeFixture fx;
    SP::UI::Declarative::Theme::CreateOptions options{};
    options.name = "RejectToken";
    auto result = SP::UI::Declarative::Theme::Create(fx.space, fx.app_root_view(), options);
    REQUIRE(result.has_value());

    SP::UI::Declarative::Theme::ColorValue cyan{};
    cyan.rgba = {0.0f, 1.0f, 1.0f, 1.0f};
    auto status = SP::UI::Declarative::Theme::SetColor(fx.space,
                                                       fx.app_root_view(),
                                                       result->canonical_name,
                                                       "does/not/exist",
                                                       cyan);
    REQUIRE_FALSE(status.has_value());
    CHECK_EQ(status.error().code, SP::Error::Code::InvalidPath);
}

TEST_CASE("Theme::SetColor propagates through inherited themes until overridden") {
    ThemeFixture fx;
    auto app_view = fx.app_root_view();

    SP::UI::Declarative::Theme::CreateOptions base_options{};
    base_options.name = "BaseTheme";
    auto base = SP::UI::Declarative::Theme::Create(fx.space, app_view, base_options);
    REQUIRE(base.has_value());

    SP::UI::Declarative::Theme::ColorValue base_color{};
    base_color.rgba = {0.25f, 0.5f, 0.75f, 1.0f};
    REQUIRE(SP::UI::Declarative::Theme::SetColor(fx.space,
                                                 app_view,
                                                 base->canonical_name,
                                                 "button/background",
                                                 base_color)
                .has_value());

    SP::UI::Declarative::Theme::CreateOptions derived_options{};
    derived_options.name = "DerivedTheme";
    derived_options.inherits = base->canonical_name;
    auto derived = SP::UI::Declarative::Theme::Create(fx.space, app_view, derived_options);
    REQUIRE(derived.has_value());

    auto derived_paths = SP::UI::Builders::Config::Theme::Resolve(app_view, derived->canonical_name);
    REQUIRE(derived_paths.has_value());
    auto derived_theme = fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
        derived_paths->value.getPath());
    REQUIRE(derived_theme.has_value());
    CHECK_EQ((*derived_theme).button.background_color[0], doctest::Approx(base_color.rgba[0]));
    CHECK_EQ((*derived_theme).button.background_color[1], doctest::Approx(base_color.rgba[1]));
    CHECK_EQ((*derived_theme).button.background_color[2], doctest::Approx(base_color.rgba[2]));

    SP::UI::Declarative::Theme::ColorValue override_color{};
    override_color.rgba = {0.9f, 0.1f, 0.4f, 1.0f};
    REQUIRE(SP::UI::Declarative::Theme::SetColor(fx.space,
                                                 app_view,
                                                 derived->canonical_name,
                                                 "button/background",
                                                 override_color)
                .has_value());

    auto updated_child = fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
        derived_paths->value.getPath());
    REQUIRE(updated_child.has_value());
    CHECK_EQ(updated_child->button.background_color[0], doctest::Approx(override_color.rgba[0]));
    CHECK_EQ(updated_child->button.background_color[1], doctest::Approx(override_color.rgba[1]));
    CHECK_EQ(updated_child->button.background_color[2], doctest::Approx(override_color.rgba[2]));

    auto parent_paths = SP::UI::Builders::Config::Theme::Resolve(app_view, base->canonical_name);
    REQUIRE(parent_paths.has_value());
    auto parent_theme = fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
        parent_paths->value.getPath());
    REQUIRE(parent_theme.has_value());
    CHECK_EQ(parent_theme->button.background_color[0], doctest::Approx(base_color.rgba[0]));
    CHECK_EQ(parent_theme->button.background_color[1], doctest::Approx(base_color.rgba[1]));
    CHECK_EQ(parent_theme->button.background_color[2], doctest::Approx(base_color.rgba[2]));
}

TEST_CASE("Theme::RebuildValue replays manual color edits") {
    ThemeFixture fx;
    auto app_view = fx.app_root_view();

    SP::UI::Declarative::Theme::CreateOptions create_options{};
    create_options.name = "ManualTheme";
    auto created = SP::UI::Declarative::Theme::Create(fx.space, app_view, create_options);
    REQUIRE(created.has_value());

    SP::UI::Declarative::Theme::ColorValue seed{};
    seed.rgba = {0.1f, 0.2f, 0.3f, 1.0f};
    REQUIRE(SP::UI::Declarative::Theme::SetColor(fx.space,
                                                 app_view,
                                                 created->canonical_name,
                                                 "button/background",
                                                 seed)
                .has_value());

    auto edit_root = SP::App::resolve_app_relative(app_view,
                                                   std::string{"themes/"} + created->canonical_name);
    REQUIRE(edit_root.has_value());
    std::array<float, 4> manual_override{0.8f, 0.2f, 0.6f, 1.0f};
    REQUIRE(SP::UI::Builders::Detail::replace_single(fx.space,
                                                     edit_root->getPath()
                                                         + "/colors/button/background",
                                                     manual_override)
                .has_value());

    REQUIRE(SP::UI::Declarative::Theme::RebuildValue(fx.space,
                                                     app_view,
                                                     created->canonical_name)
                .has_value());

    auto theme_paths = SP::UI::Builders::Config::Theme::Resolve(app_view, created->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled = fx.space.read<SP::UI::Builders::Widgets::WidgetTheme, std::string>(
        theme_paths->value.getPath());
    REQUIRE(compiled.has_value());
    CHECK_EQ(compiled->button.background_color[0], doctest::Approx(manual_override[0]));
    CHECK_EQ(compiled->button.background_color[1], doctest::Approx(manual_override[1]));
    CHECK_EQ(compiled->button.background_color[2], doctest::Approx(manual_override[2]));
}
