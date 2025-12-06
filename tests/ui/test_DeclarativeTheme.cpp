#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Detail.hpp>
#include <pathspace/ui/declarative/Runtime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/declarative/Theme.hpp>
#include <pathspace/ui/declarative/ThemeConfig.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

namespace ThemeConfig = SP::UI::Declarative::ThemeConfig;
namespace Declarative = SP::UI::Declarative;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
namespace DetailNS = SP::UI::Declarative::Detail;

namespace {

inline auto widget_space(std::string const& root, std::string_view relative) -> std::string {
    return BuilderWidgets::WidgetSpacePath(root, relative);
}

inline auto widget_space(SP::UI::Runtime::WidgetPath const& widget,
                        std::string_view relative) -> std::string {
    return BuilderWidgets::WidgetSpacePath(widget.getPath(), relative);
}

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

auto LoadCompiledTheme(SP::PathSpace& space, SP::App::AppRootPathView app_root)
    -> BuilderWidgets::WidgetTheme {
    auto active = ThemeConfig::LoadActive(space, app_root);
    std::string name;
    if (active) {
        name = *active;
    } else {
        auto const& error = active.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            auto message = error.message.value_or("LoadActive failed");
            FAIL_CHECK(message.c_str());
        }
    }
    if (name.empty()) {
        auto system_theme = ThemeConfig::LoadSystemActive(space);
        REQUIRE(system_theme.has_value());
        name = *system_theme;
    }
    auto sanitized = ThemeConfig::SanitizeName(name);
    auto resolved = ThemeConfig::Resolve(app_root, sanitized);
    REQUIRE(resolved.has_value());
    auto compiled =
        space.read<BuilderWidgets::WidgetTheme, std::string>(resolved->value.getPath());
    REQUIRE(compiled.has_value());
    return *compiled;
}

struct DeclarativeThemeFixture {
    DeclarativeThemeFixture() {
        auto launched = SP::System::LaunchStandard(space);
        REQUIRE(launched.has_value());
        auto app = SP::App::Create(space, "descriptor_theme_app");
        REQUIRE(app.has_value());
        app_root = *app;
        SP::Window::CreateOptions options{};
        options.name = "descriptor_window";
        options.title = "Descriptor Window";
        auto window = SP::Window::Create(space, app_root, options);
        REQUIRE(window.has_value());
        window_path = window->path;
    }

    ~DeclarativeThemeFixture() {
        SP::System::ShutdownDeclarativeRuntime(space);
    }

    auto app_root_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    auto parent_view() const -> SP::App::ConcretePathView {
        return SP::App::ConcretePathView{window_path.getPath()};
    }

    SP::PathSpace space;
    SP::App::AppRootPath app_root{std::string{}};
    SP::UI::WindowPath window_path{std::string{}};
};

} // namespace

