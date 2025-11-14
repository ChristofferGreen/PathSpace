#include "WidgetDetail.hpp"

#include <functional>

namespace SP::UI::Builders::Widgets::Bindings {

using namespace Detail;

namespace {

auto write_widget_footprint(PathSpace& space,
                            WidgetPath const& root,
                            DirtyRectHint footprint) -> SP::Expected<void> {
    DirtyRectHint normalized = ensure_valid_hint(footprint);
    if (normalized.max_x <= normalized.min_x || normalized.max_y <= normalized.min_y) {
        return {};
    }
    auto path = std::string(root.getPath()) + "/meta/footprint";
    if (auto status = replace_single<DirtyRectHint>(space, path, normalized); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto compute_ops_queue(WidgetPath const& root) -> ConcretePath {
    return ConcretePath{std::string(root.getPath()) + "/ops/inbox/queue"};
}

auto build_options(AppRootPathView appRoot,
                   WidgetPath const& root,
                   ConcretePathView targetPath,
                   DirtyRectHint hint,
                   bool auto_render) -> BindingOptions {
    BindingOptions options{
        .target = ConcretePath{std::string(targetPath.getPath())},
        .ops_queue = compute_ops_queue(root),
        .dirty_rect = ensure_valid_hint(hint),
        .auto_render = auto_render,
    };
    options.focus_state = Widgets::Focus::FocusStatePath(appRoot);
    options.focus_enabled = true;
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

auto emit_action_callbacks(BindingOptions const& options,
                           WidgetOp const& op) -> void {
    if (options.action_callbacks.empty()) {
        return;
    }

    auto callbacks = options.action_callbacks;
    auto action = Reducers::MakeWidgetAction(op);
    for (auto const& callback : callbacks) {
        if (!callback || !(*callback)) {
            continue;
        }
        (*callback)(action);
    }
}

auto set_widget_focus(PathSpace& space,
                      BindingOptions const& options,
                      WidgetPath const& widget) -> SP::Expected<bool> {
    if (!options.focus_enabled) {
        return false;
    }

    Widgets::Focus::Config config{
        .focus_state = options.focus_state,
        .auto_render_target = options.auto_render
            ? std::optional<ConcretePath>{options.target}
            : std::optional<ConcretePath>{},
    };
    auto result = Widgets::Focus::Set(space, config, widget);
    if (!result) {
        return std::unexpected(result.error());
    }
    return result->changed;
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

using EventHandler = std::function<void()>;

auto invoke_handler_if_present(PathSpace& space,
                               WidgetPath const& widget,
                               std::string_view event) -> SP::Expected<void> {
    if (event.empty()) {
        return {};
    }
    std::string handler_path = std::string(widget.getPath());
    handler_path.append("/events/");
    handler_path.append(event);
    handler_path.append("/handler");

    auto handler = space.read<EventHandler, std::string>(handler_path);
    if (!handler) {
        auto const& error = handler.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return {};
        }
        return std::unexpected(error);
    }

    (*handler)();
    return {};
}

auto enqueue_widget_op(PathSpace& space,
                       BindingOptions const& options,
                       std::string const& widget_path,
                       WidgetOpKind kind,
                       PointerInfo const& pointer,
                       float value,
                       std::string_view target_id = {}) -> SP::Expected<void> {
    WidgetOp op{};
    op.kind = kind;
    op.widget_path = widget_path;
    op.target_id = target_id;
    op.pointer = pointer;
    op.value = value;
    op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

    auto inserted = space.insert(options.ops_queue.getPath(), op);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    emit_action_callbacks(options, op);
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

auto read_tree_style(PathSpace& space,
                     TreePaths const& paths) -> SP::Expected<Widgets::TreeStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TreeStyle, std::string>(stylePath);
}

auto read_tree_nodes(PathSpace& space,
                     TreePaths const& paths) -> SP::Expected<std::vector<Widgets::TreeNode>> {
    return space.read<std::vector<Widgets::TreeNode>, std::string>(paths.nodes.getPath());
}

auto read_text_field_style(PathSpace& space,
                           TextFieldPaths const& paths) -> SP::Expected<Widgets::TextFieldStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TextFieldStyle, std::string>(stylePath);
}

auto read_text_area_style(PathSpace& space,
                          TextAreaPaths const& paths) -> SP::Expected<Widgets::TextAreaStyle> {
    auto stylePath = std::string(paths.root.getPath()) + "/meta/style";
    return space.read<Widgets::TextAreaStyle, std::string>(stylePath);
}

} // namespace

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo {
    return PointerInfo::Make(hit.position.scene_x, hit.position.scene_y)
        .WithInside(hit.hit);
}

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ButtonBinding> {
    if (auto style = read_button_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ButtonBinding binding{
        .widget = paths,
        .options = build_options(appRoot,
                                 paths.root,
                                 targetPath,
                                 hint,
                                 auto_render),
    };
    return binding;
}

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<ToggleBinding> {
    if (auto style = read_toggle_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ToggleBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<SliderBinding> {
    if (auto style = read_slider_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    SliderBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateListBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<ListBinding> {
    if (auto style = read_list_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    if (auto items = read_list_items(space, paths); !items) {
        return std::unexpected(items.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    ListBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTreeBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       TreePaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override,
                       bool auto_render) -> SP::Expected<TreeBinding> {
    if (auto style = read_tree_style(space, paths); !style) {
        return std::unexpected(style.error());
    }
    if (auto nodes = read_tree_nodes(space, paths); !nodes) {
        return std::unexpected(nodes.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    TreeBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateStackBinding(PathSpace& space,
                        AppRootPathView appRoot,
                        StackPaths const& paths,
                        ConcretePathView targetPath,
                        DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override,
                         bool auto_render) -> SP::Expected<StackBinding> {
    auto layout = Widgets::ReadStackLayout(space, paths);
    if (!layout) {
        return std::unexpected(layout.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(footprint));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }
    StackBinding binding{
        .layout = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTextFieldBinding(PathSpace& space,
                            AppRootPathView appRoot,
                            TextFieldPaths const& paths,
                            ConcretePathView targetPath,
                            DirtyRectHint footprint,
                            std::optional<DirtyRectHint> dirty_override,
                            bool auto_render) -> SP::Expected<TextFieldBinding> {
    auto style = read_text_field_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(text_input_dirty_hint(*style)));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }

    TextFieldBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
    };
    return binding;
}

auto CreateTextAreaBinding(PathSpace& space,
                           AppRootPathView appRoot,
                           TextAreaPaths const& paths,
                           ConcretePathView targetPath,
                           DirtyRectHint footprint,
                           std::optional<DirtyRectHint> dirty_override,
                           bool auto_render) -> SP::Expected<TextAreaBinding> {
    auto style = read_text_area_style(space, paths);
    if (!style) {
        return std::unexpected(style.error());
    }

    DirtyRectHint hint = ensure_valid_hint(dirty_override.value_or(text_input_dirty_hint(*style)));
    if (auto status = write_widget_footprint(space, paths.root, hint); !status) {
        return std::unexpected(status.error());
    }

    TextAreaBinding binding{
        .widget = paths,
        .options = build_options(appRoot, paths.root, targetPath, hint, auto_render),
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
                                        value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Activate) {
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "press"); !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Activate) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
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
                                        value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::Toggle) {
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "toggle"); !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::Press || op_kind == WidgetOpKind::Toggle) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer) -> SP::Expected<bool> {
    bool enqueue_op = false;
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
        enqueue_op = false;
        break;
    case WidgetOpKind::SliderBegin:
    case WidgetOpKind::SliderUpdate:
    case WidgetOpKind::SliderCommit:
        enqueue_op = true;
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

    if (enqueue_op) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            current_state->value);
            !status) {
            return std::unexpected(status.error());
        }
        if (op_kind == WidgetOpKind::SliderCommit) {
            if (auto handler = invoke_handler_if_present(space, binding.widget.root, "change"); !handler) {
                return std::unexpected(handler.error());
            }
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::SliderBegin || op_kind == WidgetOpKind::SliderCommit) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
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
                                        op_value);
        !status) {
        return std::unexpected(status.error());
    }
    if (op_kind == WidgetOpKind::ListSelect || op_kind == WidgetOpKind::ListActivate) {
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "child_event"); !handler) {
            return std::unexpected(handler.error());
        }
    }
    bool focus_changed = false;
    if (op_kind == WidgetOpKind::ListSelect || op_kind == WidgetOpKind::ListActivate) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchTree(PathSpace& space,
                  TreeBinding const& binding,
                  TreeState const& new_state,
                  WidgetOpKind op_kind,
                  std::string_view node_id,
                  PointerInfo const& pointer,
                  float scroll_delta) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::TreeHover:
    case WidgetOpKind::TreeSelect:
    case WidgetOpKind::TreeToggle:
    case WidgetOpKind::TreeExpand:
    case WidgetOpKind::TreeCollapse:
    case WidgetOpKind::TreeRequestLoad:
    case WidgetOpKind::TreeScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for tree binding",
                                          SP::Error::Code::InvalidType));
    }

