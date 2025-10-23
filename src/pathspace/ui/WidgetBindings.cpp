#include "WidgetDetail.hpp"

namespace SP::UI::Builders::Widgets::Bindings {

using namespace Detail;

namespace {

auto compute_ops_queue(WidgetPath const& root) -> ConcretePath {
    return ConcretePath{std::string(root.getPath()) + "/ops/inbox/queue"};
}

auto build_options(WidgetPath const& root,
                   ConcretePathView targetPath,
                   DirtyRectHint hint,
                   bool auto_render) -> BindingOptions {
    BindingOptions options{
        .target = ConcretePath{std::string(targetPath.getPath())},
        .ops_queue = compute_ops_queue(root),
        .dirty_rect = ensure_valid_hint(hint),
        .auto_render = auto_render,
    };
    return options;
}

auto read_frame_index(PathSpace& space, std::string const& target) -> SP::Expected<std::uint64_t> {
    auto frame = read_optional<std::uint64_t>(space, target + "/output/v1/common/frameIndex");
    if (!frame) {
        return std::unexpected(frame.error());
    }
    if (frame->has_value()) {
        return **frame;
    }
    return std::uint64_t{0};
}

auto submit_dirty_hint(PathSpace& space,
                       BindingOptions const& options) -> SP::Expected<void> {
    auto const& rect = options.dirty_rect;
    if (rect.max_x <= rect.min_x || rect.max_y <= rect.min_y) {
        return {};
    }
    std::array<DirtyRectHint, 1> hints{rect};
    return Renderer::SubmitDirtyRects(space,
                                      SP::ConcretePathStringView{options.target.getPath()},
                                      std::span<const DirtyRectHint>(hints.data(), hints.size()));
}

auto schedule_auto_render(PathSpace& space,
                          BindingOptions const& options,
                          std::string_view reason) -> SP::Expected<void> {
    if (!options.auto_render) {
        return {};
    }
    auto frame_index = read_frame_index(space, options.target.getPath());
    if (!frame_index) {
        return std::unexpected(frame_index.error());
    }
    return enqueue_auto_render_event(space,
                                     options.target.getPath(),
                                     reason,
                                     *frame_index);
}

auto enqueue_widget_op(PathSpace& space,
                       BindingOptions const& options,
                       std::string const& widget_path,
                       WidgetOpKind kind,
                       PointerInfo const& pointer,
                       float value) -> SP::Expected<void> {
    WidgetOp op{};
    op.kind = kind;
    op.widget_path = widget_path;
    op.pointer = pointer;
    op.value = value;
    op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

    auto inserted = space.insert(options.ops_queue.getPath(), op);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

auto read_button_style(PathSpace& space,
                       ButtonPaths const& paths) -> SP::Expected<Widgets::ButtonStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ButtonStyle, std::string>(stylePath);
}

auto read_toggle_style(PathSpace& space,
                       TogglePaths const& paths) -> SP::Expected<Widgets::ToggleStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ToggleStyle, std::string>(stylePath);
}

auto read_slider_style(PathSpace& space,
                       SliderPaths const& paths) -> SP::Expected<Widgets::SliderStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::SliderStyle, std::string>(stylePath);
}

auto read_list_style(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<Widgets::ListStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::ListStyle, std::string>(stylePath);
}

auto read_list_items(PathSpace& space,
                     ListPaths const& paths) -> SP::Expected<std::vector<Widgets::ListItem>> {
    auto itemsPath = std::string(paths.root.getPath()) + "/meta/items";
    return space.read<std::vector<Widgets::ListItem>, std::string>(itemsPath);
}

} // namespace

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo {
    PointerInfo info{};
    info.scene_x = hit.position.scene_x;
    info.scene_y = hit.position.scene_y;
    info.inside = hit.hit;
    info.primary = true;
    return info;
}

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ButtonBinding> {
    auto style = read_button_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ButtonBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ToggleBinding> {
    auto style = read_toggle_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    ToggleBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<SliderBinding> {
    auto style = read_slider_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, style->height));
    SliderBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateListBinding(PathSpace& space,
                       AppRootPathView,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<ListBinding> {
    auto style = read_list_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }
    auto items = read_list_items(space, paths);
    if (!items) {
        return std::unexpected(items.error());
    }

    auto item_count = std::max<std::size_t>(items->size(), 1u);
    float height = style->item_height * static_cast<float>(item_count) + style->border_thickness * 2.0f;
    DirtyRectHint hint = dirty_override.value_or(make_default_dirty_rect(style->width, height));
    ListBinding binding{
        .widget = paths,
        .options = build_options(paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Activate:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for button binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateButtonState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/button"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.pressed ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::Press:
    case WidgetOpKind::Release:
    case WidgetOpKind::Toggle:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for toggle binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateToggleState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/toggle"); !status) {
            return std::unexpected(status.error());
        }
    }

    float value = new_state.checked ? 1.0f : 0.0f;
    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::SliderBegin:
    case WidgetOpKind::SliderUpdate:
    case WidgetOpKind::SliderCommit:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for slider binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateSliderState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto current_state = space.read<SliderState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/slider"); !status) {
            return std::unexpected(status.error());
        }
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        current_state->value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer,
                  std::int32_t item_index,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::ListHover:
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
    case WidgetOpKind::ListScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for list binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    Widgets::ListState desired = new_state;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        desired.hovered_index = item_index;
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        if (item_index >= 0) {
            desired.selected_index = item_index;
        }
        break;
    case WidgetOpKind::ListScroll:
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    default:
        break;
    }

    auto changed = Widgets::UpdateListState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<ListState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space,
                                               binding.options,
                                               "widget/list"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    switch (op_kind) {
    case WidgetOpKind::ListHover:
        op_value = static_cast<float>(updated_state->hovered_index);
        break;
    case WidgetOpKind::ListSelect:
    case WidgetOpKind::ListActivate:
        op_value = static_cast<float>(updated_state->selected_index);
        break;
    case WidgetOpKind::ListScroll:
        op_value = updated_state->scroll_offset;
        break;
    default:
        break;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value); !status) {
        return std::unexpected(status.error());
    }
    return *changed;
}

} // namespace SP::UI::Builders::Widgets::Bindings