auto MakeTreeNodes() -> std::vector<BuilderWidgets::TreeNode> {
    std::vector<BuilderWidgets::TreeNode> nodes;
    nodes.push_back({
        .id = "root",
        .parent_id = "",
        .label = "Root",
        .enabled = true,
        .expandable = true,
        .loaded = true,
    });
    nodes.push_back({
        .id = "child",
        .parent_id = "root",
        .label = "Child",
        .enabled = true,
        .expandable = false,
        .loaded = true,
    });
    return nodes;
}

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
        ThemeConfig::Resolve(fx.app_root_view(), result->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled =
        fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
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
        ThemeConfig::Resolve(fx.app_root_view(), result->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled =
        fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
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
        fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
    REQUIRE(recompiled.has_value());
    CHECK_EQ(recompiled->palette_text_on_light[0], doctest::Approx(readable.rgba[0]));
    CHECK_EQ(recompiled->palette_text_on_light[1], doctest::Approx(readable.rgba[1]));

    SP::UI::Declarative::Theme::ColorValue swatch{};
    swatch.rgba = {0.12f, 0.88f, 0.65f, 1.0f};
    status = SP::UI::Declarative::Theme::SetColor(fx.space,
                                                  fx.app_root_view(),
                                                  result->canonical_name,
                                                  "palette/swatches/green",
                                                  swatch);
    REQUIRE(status.has_value());
    auto swatch_token = fx.space.read<std::array<float, 4>, std::string>(
        result->edit_root.getPath() + "/colors/palette/swatches/green");
    REQUIRE(swatch_token.has_value());
    CHECK_EQ((*swatch_token)[0], doctest::Approx(swatch.rgba[0]));
    CHECK_EQ((*swatch_token)[1], doctest::Approx(swatch.rgba[1]));
    auto recompiled_swatch =
        fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(theme_paths->value.getPath());
    REQUIRE(recompiled_swatch.has_value());
    CHECK_EQ(recompiled_swatch->palette_swatches[3][0], doctest::Approx(swatch.rgba[0]));
    CHECK_EQ(recompiled_swatch->palette_swatches[3][1], doctest::Approx(swatch.rgba[1]));
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

    auto derived_paths = ThemeConfig::Resolve(app_view, derived->canonical_name);
    REQUIRE(derived_paths.has_value());
    auto derived_theme = fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(
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

    auto updated_child = fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(
        derived_paths->value.getPath());
    REQUIRE(updated_child.has_value());
    CHECK_EQ(updated_child->button.background_color[0], doctest::Approx(override_color.rgba[0]));
    CHECK_EQ(updated_child->button.background_color[1], doctest::Approx(override_color.rgba[1]));
    CHECK_EQ(updated_child->button.background_color[2], doctest::Approx(override_color.rgba[2]));

    auto parent_paths = ThemeConfig::Resolve(app_view, base->canonical_name);
    REQUIRE(parent_paths.has_value());
    auto parent_theme = fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(
        parent_paths->value.getPath());
    REQUIRE(parent_theme.has_value());
    CHECK_EQ(parent_theme->button.background_color[0], doctest::Approx(base_color.rgba[0]));
    CHECK_EQ(parent_theme->button.background_color[1], doctest::Approx(base_color.rgba[1]));
    CHECK_EQ(parent_theme->button.background_color[2], doctest::Approx(base_color.rgba[2]));
}

TEST_CASE("Button descriptor inherits active theme colors when no overrides are serialized") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());
    auto button = Declarative::Button::Create(fx.space, fx.parent_view(), "theme_button");
    REQUIRE(button.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *button);
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->kind == Declarative::WidgetKind::Button);
    auto const& data = std::get<Declarative::ButtonDescriptor>(descriptor->data);

    CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.button.background_color[0]));
    CHECK_EQ(data.style.text_color[0], doctest::Approx(theme.button.text_color[0]));
    CHECK_EQ(data.style.typography.font_size,
             doctest::Approx(theme.button.typography.font_size));
}

TEST_CASE("Button descriptor preserves explicit style overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());
    BuilderWidgets::ButtonStyle custom{};
    custom.background_color = {0.85f, 0.25f, 0.42f, 1.0f};
    Declarative::Button::Args args{};
    args.style = custom;
    auto button =
        Declarative::Button::Create(fx.space, fx.parent_view(), "button_override", std::move(args));
    REQUIRE(button.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *button);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::ButtonDescriptor>(descriptor->data);

    CHECK_EQ(data.style.background_color[0], doctest::Approx(custom.background_color[0]));
    CHECK_EQ(data.style.background_color[1], doctest::Approx(custom.background_color[1]));
    CHECK_EQ(data.style.text_color[0], doctest::Approx(theme.button.text_color[0]));
}

TEST_CASE("Button descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::Button::Args args{};
    args.style.width = 280.0f;
    args.style.height = 60.0f;
    args.style.corner_radius = 10.0f;
    auto button =
        Declarative::Button::Create(fx.space, fx.parent_view(), "button_layout", std::move(args));
    REQUIRE(button.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *button);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::ButtonDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(280.0f));
    CHECK_EQ(data.style.height, doctest::Approx(60.0f));
    CHECK_EQ(data.style.corner_radius, doctest::Approx(10.0f));
    CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.button.background_color[0]));
    CHECK_EQ(data.style.text_color[0], doctest::Approx(theme.button.text_color[0]));
}

