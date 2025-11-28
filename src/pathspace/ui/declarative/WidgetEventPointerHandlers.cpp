#include "WidgetEventTrellisWorker.hpp"

#include "WidgetStateMutators.hpp"
#include "widgets/Common.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace SP::UI::Declarative {

namespace DeclarativeDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
namespace BuildersScene = SP::UI::Runtime::Scene;

namespace {
    auto format_paint_component(std::uint64_t stroke_id) -> std::string {
        return std::string{"paint_surface/stroke/"}.append(std::to_string(stroke_id));
    }

    void reset_paint_state(PointerState& state) {
        state.paint_active_widget.reset();
        state.paint_active_stroke_id.reset();
        state.paint_has_last_local = false;
    }
} // namespace

void WidgetEventTrellisWorker::handle_pointer_event(WindowBinding const& binding,
                              SP::IO::PointerEvent const& event) {
        auto& state = pointer_state(binding.token);

        if (event.motion.absolute) {
            state.x = event.motion.absolute_x;
            state.y = event.motion.absolute_y;
            state.have_position = true;
        } else if (event.motion.delta_x != 0.0f || event.motion.delta_y != 0.0f) {
            state.x += event.motion.delta_x;
            state.y += event.motion.delta_y;
            state.have_position = true;
        }

        if (!state.have_position) {
            return;
        }

        auto target = resolve_target(binding, state);
        update_hover(binding, state, target);
        if (state.primary_down && state.slider_active_widget && target && target->kind == TargetKind::Slider
            && target->widget_path == *state.slider_active_widget) {
            handle_slider_update(binding, state, *target);
        }
        if (state.primary_down && state.paint_active_widget && target
            && target->kind == TargetKind::PaintSurface
            && target->widget_path == *state.paint_active_widget) {
            handle_paint_update(binding, state, *target);
        }
    }

    void WidgetEventTrellisWorker::handle_mouse_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event) {
        auto& state = pointer_state(binding.token);
        bool pressed = event.state.pressed;

        if (pressed) {
            state.primary_down = true;
            state.active_target = state.hover_target;
            if (state.active_target && state.active_target->valid()) {
                switch (state.active_target->kind) {
                case TargetKind::Button:
                case TargetKind::Toggle:
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Press,
                                   1.0f,
                                   true);
                    if (state.active_target->kind == TargetKind::Button) {
                        DeclarativeDetail::SetButtonPressed(space_, state.active_target->widget_path, true);
                    }
                    break;
                case TargetKind::Slider:
                    handle_slider_begin(binding, state, *state.active_target);
                    break;
                case TargetKind::List:
                    handle_list_press(state, *state.active_target);
                    break;
                case TargetKind::TreeRow:
                case TargetKind::TreeToggle:
                    handle_tree_press(state, *state.active_target);
                    break;
                case TargetKind::StackPanel:
                    handle_stack_press(state, *state.active_target);
                    break;
                case TargetKind::PaintSurface:
                    handle_paint_begin(binding, state, *state.active_target);
                    break;
                default:
                    break;
                }
            }
            return;
        }

        if (!state.primary_down) {
            return;
        }
        state.primary_down = false;

        if (state.active_target && state.active_target->valid()) {
            switch (state.active_target->kind) {
            case TargetKind::Button:
                emit_widget_op(binding,
                               *state.active_target,
                               WidgetBindings::WidgetOpKind::Release,
                               0.0f,
                               true);
                DeclarativeDetail::SetButtonPressed(space_, state.active_target->widget_path, false);
                if (state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path) {
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Activate,
                                   1.0f,
                                   true);
                }
                break;
            case TargetKind::Toggle:
                emit_widget_op(binding,
                               *state.active_target,
                               WidgetBindings::WidgetOpKind::Release,
                               0.0f,
                               true);
                if (state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path) {
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Toggle,
                                   1.0f,
                                   true);
                    DeclarativeDetail::ToggleToggleChecked(space_, state.active_target->widget_path);
                }
                break;
            case TargetKind::Slider: {
                bool inside = state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path;
                handle_slider_commit(binding, state, inside);
                break;
            }
            case TargetKind::List:
                handle_list_release(binding, state, *state.active_target);
                break;
            case TargetKind::TreeRow:
            case TargetKind::TreeToggle:
                handle_tree_release(binding, state, *state.active_target);
                break;
            case TargetKind::InputField:
                handle_text_focus(binding, state, *state.active_target);
                break;
            case TargetKind::StackPanel:
                handle_stack_release(binding, state, *state.active_target);
                break;
            case TargetKind::PaintSurface: {
                bool inside = state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path;
                handle_paint_commit(binding, state, inside);
                break;
            }
            default:
                break;
            }
        }

        state.active_target.reset();
    }

    void WidgetEventTrellisWorker::handle_slider_begin(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto data = read_slider_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto value = slider_value_from_local(*data, target.local_x);
        state.slider_active_widget = target.widget_path;
        state.slider_active_value = value;
        data->state.dragging = true;
        data->state.value = value;
        write_slider_state(space_, target.widget_path, data->state);
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::SliderBegin,
                       value,
                       true);
    }

    void WidgetEventTrellisWorker::handle_slider_update(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target) {
        if (!state.slider_active_widget
            || *state.slider_active_widget != target.widget_path
            || !target.has_local) {
            return;
        }
        auto data = read_slider_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto value = slider_value_from_local(*data, target.local_x);
        if (std::abs(value - state.slider_active_value) < 1e-4f) {
            return;
        }
        state.slider_active_value = value;
        data->state.dragging = true;
        data->state.value = value;
        write_slider_state(space_, target.widget_path, data->state);
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::SliderUpdate,
                       value,
                       true);
    }

    void WidgetEventTrellisWorker::handle_slider_commit(WindowBinding const& binding,
                              PointerState& state,
                              bool inside) {
        if (!state.slider_active_widget) {
            return;
        }
        TargetInfo info{};
        info.widget_path = *state.slider_active_widget;
        info.component = "slider/thumb";
        info.kind = TargetKind::Slider;
        if (auto data = read_slider_data(space_, info.widget_path)) {
            data->state.dragging = false;
            data->state.value = state.slider_active_value;
            write_slider_state(space_, info.widget_path, data->state);
        }
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::SliderCommit,
                       state.slider_active_value,
                       inside);
        state.slider_active_widget.reset();
    }

    void WidgetEventTrellisWorker::handle_list_press(PointerState& state, TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto data = read_list_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto index = list_index_from_local(*data, target.local_y);
        state.list_press_widget = target.widget_path;
        state.list_press_index = index;
    }

    void WidgetEventTrellisWorker::handle_list_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.list_press_widget || *state.list_press_widget != target.widget_path) {
            return;
        }
        if (!state.list_press_index || *state.list_press_index < 0) {
            state.list_press_widget.reset();
            state.list_press_index.reset();
            return;
        }
        if (!state.hover_target || state.hover_target->widget_path != target.widget_path) {
            state.list_press_widget.reset();
            state.list_press_index.reset();
            return;
        }
        auto index = *state.list_press_index;
        TargetInfo info = target;
        info.component = "list/item/" + std::to_string(index);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListSelect,
                       static_cast<float>(index),
                       true);
        DeclarativeDetail::SetListSelectionIndex(space_, target.widget_path, index);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListActivate,
                       static_cast<float>(index),
                       true);
        state.list_press_widget.reset();
        state.list_press_index.reset();
    }

    void WidgetEventTrellisWorker::handle_stack_press(PointerState& state, TargetInfo const& target) {
        if (!target.stack_panel_id) {
            return;
        }
        state.stack_press_widget = target.widget_path;
        state.stack_press_panel = target.stack_panel_id;
    }

    void WidgetEventTrellisWorker::handle_stack_release(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target) {
        if (!target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.stack_press_widget || target.widget_path != *state.stack_press_widget) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.stack_press_panel || *state.stack_press_panel != *target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.hover_target || state.hover_target->widget_path != target.widget_path
            || !state.hover_target->stack_panel_id
            || *state.hover_target->stack_panel_id != *target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }

        if (write_stack_active_panel(space_, target.widget_path, *target.stack_panel_id)) {
            TargetInfo info = target;
            info.component = "stack/panel/" + *target.stack_panel_id;
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::StackSelect,
                           0.0f,
                           true);
        }

        state.stack_press_widget.reset();
        state.stack_press_panel.reset();
    }

    void WidgetEventTrellisWorker::handle_paint_begin(WindowBinding const& binding,
                            PointerState& state,
                            TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto stroke_id = ++state.paint_stroke_sequence;
        state.paint_active_widget = target.widget_path;
        state.paint_active_stroke_id = stroke_id;
        state.paint_last_local_x = target.local_x;
        state.paint_last_local_y = target.local_y;
        state.paint_has_last_local = true;

        TargetInfo info = target;
        info.component = format_paint_component(stroke_id);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeBegin,
                       0.0f,
                       true);
    }

    void WidgetEventTrellisWorker::handle_paint_update(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.paint_active_widget || !state.paint_active_stroke_id) {
            return;
        }
        if (target.widget_path != *state.paint_active_widget || !target.has_local) {
            return;
        }
        state.paint_last_local_x = target.local_x;
        state.paint_last_local_y = target.local_y;
        state.paint_has_last_local = true;

        TargetInfo info = target;
        info.component = format_paint_component(*state.paint_active_stroke_id);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeUpdate,
                       0.0f,
                       true);
    }

    void WidgetEventTrellisWorker::handle_paint_commit(WindowBinding const& binding,
                             PointerState& state,
                             bool inside) {
        if (!state.paint_active_widget || !state.paint_active_stroke_id) {
            reset_paint_state(state);
            return;
        }

        TargetInfo info{};
        info.widget_path = *state.paint_active_widget;
        info.component = format_paint_component(*state.paint_active_stroke_id);
        info.kind = TargetKind::PaintSurface;
        if (state.paint_has_last_local) {
            info.has_local = true;
            info.local_x = state.paint_last_local_x;
            info.local_y = state.paint_last_local_y;
        }

        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeCommit,
                       0.0f,
                       inside);
        reset_paint_state(state);
    }

    void WidgetEventTrellisWorker::handle_tree_press(PointerState& state, TargetInfo const& target) {
        state.tree_press_widget = target.widget_path;
        state.tree_press_node = target.tree_node_id;
        state.tree_press_toggle = (target.kind == TargetKind::TreeToggle);
    }

    void WidgetEventTrellisWorker::handle_tree_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.tree_press_widget || *state.tree_press_widget != target.widget_path) {
            return;
        }
        if (!state.tree_press_node || target.tree_node_id != state.tree_press_node) {
            state.tree_press_widget.reset();
            state.tree_press_node.reset();
            state.tree_press_toggle = false;
            return;
        }
        auto info = target;
        if (state.tree_press_toggle) {
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::TreeToggle,
                           0.0f,
                           true);
            if (info.tree_node_id) {
                DeclarativeDetail::ToggleTreeExpanded(space_, target.widget_path, *info.tree_node_id);
            }
        } else {
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::TreeSelect,
                           0.0f,
                           true);
            if (info.tree_node_id) {
            DeclarativeDetail::SetTreeSelectedNode(space_, target.widget_path, *info.tree_node_id);
            }
        }
        state.tree_press_widget.reset();
        state.tree_press_node.reset();
        state.tree_press_toggle = false;
    }

    void WidgetEventTrellisWorker::handle_text_focus(WindowBinding const& binding,
                           PointerState& state,
                           TargetInfo const& target) {
        state.text_focus_widget = target.widget_path;
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::TextFocus,
                       0.0f,
                       true);
    }

    auto WidgetEventTrellisWorker::resolve_target(WindowBinding const& binding,
                        PointerState const& state) -> std::optional<TargetInfo> {
        if (binding.scene_path.empty()) {
            return std::nullopt;
        }

        auto result = run_hit_test(binding, state);
        if (!result) {
            ++hit_test_failures_;
            return std::nullopt;
        }
        if (!result->hit) {
            return std::nullopt;
        }

        auto target = BuilderWidgets::ResolveHitTarget(*result);
        if (!target) {
            return std::nullopt;
        }

        TargetInfo info{};
        info.widget_path = target->widget.getPath();
        info.component = target->component;
        info.local_x = result->position.local_x;
        info.local_y = result->position.local_y;
        info.has_local = result->position.has_local;
        parse_component(info);
        if (!info.valid()) {
            return std::nullopt;
        }
        return info;
    }

    void WidgetEventTrellisWorker::update_hover(WindowBinding const& binding,
                      PointerState& state,
                      std::optional<TargetInfo> target) {
        bool changed = false;
        if (target && (!state.hover_target || state.hover_target->widget_path != target->widget_path)) {
            changed = true;
        } else if (!target && state.hover_target) {
            changed = true;
        }

        if (!changed) {
            return;
        }

        auto previous = state.hover_target;
        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverExit, 0.0f, false);
        }
        state.hover_target = target;
        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverEnter, 0.0f, true);
        }
        handle_hover_state(binding, state, previous, state.hover_target);
    }