    auto current_state = space.read<TreeState, std::string>(binding.widget.state.getPath());
    if (!current_state) {
        return std::unexpected(current_state.error());
    }

    auto nodes = read_tree_nodes(space, binding.widget);
    if (!nodes) {
        return std::unexpected(nodes.error());
    }

    auto [index, children, roots] = build_tree_children(*nodes);
    (void)roots;

    auto find_node = [&](std::string const& id) -> std::optional<std::size_t> {
        auto it = index.find(id);
        if (it == index.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    TreeState desired = *current_state;
    std::string node_key = node_id.empty() ? std::string{} : std::string{node_id};

    auto ensure_node = [&]() -> std::optional<std::size_t> {
        if (node_key.empty()) {
            return std::nullopt;
        }
        return find_node(node_key);
    };

    bool should_request_load = false;

    switch (op_kind) {
    case WidgetOpKind::TreeHover: {
        if (node_key.empty()) {
            desired.hovered_id.clear();
        } else if (auto node_index = ensure_node()) {
            if ((*nodes)[*node_index].enabled) {
                desired.hovered_id = node_key;
            }
        }
        break;
    }
    case WidgetOpKind::TreeSelect: {
        if (!node_key.empty()) {
            if (auto node_index = ensure_node()) {
                if ((*nodes)[*node_index].enabled) {
                    desired.selected_id = node_key;
                    desired.hovered_id = node_key;
                }
            }
        }
        break;
    }
    case WidgetOpKind::TreeToggle:
    case WidgetOpKind::TreeExpand:
    case WidgetOpKind::TreeCollapse:
    case WidgetOpKind::TreeRequestLoad: {
        if (node_key.empty()) {
            return std::unexpected(make_error("tree operation requires node id",
                                              SP::Error::Code::InvalidPath));
        }
        auto node_index = ensure_node();
        if (!node_index) {
            return std::unexpected(make_error("unknown tree node id",
                                              SP::Error::Code::InvalidPath));
        }
        bool has_published_children = (*node_index < children.size()) && !children[*node_index].empty();
        bool expandable = has_published_children || (*nodes)[*node_index].expandable;
        if (!expandable) {
            break;
        }

        auto toggle_expansion = [&](bool expand) {
            auto& expanded = desired.expanded_ids;
            auto it = std::find(expanded.begin(), expanded.end(), node_key);
            if (expand) {
                if (it == expanded.end()) {
                    expanded.push_back(node_key);
                    if (!has_published_children && (*nodes)[*node_index].expandable) {
                        should_request_load = true;
                        if (std::find(desired.loading_ids.begin(), desired.loading_ids.end(), node_key)
                            == desired.loading_ids.end()) {
                            desired.loading_ids.push_back(node_key);
                        }
                    }
                }
            } else {
                if (it != expanded.end()) {
                    expanded.erase(it);
                }
                desired.loading_ids.erase(std::remove(desired.loading_ids.begin(),
                                                     desired.loading_ids.end(),
                                                     node_key),
                                          desired.loading_ids.end());
            }
        };

        if (op_kind == WidgetOpKind::TreeToggle) {
            bool is_expanded = std::find(desired.expanded_ids.begin(), desired.expanded_ids.end(), node_key)
                               != desired.expanded_ids.end();
            toggle_expansion(!is_expanded);
        } else if (op_kind == WidgetOpKind::TreeExpand) {
            toggle_expansion(true);
        } else if (op_kind == WidgetOpKind::TreeCollapse) {
            toggle_expansion(false);
        } else if (op_kind == WidgetOpKind::TreeRequestLoad) {
            if (std::find(desired.loading_ids.begin(), desired.loading_ids.end(), node_key)
                == desired.loading_ids.end()) {
                desired.loading_ids.push_back(node_key);
            }
        }
        break;
    }
    case WidgetOpKind::TreeScroll: {
        desired.scroll_offset = current_state->scroll_offset + scroll_delta;
        break;
    }
    default:
        break;
    }

    auto changed = Widgets::UpdateTreeState(space, binding.widget, desired);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    auto updated_state = space.read<TreeState, std::string>(binding.widget.state.getPath());
    if (!updated_state) {
        return std::unexpected(updated_state.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/tree"); !status) {
            return std::unexpected(status.error());
        }
    }

    float op_value = 0.0f;
    if (op_kind == WidgetOpKind::TreeScroll) {
        op_value = updated_state->scroll_offset;
    }

    if (auto status = enqueue_widget_op(space,
                                        binding.options,
                                        binding.widget.root.getPath(),
                                        op_kind,
                                        pointer,
                                        op_value,
                                        node_key);
        !status) {
        return std::unexpected(status.error());
    }
    switch (op_kind) {
    case WidgetOpKind::TreeHover:
    case WidgetOpKind::TreeScroll:
        break;
    default:
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "node_event"); !handler) {
            return std::unexpected(handler.error());
        }
        break;
    }