TEST_CASE("List descriptor layers theme defaults with serialized overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    auto make_items = [] {
        std::vector<BuilderWidgets::ListItem> items;
        items.push_back({"row_0", "Row 0"});
        items.push_back({"row_1", "Row 1"});
        return items;
    };

    SUBCASE("Defaults inherit active theme colors") {
        Declarative::List::Args args{};
        args.items = make_items();
        auto list =
            Declarative::List::Create(fx.space, fx.parent_view(), "list_theme", std::move(args));
        REQUIRE(list.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *list);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::ListDescriptor>(descriptor->data);

        CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.list.background_color[0]));
        CHECK_EQ(data.style.item_text_color[0], doctest::Approx(theme.list.item_text_color[0]));
    }

    SUBCASE("Overrides win for explicit fields") {
        Declarative::List::Args args{};
        args.items = make_items();
        args.style_override().item_text({0.12f, 0.94f, 0.78f, 1.0f});
        auto list = Declarative::List::Create(fx.space,
                                              fx.parent_view(),
                                              "list_override",
                                              std::move(args));
        REQUIRE(list.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *list);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::ListDescriptor>(descriptor->data);

        CHECK_EQ(data.style.item_text_color[1], doctest::Approx(0.94f));
        CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.list.background_color[0]));
    }
}

TEST_CASE("List descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    auto make_items = [] {
        std::vector<BuilderWidgets::ListItem> items;
        items.push_back({"row_0", "Row 0"});
        items.push_back({"row_1", "Row 1"});
        return items;
    };

    Declarative::List::Args args{};
    args.items = make_items();
    args.style.width = 320.0f;
    args.style.item_height = 44.0f;
    args.style.corner_radius = 12.0f;
    args.style.border_thickness = 2.0f;
    auto list = Declarative::List::Create(fx.space, fx.parent_view(), "list_layout", std::move(args));
    REQUIRE(list.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *list);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::ListDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(320.0f));
    CHECK_EQ(data.style.item_height, doctest::Approx(44.0f));
    CHECK_EQ(data.style.corner_radius, doctest::Approx(12.0f));
    CHECK_EQ(data.style.border_thickness, doctest::Approx(2.0f));
    CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.list.background_color[0]));
    CHECK_EQ(data.style.item_text_color[0], doctest::Approx(theme.list.item_text_color[0]));
}

TEST_CASE("Slider descriptor layers theme defaults and explicit overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    SUBCASE("Defaults inherit the active theme colors") {
        Declarative::Slider::Args args{};
        args.minimum = 0.0f;
        args.maximum = 10.0f;
        args.value = 4.0f;
        auto slider = Declarative::Slider::Create(fx.space, fx.parent_view(), "slider_theme", args);
        REQUIRE(slider.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *slider);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::SliderDescriptor>(descriptor->data);
        CHECK_EQ(data.style.track_color[0], doctest::Approx(theme.slider.track_color[0]));
        CHECK_EQ(data.style.fill_color[1], doctest::Approx(theme.slider.fill_color[1]));
        CHECK_EQ(data.style.thumb_color[2], doctest::Approx(theme.slider.thumb_color[2]));
        CHECK_EQ(data.style.label_color[3], doctest::Approx(theme.slider.label_color[3]));
    }

    SUBCASE("Overrides win for targeted slider fields") {
        Declarative::Slider::Args args{};
        args.minimum = 0.0f;
        args.maximum = 1.0f;
        args.value = 0.25f;
        args.style_override()
            .fill_color({0.22f, 0.54f, 0.81f, 1.0f})
            .thumb_color({0.91f, 0.72f, 0.11f, 1.0f});
        auto slider = Declarative::Slider::Create(fx.space, fx.parent_view(), "slider_override", args);
        REQUIRE(slider.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *slider);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::SliderDescriptor>(descriptor->data);
        CHECK_EQ(data.style.fill_color[0], doctest::Approx(0.22f));
        CHECK_EQ(data.style.fill_color[1], doctest::Approx(0.54f));
        CHECK_EQ(data.style.thumb_color[0], doctest::Approx(0.91f));
        CHECK_EQ(data.style.thumb_color[2], doctest::Approx(0.11f));
        CHECK_EQ(data.style.track_color[0], doctest::Approx(theme.slider.track_color[0]));
    }
}

