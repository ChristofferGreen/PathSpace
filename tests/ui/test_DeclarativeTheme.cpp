#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
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