    if (should_request_load) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            WidgetOpKind::TreeRequestLoad,
                                            pointer,
                                            0.0f,
                                            node_key);
            !status) {
            return std::unexpected(status.error());
        }
        if (auto handler = invoke_handler_if_present(space, binding.widget.root, "node_event"); !handler) {
            return std::unexpected(handler.error());
        }
    }

    bool focus_changed = false;
    if (op_kind == WidgetOpKind::TreeSelect || op_kind == WidgetOpKind::TreeToggle) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }
    return *changed || focus_changed;
}

auto DispatchTextField(PathSpace& space,
                       TextFieldBinding const& binding,
                       TextFieldState const& new_state,
                       WidgetOpKind op_kind,
                       PointerInfo const& pointer) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::TextInput:
    case WidgetOpKind::TextDelete:
    case WidgetOpKind::TextMoveCursor:
    case WidgetOpKind::TextSetSelection:
    case WidgetOpKind::TextCompositionStart:
    case WidgetOpKind::TextCompositionUpdate:
    case WidgetOpKind::TextCompositionCommit:
    case WidgetOpKind::TextCompositionCancel:
    case WidgetOpKind::TextSubmit:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for text field binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateTextFieldState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/text_field"); !status) {
            return std::unexpected(status.error());
        }
    }

    bool focus_changed = false;
    if (op_kind != WidgetOpKind::HoverEnter && op_kind != WidgetOpKind::HoverExit) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }

    bool enqueue = (op_kind != WidgetOpKind::HoverEnter && op_kind != WidgetOpKind::HoverExit);
    float value = (op_kind == WidgetOpKind::TextSubmit) ? 1.0f : 0.0f;
    if (enqueue) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            value);
            !status) {
            return std::unexpected(status.error());
        }
        std::string_view event_name;
        if (op_kind == WidgetOpKind::TextSubmit) {
            event_name = "submit";
        } else {
            event_name = "change";
        }
        if (!event_name.empty()) {
            if (auto handler = invoke_handler_if_present(space, binding.widget.root, event_name); !handler) {
                return std::unexpected(handler.error());
            }
        }
    }

    return *changed || focus_changed;
}