TEST_CASE("Slider descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::Slider::Args args{};
    args.minimum = 0.0f;
    args.maximum = 5.0f;
    args.value = 1.0f;
    args.style.width = 360.0f;
    args.style.height = 44.0f;
    args.style.track_height = 8.0f;
    args.style.thumb_radius = 14.0f;
    auto slider = Declarative::Slider::Create(fx.space, fx.parent_view(), "slider_layout", args);
    REQUIRE(slider.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *slider);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::SliderDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(args.style.width));
    CHECK_EQ(data.style.height, doctest::Approx(args.style.height));
    CHECK_EQ(data.style.track_height, doctest::Approx(args.style.track_height));
    CHECK_EQ(data.style.thumb_radius, doctest::Approx(args.style.thumb_radius));
    CHECK_EQ(data.style.track_color[0], doctest::Approx(theme.slider.track_color[0]));
    CHECK_EQ(data.style.fill_color[0], doctest::Approx(theme.slider.fill_color[0]));
}

TEST_CASE("Toggle descriptor layers theme defaults and overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    SUBCASE("Defaults reuse the active theme palette") {
        Declarative::Toggle::Args args{};
        auto toggle = Declarative::Toggle::Create(fx.space, fx.parent_view(), "toggle_theme", args);
        REQUIRE(toggle.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *toggle);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::ToggleDescriptor>(descriptor->data);
        CHECK_EQ(data.style.track_off_color[0], doctest::Approx(theme.toggle.track_off_color[0]));
        CHECK_EQ(data.style.track_on_color[1], doctest::Approx(theme.toggle.track_on_color[1]));
        CHECK_EQ(data.style.thumb_color[2], doctest::Approx(theme.toggle.thumb_color[2]));
    }

    SUBCASE("Explicit overrides win for toggle colors") {
        Declarative::Toggle::Args args{};
        args.style_override()
            .track_off({0.18f, 0.22f, 0.28f, 1.0f})
            .track_on({0.35f, 0.82f, 0.44f, 1.0f})
            .thumb({0.92f, 0.92f, 0.92f, 1.0f});
        auto toggle = Declarative::Toggle::Create(fx.space, fx.parent_view(), "toggle_override", args);
        REQUIRE(toggle.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *toggle);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::ToggleDescriptor>(descriptor->data);
        CHECK_EQ(data.style.track_off_color[0], doctest::Approx(0.18f));
        CHECK_EQ(data.style.track_on_color[1], doctest::Approx(0.82f));
        CHECK_EQ(data.style.thumb_color[0], doctest::Approx(0.92f));
        CHECK_EQ(data.style.track_off_color[2], doctest::Approx(0.28f));
    }
}

TEST_CASE("Toggle descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::Toggle::Args args{};
    args.style.width = 72.0f;
    args.style.height = 36.0f;
    auto toggle = Declarative::Toggle::Create(fx.space, fx.parent_view(), "toggle_layout", args);
    REQUIRE(toggle.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *toggle);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::ToggleDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(args.style.width));
    CHECK_EQ(data.style.height, doctest::Approx(args.style.height));
    CHECK_EQ(data.style.track_on_color[0], doctest::Approx(theme.toggle.track_on_color[0]));
    CHECK_EQ(data.style.track_off_color[0], doctest::Approx(theme.toggle.track_off_color[0]));
}

