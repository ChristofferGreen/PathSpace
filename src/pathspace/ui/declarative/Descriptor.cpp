#include <pathspace/ui/declarative/Descriptor.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/core/Error.hpp>
#include <pathspace/ui/TextBuilder.hpp>

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace SP::UI::Declarative {
namespace {

namespace Detail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;

auto make_descriptor_error(std::string message,
                           SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return Detail::make_error(std::move(message), code);
}

auto kind_from_string(std::string_view raw) -> std::optional<WidgetKind> {
    if (raw == "button") {
        return WidgetKind::Button;
    }
    if (raw == "toggle") {
        return WidgetKind::Toggle;
    }
    if (raw == "slider") {
        return WidgetKind::Slider;
    }
    if (raw == "list") {
        return WidgetKind::List;
    }
    if (raw == "tree") {
        return WidgetKind::Tree;
    }
    if (raw == "stack") {
        return WidgetKind::Stack;
    }
    if (raw == "label") {
        return WidgetKind::Label;
    }
    if (raw == "input_field") {
        return WidgetKind::InputField;
    }
    if (raw == "paint_surface") {
        return WidgetKind::PaintSurface;
    }
    return std::nullopt;
}

template <typename T>
auto read_required(PathSpace& space, std::string const& path) -> SP::Expected<T> {
    auto value = space.read<T, std::string>(path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return *value;
}

auto read_label_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<LabelDescriptor> {
    LabelDescriptor descriptor{};
    auto text = read_required<std::string>(space, root + "/state/text");
    if (!text) {
        return std::unexpected(text.error());
    }
    descriptor.text = *text;
    auto typography = read_required<BuilderWidgets::TypographyStyle>(space, root + "/meta/typography");
    if (!typography) {
        return std::unexpected(typography.error());
    }
    descriptor.typography = *typography;
    auto color = read_required<std::array<float, 4>>(space, root + "/meta/color");
    if (!color) {
        return std::unexpected(color.error());
    }
    descriptor.color = *color;
    return descriptor;
}

auto read_button_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ButtonDescriptor> {
    ButtonDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ButtonStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ButtonState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto label = space.read<std::string, std::string>(root + "/meta/label");
    if (label) {
        descriptor.label = *label;
    } else {
        auto const& err = label.error();
        if (err.code != SP::Error::Code::NoSuchPath
            && err.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(err);
        }
    }
    return descriptor;
}

auto read_toggle_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ToggleDescriptor> {
    ToggleDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ToggleStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ToggleState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    return descriptor;
}

auto read_slider_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<SliderDescriptor> {
    SliderDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::SliderStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::SliderState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto range = read_required<BuilderWidgets::SliderRange>(space, root + "/meta/range");
    if (!range) {
        return std::unexpected(range.error());
    }
    descriptor.range = *range;
    return descriptor;
}

auto read_list_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<ListDescriptor> {
    ListDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::ListStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::ListState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto items = space.read<std::vector<BuilderWidgets::ListItem>, std::string>(root + "/meta/items");
    if (items) {
        descriptor.items = *items;
    } else {
        auto const& err = items.error();
        if (err.code != SP::Error::Code::NoSuchPath
            && err.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(err);
        }
    }
    return descriptor;
}

auto read_tree_descriptor(PathSpace& space, std::string const& root)
    -> SP::Expected<TreeDescriptor> {
    TreeDescriptor descriptor{};
    auto style = read_required<BuilderWidgets::TreeStyle>(space, root + "/meta/style");
    if (!style) {
        return std::unexpected(style.error());
    }
    descriptor.style = *style;
    auto state = read_required<BuilderWidgets::TreeState>(space, root + "/state");
    if (!state) {
        return std::unexpected(state.error());
    }
    descriptor.state = *state;
    auto nodes = space.read<std::vector<BuilderWidgets::TreeNode>, std::string>(root + "/meta/nodes");
    if (nodes) {
        descriptor.nodes = *nodes;
    } else {
        return std::unexpected(nodes.error());
    }
    return descriptor;
}

struct BucketVisitor {
    DescriptorBucketOptions options;
    std::string authoring_root;

    auto operator()(ButtonDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ButtonPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildButtonPreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(ToggleDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TogglePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildTogglePreview(descriptor.style, descriptor.state, preview);
    }

    auto operator()(SliderDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::SliderPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        return BuilderWidgets::BuildSliderPreview(descriptor.style,
                                           descriptor.range,
                                           descriptor.state,
                                           preview);
    }

    auto operator()(ListDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::ListPreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildListPreview(descriptor.style,
                                                std::span<BuilderWidgets::ListItem const>{descriptor.items},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(TreeDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        BuilderWidgets::TreePreviewOptions preview{};
        preview.authoring_root = authoring_root;
        preview.pulsing_highlight = options.pulsing_highlight;
        auto result = BuilderWidgets::BuildTreePreview(descriptor.style,
                                                std::span<BuilderWidgets::TreeNode const>{descriptor.nodes},
                                                descriptor.state,
                                                preview);
        return result.bucket;
    }

    auto operator()(LabelDescriptor const& descriptor) const
        -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
        auto drawable_id = std::hash<std::string>{}(authoring_root);
        auto built = SP::UI::Builders::Text::BuildTextBucket(descriptor.text,
                                                              0.0f,
                                                              descriptor.typography.line_height,
                                                              descriptor.typography,
                                                              descriptor.color,
                                                              drawable_id,
                                                              authoring_root + "/label",
                                                              0.0f);
        if (!built) {
            return std::unexpected(make_descriptor_error("Failed to build label bucket"));
        }
        return built->bucket;
    }
};

} // namespace

auto LoadWidgetDescriptor(PathSpace& space,
                          SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<WidgetDescriptor> {
    auto root = widget.getPath();
    auto kind_value = space.read<std::string, std::string>(root + "/meta/kind");
    if (!kind_value) {
        return std::unexpected(kind_value.error());
    }
    auto kind = kind_from_string(*kind_value);
    if (!kind) {
        return std::unexpected(make_descriptor_error("Unsupported declarative widget kind: " + *kind_value,
                                                     SP::Error::Code::NotSupported));
    }

    WidgetDescriptor descriptor{};
    descriptor.kind = *kind;
    descriptor.widget = widget;

    switch (*kind) {
    case WidgetKind::Button: {
        auto loaded = read_button_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Toggle: {
        auto loaded = read_toggle_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Slider: {
        auto loaded = read_slider_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::List: {
        auto loaded = read_list_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Tree: {
        auto loaded = read_tree_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Label: {
        auto loaded = read_label_descriptor(space, root);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        descriptor.data = *loaded;
        break;
    }
    case WidgetKind::Stack:
    case WidgetKind::InputField:
    case WidgetKind::PaintSurface:
        return std::unexpected(make_descriptor_error("WidgetKind not yet supported by descriptor loader",
                                                     SP::Error::Code::NotSupported));
    }

    return descriptor;
}

auto BuildWidgetBucket(WidgetDescriptor const& descriptor,
                       DescriptorBucketOptions const& options)
    -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot> {
    BucketVisitor visitor{
        .options = options,
        .authoring_root = descriptor.widget.getPath(),
    };
    return std::visit(visitor, descriptor.data);
}

} // namespace SP::UI::Declarative