auto DispatchTextArea(PathSpace& space,
                      TextAreaBinding const& binding,
                      TextAreaState const& new_state,
                      WidgetOpKind op_kind,
                      PointerInfo const& pointer,
                      float scroll_delta_y) -> SP::Expected<bool> {
    switch (op_kind) {
    case WidgetOpKind::HoverEnter:
    case WidgetOpKind::HoverExit:
    case WidgetOpKind::TextInput:
    case WidgetOpKind::TextDelete:
    case WidgetOpKind::TextMoveCursor:
    case WidgetOpKind::TextSetSelection:
    case WidgetOpKind::TextCompositionStart:
    case WidgetOpKind::TextCompositionUpdate:
    case WidgetOpKind::TextCompositionCommit:
    case WidgetOpKind::TextCompositionCancel:
    case WidgetOpKind::TextSubmit:
    case WidgetOpKind::TextScroll:
        break;
    default:
        return std::unexpected(make_error("Unsupported widget op kind for text area binding",
                                          SP::Error::Code::InvalidType));
    }

    auto changed = Widgets::UpdateTextAreaState(space, binding.widget, new_state);
    if (!changed) {
        return std::unexpected(changed.error());
    }

    if (*changed) {
        if (auto status = submit_dirty_hint(space, binding.options); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = schedule_auto_render(space, binding.options, "widget/text_area"); !status) {
            return std::unexpected(status.error());
        }
    }

    bool focus_changed = false;
    if (op_kind != WidgetOpKind::HoverEnter && op_kind != WidgetOpKind::HoverExit) {
        auto focus = set_widget_focus(space, binding.options, binding.widget.root);
        if (!focus) {
            return std::unexpected(focus.error());
        }
        focus_changed = *focus;
    }

    bool enqueue = (op_kind != WidgetOpKind::HoverEnter && op_kind != WidgetOpKind::HoverExit);
    float value = (op_kind == WidgetOpKind::TextScroll) ? scroll_delta_y : 0.0f;
    if (enqueue) {
        if (auto status = enqueue_widget_op(space,
                                            binding.options,
                                            binding.widget.root.getPath(),
                                            op_kind,
                                            pointer,
                                            value);
            !status) {
            return std::unexpected(status.error());
        }
        std::string_view event_name;
        if (op_kind == WidgetOpKind::TextSubmit) {
            event_name = "submit";
        } else {
            event_name = "change";
        }
        if (!event_name.empty()) {
            if (auto handler = invoke_handler_if_present(space, binding.widget.root, event_name); !handler) {
                return std::unexpected(handler.error());
            }
        }
    }

    return *changed || focus_changed;
}