TEST_CASE("Tree descriptor layers theme defaults and honors style overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    SUBCASE("Defaults reuse the active tree theme") {
        Declarative::Tree::Args args{};
        args.nodes = MakeTreeNodes();
        auto tree = Declarative::Tree::Create(fx.space, fx.parent_view(), "tree_theme", args);
        REQUIRE(tree.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *tree);
        REQUIRE(descriptor.has_value());
        REQUIRE(descriptor->kind == Declarative::WidgetKind::Tree);
        auto const& data = std::get<Declarative::TreeDescriptor>(descriptor->data);

        CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.tree.background_color[0]));
        CHECK_EQ(data.style.row_color[1], doctest::Approx(theme.tree.row_color[1]));
        CHECK_EQ(data.style.text_color[2], doctest::Approx(theme.tree.text_color[2]));
        CHECK_EQ(data.nodes.size(), MakeTreeNodes().size());
    }

    SUBCASE("Explicit overrides win for targeted tree fields") {
        Declarative::Tree::Args args{};
        args.nodes = MakeTreeNodes();
        args.style_override()
            .row({0.12f, 0.32f, 0.44f, 1.0f})
            .text({0.88f, 0.91f, 0.96f, 1.0f});
        auto tree = Declarative::Tree::Create(fx.space,
                                              fx.parent_view(),
                                              "tree_override",
                                              args);
        REQUIRE(tree.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *tree);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::TreeDescriptor>(descriptor->data);

        CHECK_EQ(data.style.row_color[0], doctest::Approx(0.12f));
        CHECK_EQ(data.style.row_color[1], doctest::Approx(0.32f));
        CHECK_EQ(data.style.text_color[0], doctest::Approx(0.88f));
        CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.tree.background_color[0]));
    }

    SUBCASE("style_override sets mask bits even when values match the theme") {
        Declarative::Tree::Args args{};
        args.nodes = MakeTreeNodes();
        auto theme_row = theme.tree.row_color;
        args.style_override().row(theme_row);
        auto tree = Declarative::Tree::Create(fx.space, fx.parent_view(), "tree_mask", args);
        REQUIRE(tree.has_value());

        auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *tree);
        REQUIRE(descriptor.has_value());
        auto const& data = std::get<Declarative::TreeDescriptor>(descriptor->data);

        CHECK_EQ(data.style.row_color[0], doctest::Approx(theme_row[0]));
        CHECK(BuilderWidgets::HasStyleOverride(data.style.overrides,
                                               BuilderWidgets::TreeStyleOverrideField::Row));
    }
}

TEST_CASE("Tree descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::Tree::Args args{};
    args.nodes = MakeTreeNodes();
    args.style.width = 360.0f;
    args.style.row_height = 40.0f;
    args.style.corner_radius = 10.0f;
    args.style.border_thickness = 2.0f;
    args.style.indent_per_level = 22.0f;
    args.style.toggle_icon_size = 14.0f;
    auto tree = Declarative::Tree::Create(fx.space, fx.parent_view(), "tree_layout", args);
    REQUIRE(tree.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *tree);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::TreeDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(args.style.width));
    CHECK_EQ(data.style.row_height, doctest::Approx(args.style.row_height));
    CHECK_EQ(data.style.corner_radius, doctest::Approx(args.style.corner_radius));
    CHECK_EQ(data.style.border_thickness, doctest::Approx(args.style.border_thickness));
    CHECK_EQ(data.style.indent_per_level, doctest::Approx(args.style.indent_per_level));
    CHECK_EQ(data.style.toggle_icon_size, doctest::Approx(args.style.toggle_icon_size));
    CHECK_EQ(data.style.background_color[0], doctest::Approx(theme.tree.background_color[0]));
    CHECK_EQ(data.style.text_color[0], doctest::Approx(theme.tree.text_color[0]));
}

TEST_CASE("InputField descriptor inherits text field theme colors by default") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::InputField::Args args{};
    args.text = "Theme aware";
    auto input = Declarative::InputField::Create(fx.space,
                                                fx.parent_view(),
                                                "input_theme",
                                                std::move(args));
    REQUIRE(input.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *input);
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->kind == Declarative::WidgetKind::InputField);
    auto const& data = std::get<Declarative::InputFieldDescriptor>(descriptor->data);

    CHECK_EQ(data.style.background_color[0],
             doctest::Approx(theme.text_field.background_color[0]));
    CHECK_EQ(data.style.text_color[0], doctest::Approx(theme.text_field.text_color[0]));
    CHECK_EQ(data.style.placeholder_color[0],
             doctest::Approx(theme.text_field.placeholder_color[0]));
}

