#include <pathspace/ui/declarative/Theme.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <string_view>
#include <optional>
#include <utility>

namespace SP::UI::Declarative::Theme {

namespace {

using SP::UI::Builders::Detail::replace_single;
using SP::UI::Builders::Detail::read_optional;
using SP::UI::Builders::Detail::make_error;
using Color = std::array<float, 4>;

template <typename T>
auto ensure_value(PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    auto existing = read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return replace_single<T>(space, path, value);
}

auto clamp_component(float value) -> float {
    if (std::isnan(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

auto sanitize_color(Color const& input) -> Color {
    Color clamped{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        clamped[i] = clamp_component(input[i]);
    }
    return clamped;
}

auto sanitize_component(std::string_view component) -> SP::Expected<std::string> {
    if (component.empty()) {
        return std::unexpected(make_error("theme token component must not be empty",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    std::string sanitized;
    sanitized.reserve(component.size());
    for (char ch : component) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            sanitized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            continue;
        }
        if (ch == '-' || ch == '_') {
            sanitized.push_back('_');
            continue;
        }
        return std::unexpected(make_error("theme token component must be alphanumeric, '-' or '_'",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    return sanitized;
}

auto normalize_token(std::string_view token) -> SP::Expected<std::string> {
    std::string normalized;
    normalized.reserve(token.size());
    bool expect_component = true;
    for (char ch : token) {
        if (ch == '/') {
            if (expect_component) {
                return std::unexpected(make_error("theme token must not contain empty components",
                                                  SP::Error::Code::InvalidPath));
            }
            normalized.push_back('/');
            expect_component = true;
            continue;
        }
        normalized.push_back(ch);
        expect_component = false;
    }
    if (normalized.empty() || normalized.front() == '/' || normalized.back() == '/') {
        return std::unexpected(make_error("theme token must not start or end with '/'",
                                          SP::Error::Code::InvalidPath));
    }

    std::string rebuilt;
    rebuilt.reserve(normalized.size());
    std::string current;
    for (char ch : normalized) {
        if (ch == '/') {
            auto sanitized = sanitize_component(current);
            if (!sanitized) {
                return sanitized;
            }
            if (!rebuilt.empty()) {
                rebuilt.push_back('/');
            }
            rebuilt.append(*sanitized);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    auto sanitized = sanitize_component(current);
    if (!sanitized) {
        return sanitized;
    }
    if (!rebuilt.empty()) {
        rebuilt.push_back('/');
    }
    rebuilt.append(*sanitized);
    return rebuilt;
}

auto theme_edit_root(SP::App::AppRootPathView app_root, std::string_view name) -> SP::Expected<SP::App::ConcretePath> {
    std::string relative{"themes/"};
    relative.append(name);
    return SP::App::resolve_app_relative(app_root, relative);
}

auto color_node_path(std::string const& root, std::string const& token) -> std::string {
    std::string path = root;
    path.append("/colors/");
    path.append(token);
    return path;
}

struct ColorBinding {
    std::string_view token;
    Color (*read)(WidgetTheme const&);
    void (*write)(WidgetTheme&, Color const&);
};

#define THEME_COLOR_BINDING(key, field)                                                               \
    ColorBinding {                                                                                    \
        key,                                                                                          \
            [](WidgetTheme const& theme) -> Color { return field; },                                  \
            [](WidgetTheme& theme, Color const& value) { field = value; },                            \
    }

constexpr ColorBinding kColorBindings[] = {
    THEME_COLOR_BINDING("button/background", theme.button.background_color),
    THEME_COLOR_BINDING("button/text", theme.button.text_color),
    THEME_COLOR_BINDING("toggle/track_off", theme.toggle.track_off_color),
    THEME_COLOR_BINDING("toggle/track_on", theme.toggle.track_on_color),
    THEME_COLOR_BINDING("toggle/thumb", theme.toggle.thumb_color),
    THEME_COLOR_BINDING("slider/track", theme.slider.track_color),
    THEME_COLOR_BINDING("slider/fill", theme.slider.fill_color),
    THEME_COLOR_BINDING("slider/thumb", theme.slider.thumb_color),
    THEME_COLOR_BINDING("slider/label", theme.slider.label_color),
    THEME_COLOR_BINDING("list/background", theme.list.background_color),
    THEME_COLOR_BINDING("list/border", theme.list.border_color),
    THEME_COLOR_BINDING("list/item", theme.list.item_color),
    THEME_COLOR_BINDING("list/item_hover", theme.list.item_hover_color),
    THEME_COLOR_BINDING("list/item_selected", theme.list.item_selected_color),
    THEME_COLOR_BINDING("list/separator", theme.list.separator_color),
    THEME_COLOR_BINDING("list/item_text", theme.list.item_text_color),
    THEME_COLOR_BINDING("tree/background", theme.tree.background_color),
    THEME_COLOR_BINDING("tree/border", theme.tree.border_color),
    THEME_COLOR_BINDING("tree/row", theme.tree.row_color),
    THEME_COLOR_BINDING("tree/row_hover", theme.tree.row_hover_color),
    THEME_COLOR_BINDING("tree/row_selected", theme.tree.row_selected_color),
    THEME_COLOR_BINDING("tree/row_disabled", theme.tree.row_disabled_color),
    THEME_COLOR_BINDING("tree/connector", theme.tree.connector_color),
    THEME_COLOR_BINDING("tree/toggle", theme.tree.toggle_color),
    THEME_COLOR_BINDING("tree/text", theme.tree.text_color),
    THEME_COLOR_BINDING("text_field/background", theme.text_field.background_color),
    THEME_COLOR_BINDING("text_field/border", theme.text_field.border_color),
    THEME_COLOR_BINDING("text_field/text", theme.text_field.text_color),
    THEME_COLOR_BINDING("text_field/placeholder", theme.text_field.placeholder_color),
    THEME_COLOR_BINDING("text_field/selection", theme.text_field.selection_color),
    THEME_COLOR_BINDING("text_field/composition", theme.text_field.composition_color),
    THEME_COLOR_BINDING("text_field/caret", theme.text_field.caret_color),
    THEME_COLOR_BINDING("text_area/background", theme.text_area.background_color),
    THEME_COLOR_BINDING("text_area/border", theme.text_area.border_color),
    THEME_COLOR_BINDING("text_area/text", theme.text_area.text_color),
    THEME_COLOR_BINDING("text_area/placeholder", theme.text_area.placeholder_color),
    THEME_COLOR_BINDING("text_area/selection", theme.text_area.selection_color),
    THEME_COLOR_BINDING("text_area/composition", theme.text_area.composition_color),
    THEME_COLOR_BINDING("text_area/caret", theme.text_area.caret_color),
    THEME_COLOR_BINDING("heading/color", theme.heading_color),
    THEME_COLOR_BINDING("caption/color", theme.caption_color),
    THEME_COLOR_BINDING("accent_text/color", theme.accent_text_color),
    THEME_COLOR_BINDING("muted_text/color", theme.muted_text_color),
    THEME_COLOR_BINDING("palette/text_on_light", theme.palette_text_on_light),
    THEME_COLOR_BINDING("palette/text_on_dark", theme.palette_text_on_dark),
    THEME_COLOR_BINDING("palette/swatches/red", theme.palette_swatches[0]),
    THEME_COLOR_BINDING("palette/swatches/orange", theme.palette_swatches[1]),
    THEME_COLOR_BINDING("palette/swatches/yellow", theme.palette_swatches[2]),
    THEME_COLOR_BINDING("palette/swatches/green", theme.palette_swatches[3]),
    THEME_COLOR_BINDING("palette/swatches/blue", theme.palette_swatches[4]),
    THEME_COLOR_BINDING("palette/swatches/purple", theme.palette_swatches[5]),
};

#undef THEME_COLOR_BINDING

auto find_color_binding(std::string_view token) -> std::optional<ColorBinding> {
    for (auto const& binding : kColorBindings) {
        if (binding.token == token) {
            return binding;
        }
    }
    return std::nullopt;
}

auto write_color_token(PathSpace& space,
                       std::string const& edit_root,
                       std::string const& token,
                       Color const& color,
                       bool overwrite) -> SP::Expected<void> {
    auto path = color_node_path(edit_root, token);
    if (!overwrite) {
        auto existing = read_optional<Color>(space, path);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (existing->has_value()) {
            return {};
        }
    }
    return replace_single<Color>(space, path, color);
}

auto update_compiled_theme(PathSpace& space,
                           SP::App::AppRootPathView app_root,
                           std::string const& sanitized_name,
                           WidgetTheme const& updated) -> SP::Expected<void> {
    auto paths = SP::UI::Builders::Config::Theme::Ensure(space,
                                                         app_root,
                                                         sanitized_name,
                                                         updated);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    auto status = replace_single<WidgetTheme>(space, paths->value.getPath(), updated);
    if (!status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto seed_color_tokens(PathSpace& space,
                       std::string const& edit_root,
                       WidgetTheme const& theme,
                       bool overwrite_tokens) -> SP::Expected<void> {
    for (auto const& binding : kColorBindings) {
        auto status = write_color_token(space,
                                        edit_root,
                                        std::string(binding.token),
                                        binding.read(theme),
                                        overwrite_tokens);
        if (!status) {
            return status;
        }
    }
    return {};
}

auto load_theme_value(PathSpace& space,
                      SP::App::AppRootPathView app_root,
                      std::string const& sanitized) -> SP::Expected<WidgetTheme> {
    auto defaults = SP::UI::Builders::Widgets::MakeDefaultWidgetTheme();
    auto paths = SP::UI::Builders::Config::Theme::Ensure(space, app_root, sanitized, defaults);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    auto loaded = SP::UI::Builders::Config::Theme::Load(space, *paths);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return *loaded;
}

} // namespace

auto Create(PathSpace& space,
            SP::App::AppRootPathView app_root,
            CreateOptions const& options) -> SP::Expected<CreateResult> {
    if (options.name.empty()) {
        return std::unexpected(make_error("theme name must not be empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(options.name);
    std::optional<std::string> sanitized_inherits;
    if (options.inherits && !options.inherits->empty()) {
        sanitized_inherits = SP::UI::Builders::Config::Theme::SanitizeName(*options.inherits);
    }

    WidgetTheme seed = options.seed_theme.value_or(SP::UI::Builders::Widgets::MakeDefaultWidgetTheme());
    if (sanitized_inherits) {
        auto parent_theme = load_theme_value(space, app_root, *sanitized_inherits);
        if (!parent_theme) {
            return std::unexpected(parent_theme.error());
        }
        seed = *parent_theme;
    }

    auto config_paths =
        SP::UI::Builders::Config::Theme::Ensure(space, app_root, sanitized, seed);
    if (!config_paths) {
        return std::unexpected(config_paths.error());
    }

    if (options.overwrite_existing_value) {
        auto status = replace_single<WidgetTheme>(space, config_paths->value.getPath(), seed);
        if (!status) {
            return std::unexpected(status.error());
        }
    }

    auto edit_root = theme_edit_root(app_root, sanitized);
    if (!edit_root) {
        return std::unexpected(edit_root.error());
    }

    if (sanitized_inherits) {
        auto edit_inherits = edit_root->getPath() + "/style/inherits";
        auto status = replace_single<std::string>(space, edit_inherits, *sanitized_inherits);
        if (!status) {
            return std::unexpected(status.error());
        }
        auto config_inherits = config_paths->root.getPath() + "/style/inherits";
        status = replace_single<std::string>(space, config_inherits, *sanitized_inherits);
        if (!status) {
            return std::unexpected(status.error());
        }
    }

    if (options.populate_tokens) {
        auto status = seed_color_tokens(space, edit_root->getPath(), seed, options.overwrite_existing_value);
        if (!status) {
            return std::unexpected(status.error());
        }
    }

    if (options.set_active) {
        auto status = SP::UI::Builders::Config::Theme::SetActive(space, app_root, sanitized);
        if (!status) {
            return std::unexpected(status.error());
        }
    }

    SceneLifecycle::InvalidateThemes(space, app_root);

    CreateResult result{
        .canonical_name = sanitized,
        .edit_root = *edit_root,
    };
    return result;
}

auto SetColor(PathSpace& space,
              SP::App::AppRootPathView app_root,
              std::string_view theme_name,
              std::string_view token,
              ColorValue const& value) -> SP::Expected<void> {
    if (theme_name.empty()) {
        return std::unexpected(make_error("theme name must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    auto sanitized_name = SP::UI::Builders::Config::Theme::SanitizeName(theme_name);
    auto normalized_token = normalize_token(token);
    if (!normalized_token) {
        return std::unexpected(normalized_token.error());
    }

    auto binding = find_color_binding(*normalized_token);
    if (!binding) {
        return std::unexpected(make_error("unsupported theme color token '" + std::string(*normalized_token) + "'",
                                          SP::Error::Code::InvalidPath));
    }

    auto edit_root = theme_edit_root(app_root, sanitized_name);
    if (!edit_root) {
        return std::unexpected(edit_root.error());
    }

    auto color = sanitize_color(value.rgba);
    auto write_status = write_color_token(space,
                                          edit_root->getPath(),
                                          *normalized_token,
                                          color,
                                          true);
    if (!write_status) {
        return write_status;
    }

    auto theme_value = load_theme_value(space, app_root, sanitized_name);
    if (!theme_value) {
        return std::unexpected(theme_value.error());
    }
    binding->write(*theme_value, color);

    auto update_status = update_compiled_theme(space, app_root, sanitized_name, *theme_value);
    if (!update_status) {
        return update_status;
    }

    SceneLifecycle::InvalidateThemes(space, app_root);
    return {};
}

auto RebuildValue(PathSpace& space,
                  SP::App::AppRootPathView app_root,
                  std::string_view theme_name) -> SP::Expected<void> {
    if (theme_name.empty()) {
        return std::unexpected(make_error("theme name must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    auto sanitized = SP::UI::Builders::Config::Theme::SanitizeName(theme_name);
    auto theme = load_theme_value(space, app_root, sanitized);
    if (!theme) {
        return std::unexpected(theme.error());
    }

    auto edit_root = theme_edit_root(app_root, sanitized);
    if (!edit_root) {
        return std::unexpected(edit_root.error());
    }

    for (auto const& binding : kColorBindings) {
        auto path = color_node_path(edit_root->getPath(), std::string(binding.token));
        auto stored = read_optional<Color>(space, path);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (!stored->has_value()) {
            continue;
        }
        binding.write(*theme, sanitize_color(**stored));
    }

    auto status = update_compiled_theme(space, app_root, sanitized, *theme);
    if (!status) {
        return status;
    }

    SceneLifecycle::InvalidateThemes(space, app_root);
    return {};
}

} // namespace SP::UI::Declarative::Theme