void WidgetEventTrellisWorker::handle_hover_state(WindowBinding const& binding,
                            PointerState& state,
                            std::optional<TargetInfo> const& previous,
                            std::optional<TargetInfo> const& current) {
        (void)binding;
        if (previous) {
            switch (previous->kind) {
            case TargetKind::Button:
                DeclarativeDetail::SetButtonHovered(space_, previous->widget_path, false);
                break;
            case TargetKind::Toggle:
                DeclarativeDetail::SetToggleHovered(space_, previous->widget_path, false);
                break;
            case TargetKind::Slider:
                update_slider_hover(space_, previous->widget_path, false);
                break;
            case TargetKind::List:
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                DeclarativeDetail::SetListHoverIndex(space_, previous->widget_path, std::nullopt);
                break;
            case TargetKind::TreeRow:
            case TargetKind::TreeToggle:
                state.tree_hover_widget.reset();
                state.tree_hover_node.reset();
                DeclarativeDetail::SetTreeHoveredNode(space_, previous->widget_path, std::nullopt);
                break;
            default:
                break;
            }
        }

        if (!current) {
            return;
        }

        switch (current->kind) {
        case TargetKind::Button:
            DeclarativeDetail::SetButtonHovered(space_, current->widget_path, true);
            break;
        case TargetKind::Toggle:
            DeclarativeDetail::SetToggleHovered(space_, current->widget_path, true);
            break;
        case TargetKind::Slider:
            update_slider_hover(space_, current->widget_path, true);
            break;
        case TargetKind::List: {
            if (!current->has_local) {
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                DeclarativeDetail::SetListHoverIndex(space_, current->widget_path, std::nullopt);
                break;
            }
            auto data = read_list_data(space_, current->widget_path);
            if (!data) {
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                break;
            }
            auto index = list_index_from_local(*data, current->local_y);
            if (state.list_hover_widget == current->widget_path && state.list_hover_index == index) {
                break;
            }
            state.list_hover_widget = current->widget_path;
            state.list_hover_index = index;
            DeclarativeDetail::SetListHoverIndex(space_, current->widget_path, index);
            if (index) {
                auto hover_target = *current;
                hover_target.component = "list/item/" + std::to_string(*index);
                emit_widget_op(binding,
                               hover_target,
                               WidgetBindings::WidgetOpKind::ListHover,
                               static_cast<float>(*index),
                               true);
            }
            break;
        }
        case TargetKind::TreeRow:
        case TargetKind::TreeToggle:
            if (state.tree_hover_widget == current->widget_path
                && state.tree_hover_node == current->tree_node_id) {
                break;
            }
            state.tree_hover_widget = current->widget_path;
            state.tree_hover_node = current->tree_node_id;
            DeclarativeDetail::SetTreeHoveredNode(space_, current->widget_path, current->tree_node_id);
            if (current->tree_node_id) {
                emit_widget_op(binding,
                               *current,
                               WidgetBindings::WidgetOpKind::TreeHover,
                               0.0f,
                               true);
            }
            break;
        default:
            break;
        }
    }

    void WidgetEventTrellisWorker::parse_component(TargetInfo& info) {
        if (info.component.empty()) {
            info.kind = TargetKind::Unknown;
            return;
        }
        std::vector<std::string> parts;
        std::string current;
        for (char ch : info.component) {
            if (ch == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(ch);
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }
        if (parts.empty()) {
            info.kind = TargetKind::Unknown;
            return;
        }
        auto const& prefix = parts.front();
        if (prefix == "button") {
            info.kind = TargetKind::Button;
            return;
        }
        if (prefix == "toggle") {
            info.kind = TargetKind::Toggle;
            return;
        }
        if (prefix == "slider") {
            info.kind = TargetKind::Slider;
            return;
        }
        if (prefix == "list") {
            info.kind = TargetKind::List;
            if (parts.size() >= 3 && parts[1] == "item") {
                info.list_item_id = parts[2];
                try {
                    info.list_index = std::stoi(parts[2]);
                } catch (...) {
                    info.list_index.reset();
                }
            }
            return;
        }
        if (prefix == "tree") {
            if (parts.size() >= 3 && parts[1] == "toggle") {
                info.kind = TargetKind::TreeToggle;
                info.tree_node_id = parts[2];
                return;
            }
            if (parts.size() >= 3 && parts[1] == "row") {
                info.kind = TargetKind::TreeRow;
                info.tree_node_id = parts[2];
                return;
            }
            info.kind = TargetKind::TreeRow;
            return;
        }
        if (prefix == "stack") {
            info.kind = TargetKind::StackPanel;
            if (parts.size() >= 3 && parts[1] == "child") {
                info.stack_panel_id = parts[2];
            }
            return;
        }
        if (prefix == "input_field") {
            info.kind = TargetKind::InputField;
            return;
        }
        if (prefix == "paint_surface") {
            info.kind = TargetKind::PaintSurface;
            return;
        }
        info.kind = TargetKind::Unknown;
    }

} // namespace SP::UI::Declarative