TEST_CASE("InputField descriptor preserves explicit text color overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::InputField::Args args{};
    args.text = "Override";
    auto input = Declarative::InputField::Create(fx.space,
                                                fx.parent_view(),
                                                "input_override",
                                                std::move(args));
    REQUIRE(input.has_value());

    BuilderWidgets::TextFieldStyle custom = theme.text_field;
    custom.text_color = {0.25f, 0.73f, 0.52f, 1.0f};
    BuilderWidgets::UpdateOverrides(custom);
    auto style_path = widget_space(*input, "/meta/style");
    auto replaced = DetailNS::replace_single(fx.space, style_path, custom);
    REQUIRE(replaced.has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *input);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::InputFieldDescriptor>(descriptor->data);

    CHECK_EQ(data.style.text_color[0], doctest::Approx(custom.text_color[0]));
    CHECK_EQ(data.style.text_color[1], doctest::Approx(custom.text_color[1]));
    CHECK_EQ(data.style.placeholder_color[0],
             doctest::Approx(theme.text_field.placeholder_color[0]));
    CHECK_EQ(data.style.background_color[0],
             doctest::Approx(theme.text_field.background_color[0]));
}

TEST_CASE("InputField descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    Declarative::InputField::Args args{};
    args.text = "Layout overrides";
    auto input = Declarative::InputField::Create(fx.space,
                                                fx.parent_view(),
                                                "input_layout",
                                                std::move(args));
    REQUIRE(input.has_value());

    BuilderWidgets::TextFieldStyle custom = theme.text_field;
    custom.width = 420.0f;
    custom.height = 60.0f;
    custom.padding_x = 20.0f;
    custom.padding_y = 14.0f;
    custom.corner_radius = 10.0f;
    BuilderWidgets::UpdateOverrides(custom);
    auto style_path = widget_space(*input, "/meta/style");
    REQUIRE(DetailNS::replace_single(fx.space, style_path, custom).has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, *input);
    REQUIRE(descriptor.has_value());
    auto const& data = std::get<Declarative::InputFieldDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(custom.width));
    CHECK_EQ(data.style.height, doctest::Approx(custom.height));
    CHECK_EQ(data.style.padding_x, doctest::Approx(custom.padding_x));
    CHECK_EQ(data.style.padding_y, doctest::Approx(custom.padding_y));
    CHECK_EQ(data.style.corner_radius, doctest::Approx(custom.corner_radius));
    CHECK_EQ(data.style.background_color[0],
             doctest::Approx(theme.text_field.background_color[0]));
}

TEST_CASE("TextArea descriptor inherits theme colors by default") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    auto widget_root = std::string(fx.parent_view().getPath()) + "/widgets/text_area_theme";
    auto widget = SP::UI::Runtime::WidgetPath{widget_root};

    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/meta/kind", std::string{"text_area"})
                .has_value());
    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/state/text", std::string{"Multiline"})
                .has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     widget_root + "/state/placeholder",
                                     std::string{"Placeholder"})
                .has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, widget);
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->kind == Declarative::WidgetKind::TextArea);
    auto const& data = std::get<Declarative::TextAreaDescriptor>(descriptor->data);

    CHECK_EQ(data.style.background_color[0],
             doctest::Approx(theme.text_area.background_color[0]));
    CHECK_EQ(data.style.text_color[1], doctest::Approx(theme.text_area.text_color[1]));
    CHECK_EQ(data.style.caret_color[2], doctest::Approx(theme.text_area.caret_color[2]));
    CHECK_EQ(data.state.placeholder, "Placeholder");
}