auto UpdateStack(PathSpace& space,
                 StackBinding const& binding,
                 StackLayoutParams const& params) -> SP::Expected<bool> {
    auto changed = Widgets::UpdateStackLayout(space, binding.layout, params);
    if (!changed) {
        return std::unexpected(changed.error());
    }
    if (!*changed) {
        return false;
    }

    auto layout = Widgets::ReadStackLayout(space, binding.layout);
    if (!layout) {
        return std::unexpected(layout.error());
    }

    DirtyRectHint updated_hint = binding.options.dirty_rect;
    auto layout_hint = make_default_dirty_rect(layout->width, layout->height);
    if (updated_hint.max_x <= updated_hint.min_x || updated_hint.max_y <= updated_hint.min_y) {
        updated_hint = layout_hint;
    } else {
        updated_hint.min_x = std::min(updated_hint.min_x, layout_hint.min_x);
        updated_hint.min_y = std::min(updated_hint.min_y, layout_hint.min_y);
        updated_hint.max_x = std::max(updated_hint.max_x, layout_hint.max_x);
        updated_hint.max_y = std::max(updated_hint.max_y, layout_hint.max_y);
    }
    updated_hint = ensure_valid_hint(updated_hint);
    if (auto status = write_widget_footprint(space, binding.layout.root, updated_hint); !status) {
        return std::unexpected(status.error());
    }

    BindingOptions refreshed = binding.options;
    refreshed.dirty_rect = updated_hint;
    if (auto status = submit_dirty_hint(space, refreshed); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = schedule_auto_render(space, refreshed, "widget/stack"); !status) {
        return std::unexpected(status.error());
    }
    return true;
}

} // namespace SP::UI::Builders::Widgets::Bindings