TEST_CASE("TextArea descriptor preserves explicit overrides") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    auto widget_root = std::string(fx.parent_view().getPath()) + "/widgets/text_area_override";
    auto widget = SP::UI::Runtime::WidgetPath{widget_root};

    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/meta/kind", std::string{"text_area"})
                .has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     widget_root + "/state/text",
                                     std::string{"Overrides matter"})
                .has_value());

    BuilderWidgets::TextAreaStyle custom = theme.text_area;
    custom.background_color = {0.15f, 0.35f, 0.55f, 1.0f};
    custom.text_color = {0.92f, 0.85f, 0.12f, 1.0f};
    BuilderWidgets::UpdateOverrides(custom);
    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/meta/style", custom).has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, widget);
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->kind == Declarative::WidgetKind::TextArea);
    auto const& data = std::get<Declarative::TextAreaDescriptor>(descriptor->data);

    CHECK_EQ(data.style.background_color[0], doctest::Approx(custom.background_color[0]));
    CHECK_EQ(data.style.background_color[2], doctest::Approx(custom.background_color[2]));
    CHECK_EQ(data.style.text_color[1], doctest::Approx(custom.text_color[1]));
    CHECK_EQ(data.style.placeholder_color[0],
             doctest::Approx(theme.text_area.placeholder_color[0]));
    CHECK_EQ(data.style.selection_color[2],
             doctest::Approx(theme.text_area.selection_color[2]));
}

TEST_CASE("TextArea descriptor preserves layout overrides while layering theme colors") {
    DeclarativeThemeFixture fx;
    auto theme = LoadCompiledTheme(fx.space, fx.app_root_view());

    auto widget_root = std::string(fx.parent_view().getPath()) + "/widgets/text_area_layout";
    auto widget = SP::UI::Runtime::WidgetPath{widget_root};

    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/meta/kind", std::string{"text_area"})
                .has_value());
    REQUIRE(DetailNS::replace_single(fx.space,
                                     widget_root + "/state/text",
                                     std::string{"Layout data"})
                .has_value());

    BuilderWidgets::TextAreaStyle custom = theme.text_area;
    custom.width = 640.0f;
    custom.height = 280.0f;
    custom.padding_x = 24.0f;
    custom.padding_y = 18.0f;
    custom.min_height = 260.0f;
    custom.line_spacing = 8.0f;
    custom.wrap_lines = false;
    BuilderWidgets::UpdateOverrides(custom);
    REQUIRE(DetailNS::replace_single(fx.space, widget_root + "/meta/style", custom).has_value());

    auto descriptor = Declarative::LoadWidgetDescriptor(fx.space, widget);
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->kind == Declarative::WidgetKind::TextArea);
    auto const& data = std::get<Declarative::TextAreaDescriptor>(descriptor->data);

    CHECK_EQ(data.style.width, doctest::Approx(custom.width));
    CHECK_EQ(data.style.height, doctest::Approx(custom.height));
    CHECK_EQ(data.style.padding_x, doctest::Approx(custom.padding_x));
    CHECK_EQ(data.style.padding_y, doctest::Approx(custom.padding_y));
    CHECK_EQ(data.style.min_height, doctest::Approx(custom.min_height));
    CHECK_EQ(data.style.line_spacing, doctest::Approx(custom.line_spacing));
    CHECK_FALSE(data.style.wrap_lines);
    CHECK_EQ(data.style.background_color[1],
             doctest::Approx(theme.text_area.background_color[1]));
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
    REQUIRE(SP::UI::Declarative::Detail::replace_single(fx.space,
                                                     edit_root->getPath()
                                                         + "/colors/button/background",
                                                     manual_override)
                .has_value());

    REQUIRE(SP::UI::Declarative::Theme::RebuildValue(fx.space,
                                                     app_view,
                                                     created->canonical_name)
                .has_value());

    auto theme_paths = ThemeConfig::Resolve(app_view, created->canonical_name);
    REQUIRE(theme_paths.has_value());
    auto compiled = fx.space.read<SP::UI::Runtime::Widgets::WidgetTheme, std::string>(
        theme_paths->value.getPath());
    REQUIRE(compiled.has_value());
    CHECK_EQ(compiled->button.background_color[0], doctest::Approx(manual_override[0]));
    CHECK_EQ(compiled->button.background_color[1], doctest::Approx(manual_override[1]));
    CHECK_EQ(compiled->button.background_color[2], doctest::Approx(manual_override[2]));
}
