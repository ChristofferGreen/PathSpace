#include "WidgetEventTrellisWorker.hpp"

#include "WidgetStateMutators.hpp"
#include "widgets/Common.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace SP::UI::Declarative {

namespace DeclarativeDetail = SP::UI::Declarative::Detail;

void WidgetEventTrellisWorker::handle_button_event(WindowBinding const& binding,
                             SP::IO::ButtonEvent const& event) {
        switch (event.source) {
        case SP::IO::ButtonSource::Mouse:
            handle_mouse_button_event(binding, event);
            break;
        case SP::IO::ButtonSource::Keyboard:
        case SP::IO::ButtonSource::Gamepad:
            if (!handle_focus_nav_event(binding, event)) {
                handle_focus_button_event(binding, event);
            }
            break;
        default:
            break;
        }
    }

    bool WidgetEventTrellisWorker::handle_focus_nav_event(WindowBinding const& binding,
                                SP::IO::ButtonEvent const& event) {
        auto nav = classify_focus_nav(event);
        if (!nav) {
            return false;
        }
        auto focused = focused_widget_path(space_, binding);
        if (!focused || focused->empty()) {
            return false;
        }
        auto target = focus_target_for_widget(*focused);
        if (!target) {
            return false;
        }

        switch (target->kind) {
        case TargetKind::Slider:
            return handle_slider_focus_nav(binding, *target, *nav);
        case TargetKind::List:
            if (nav->direction != FocusDirection::None) {
                return handle_list_focus_nav(binding, *target, *nav);
            }
            if (nav->command == FocusCommand::Submit) {
                return handle_list_submit(binding, *target);
            }
            return false;
        case TargetKind::TreeRow:
        case TargetKind::TreeToggle:
            if (nav->direction != FocusDirection::None) {
                return handle_tree_focus_nav(binding, *target, *nav);
            }
            return false;
        case TargetKind::InputField:
            return handle_text_focus_nav(binding, *target, *nav);
        default:
            return false;
        }
    }

    bool WidgetEventTrellisWorker::handle_slider_focus_nav(WindowBinding const& binding,
                                 TargetInfo const& target,
                                 FocusNavEvent const& nav) {
        int step_direction = 0;
        switch (nav.direction) {
        case FocusDirection::Left:
        case FocusDirection::Down:
            step_direction = -1;
            break;
        case FocusDirection::Right:
        case FocusDirection::Up:
            step_direction = 1;
            break;
        default:
            return false;
        }
        auto data = read_slider_data(space_, target.widget_path);
        if (!data) {
            return false;
        }
        float step = slider_step_size(*data);
        float next_value = clamp_slider_value(*data,
                                              data->state.value + static_cast<float>(step_direction) * step);
        if (std::abs(next_value - data->state.value) < 1e-6f) {
            return false;
        }
        data->state.dragging = false;
        data->state.value = next_value;
        if (!write_slider_state(space_, target.widget_path, data->state)) {
            return false;
        }

        TargetInfo info = target;
        info.kind = TargetKind::Slider;
        info.component = "slider/thumb";
        info.has_local = true;
        info.local_x = slider_local_from_value(*data, next_value);
        info.local_y = std::max(data->style.height, 1.0f) * 0.5f;
        auto pointer = focus_pointer_with_local(info.local_x, info.local_y);

        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::SliderUpdate,
                       next_value,
                       true,
                       pointer);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::SliderCommit,
                       next_value,
                       true,
                       pointer);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_list_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav) {
        int delta = 0;
        if (nav.direction == FocusDirection::Up) {
            delta = -1;
        } else if (nav.direction == FocusDirection::Down) {
            delta = 1;
        } else {
            return false;
        }

        auto data = read_list_data(space_, target.widget_path);
        if (!data || data->items.empty()) {
            return false;
        }

        std::int32_t current = data->state.selected_index;
        if (current < 0) {
            current = data->state.hovered_index >= 0 ? data->state.hovered_index : 0;
        }
        auto max_index = static_cast<std::int32_t>(data->items.size()) - 1;
        auto next = std::clamp(current + delta, 0, std::max<std::int32_t>(0, max_index));
        if (next == current) {
            return false;
        }

        DeclarativeDetail::SetListHoverIndex(space_, target.widget_path, next);
        DeclarativeDetail::SetListSelectionIndex(space_, target.widget_path, next);

        auto [local_x, local_y] = list_local_center(*data, next);
        TargetInfo info{};
        info.widget_path = target.widget_path;
        info.kind = TargetKind::List;
        info.component = "list/item/" + std::to_string(next);
        info.list_index = next;
        info.list_item_id = list_item_id(*data, next);
        auto pointer = focus_pointer_with_local(local_x, local_y);

        {
            auto& state = pointer_state(binding.token);
            state.list_hover_widget = target.widget_path;
            state.list_hover_index = next;
        }

        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListHover,
                       static_cast<float>(next),
                       true,
                       pointer);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListSelect,
                       static_cast<float>(next),
                       true,
                       pointer);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_list_submit(WindowBinding const& binding,
                            TargetInfo const& target) {
        auto data = read_list_data(space_, target.widget_path);
        if (!data) {
            return false;
        }
        std::int32_t index = data->state.selected_index >= 0
            ? data->state.selected_index
            : data->state.hovered_index;
        if (index < 0 || index >= static_cast<std::int32_t>(data->items.size())) {
            return false;
        }
        auto [local_x, local_y] = list_local_center(*data, index);
        TargetInfo info{};
        info.widget_path = target.widget_path;
        info.kind = TargetKind::List;
        info.component = "list/item/" + std::to_string(index);
        info.list_index = index;
        info.list_item_id = list_item_id(*data, index);
        auto pointer = focus_pointer_with_local(local_x, local_y);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListActivate,
                       static_cast<float>(index),
                       true,
                       pointer);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_tree_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav) {
        auto data = read_tree_data(space_, target.widget_path);
        if (!data) {
            return false;
        }
        auto rows = build_tree_rows(*data);
        if (rows.empty()) {
            return false;
        }

        std::string current_id = data->state.selected_id;
        auto idx_opt = tree_row_index(rows, current_id);
        std::size_t idx = idx_opt.value_or(0);
        if (!idx_opt) {
            select_tree_row(binding, target.widget_path, rows[idx].id);
        }

        auto select_if_valid = [&](std::size_t new_index) -> bool {
            if (new_index >= rows.size()) {
                return false;
            }
            return select_tree_row(binding, target.widget_path, rows[new_index].id);
        };

        switch (nav.direction) {
        case FocusDirection::Up:
            if (idx == 0) {
                return false;
            }
            return select_if_valid(idx - 1);
        case FocusDirection::Down:
            if (idx + 1 >= rows.size()) {
                return false;
            }
            return select_if_valid(idx + 1);
        case FocusDirection::Left:
            if (rows[idx].expandable && rows[idx].expanded) {
                DeclarativeDetail::ToggleTreeExpanded(space_, target.widget_path, rows[idx].id);
                return emit_tree_toggle(binding, target.widget_path, rows[idx].id);
            }
            if (!rows[idx].parent_id.empty()) {
                if (auto parent_idx = tree_row_index(rows, rows[idx].parent_id)) {
                    return select_if_valid(*parent_idx);
                }
            }
            return false;
        case FocusDirection::Right:
            if (rows[idx].expandable && !rows[idx].expanded) {
                DeclarativeDetail::ToggleTreeExpanded(space_, target.widget_path, rows[idx].id);
                return emit_tree_toggle(binding, target.widget_path, rows[idx].id);
            }
            if (rows[idx].expandable && rows[idx].expanded) {
                if (idx + 1 < rows.size() && rows[idx + 1].depth == rows[idx].depth + 1) {
                    return select_if_valid(idx + 1);
                }
            }
            return false;
        default:
            return false;
        }
    }

    bool WidgetEventTrellisWorker::select_tree_row(WindowBinding const& binding,
                         std::string const& widget_path,
                         std::string const& node_id) {
        TargetInfo info{};
        info.widget_path = widget_path;
        info.kind = TargetKind::TreeRow;
        info.component = "tree/row/" + node_id;
        info.tree_node_id = node_id;

        DeclarativeDetail::SetTreeHoveredNode(space_, widget_path, node_id);
        DeclarativeDetail::SetTreeSelectedNode(space_, widget_path, node_id);

        {
            auto& state = pointer_state(binding.token);
            state.tree_hover_widget = widget_path;
            state.tree_hover_node = node_id;
        }

        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TreeHover,
                       0.0f,
                       true);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TreeSelect,
                       0.0f,
                       true);
        return true;
    }

    bool WidgetEventTrellisWorker::emit_tree_toggle(WindowBinding const& binding,
                          std::string const& widget_path,
                          std::string const& node_id) {
        TargetInfo info{};
        info.widget_path = widget_path;
        info.kind = TargetKind::TreeToggle;
        info.component = "tree/toggle/" + node_id;
        info.tree_node_id = node_id;
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TreeToggle,
                       0.0f,
                       true);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_text_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav) {
        switch (nav.direction) {
        case FocusDirection::Left:
            return handle_text_cursor_step(binding, target, -1);
        case FocusDirection::Right:
            return handle_text_cursor_step(binding, target, 1);
        case FocusDirection::Up:
            return handle_text_cursor_step(binding, target, -1);
        case FocusDirection::Down:
            return handle_text_cursor_step(binding, target, 1);
        default:
            break;
        }

        switch (nav.command) {
        case FocusCommand::DeleteBackward:
            return handle_text_delete(binding, target, false);
        case FocusCommand::DeleteForward:
            return handle_text_delete(binding, target, true);
        case FocusCommand::Submit:
            return handle_text_submit(binding, target);
        case FocusCommand::None:
            return false;
        }
        return false;
    }

    bool WidgetEventTrellisWorker::handle_text_cursor_step(WindowBinding const& binding,
                                 TargetInfo const& target,
                                 int delta) {
        if (delta == 0) {
            return false;
        }
        auto state = read_text_state(space_, target.widget_path);
        if (!state) {
            return false;
        }
        std::uint32_t start = std::min(state->selection_start, state->selection_end);
        std::uint32_t end = std::max(state->selection_start, state->selection_end);
        bool changed = false;
        if (start != end) {
            state->cursor = (delta < 0) ? start : end;
            changed = true;
        } else {
            auto cursor = static_cast<std::int64_t>(state->cursor);
            cursor += delta;
            auto max_cursor = static_cast<std::int64_t>(state->text.size());
            auto clamped = std::clamp(cursor,
                                      static_cast<std::int64_t>(0),
                                      max_cursor);
            if (static_cast<std::uint32_t>(clamped) == state->cursor) {
                return false;
            }
            state->cursor = static_cast<std::uint32_t>(clamped);
            changed = true;
        }
        state->selection_start = state->cursor;
        state->selection_end = state->cursor;
        if (!changed || !write_text_state(space_, target.widget_path, *state)) {
            return false;
        }

        TargetInfo info = target;
        info.kind = TargetKind::InputField;
        info.component = "input_field/text";
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TextMoveCursor,
                       static_cast<float>(delta),
                       true);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_text_delete(WindowBinding const& binding,
                            TargetInfo const& target,
                            bool forward) {
        auto state = read_text_state(space_, target.widget_path);
        if (!state) {
            return false;
        }

        std::uint32_t start = std::min(state->selection_start, state->selection_end);
        std::uint32_t end = std::max(state->selection_start, state->selection_end);
        bool changed = false;
        std::size_t text_size = state->text.size();

        if (start != end) {
            if (start < text_size) {
                auto count = std::min<std::size_t>(text_size - static_cast<std::size_t>(start),
                                                   static_cast<std::size_t>(end - start));
                state->text.erase(static_cast<std::size_t>(start), count);
            }
            state->cursor = start;
            changed = true;
        } else if (!forward && state->cursor > 0) {
            auto erase_index = static_cast<std::size_t>(state->cursor - 1);
            if (erase_index < text_size) {
                state->text.erase(erase_index, 1);
                state->cursor = static_cast<std::uint32_t>(erase_index);
                changed = true;
            }
        } else if (forward && state->cursor < text_size) {
            state->text.erase(static_cast<std::size_t>(state->cursor), 1);
            changed = true;
        }

        if (!changed) {
            return false;
        }
        state->selection_start = state->cursor;
        state->selection_end = state->cursor;
        if (!write_text_state(space_, target.widget_path, *state)) {
            return false;
        }

        TargetInfo info = target;
        info.kind = TargetKind::InputField;
        info.component = "input_field/text";
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TextDelete,
                       forward ? 1.0f : -1.0f,
                       true);
        return true;
    }

    bool WidgetEventTrellisWorker::handle_text_submit(WindowBinding const& binding,
                            TargetInfo const& target) {
        TargetInfo info = target;
        info.kind = TargetKind::InputField;
        info.component = "input_field/text";
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TextSubmit,
                       1.0f,
                       true);
        return true;
    }

    void WidgetEventTrellisWorker::handle_focus_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event) {
        auto& state = pointer_state(binding.token);
        bool pressed = event.state.pressed;
        auto focused = focused_widget_path(space_, binding);

        if (pressed) {
            if (!focused || focused->empty()) {
                return;
            }
            auto target = focus_target_for_widget(*focused);
            if (!target) {
                return;
            }
            if (state.focus_press_target
                && state.focus_press_target->widget_path == target->widget_path
                && state.focus_press_target->kind == target->kind) {
                return;
            }
            switch (target->kind) {
            case TargetKind::Button:
                state.focus_press_target = target;
                DeclarativeDetail::SetButtonPressed(space_, target->widget_path, true);
                emit_widget_op(binding,
                               *target,
                               WidgetBindings::WidgetOpKind::Press,
                               1.0f,
                               true);
                break;
            case TargetKind::Toggle:
                state.focus_press_target = target;
                emit_widget_op(binding,
                               *target,
                               WidgetBindings::WidgetOpKind::Press,
                               1.0f,
                               true);
                break;
            default:
                break;
            }
            return;
        }

        if (!state.focus_press_target) {
            return;
        }

        auto target = *state.focus_press_target;
        state.focus_press_target.reset();
        bool inside = focused && !focused->empty() && *focused == target.widget_path;

        switch (target.kind) {
        case TargetKind::Button:
            emit_widget_op(binding,
                           target,
                           WidgetBindings::WidgetOpKind::Release,
                           0.0f,
                           inside);
            DeclarativeDetail::SetButtonPressed(space_, target.widget_path, false);
            if (inside) {
                emit_widget_op(binding,
                               target,
                               WidgetBindings::WidgetOpKind::Activate,
                               1.0f,
                               true);
            }
            break;
        case TargetKind::Toggle:
            emit_widget_op(binding,
                           target,
                           WidgetBindings::WidgetOpKind::Release,
                           0.0f,
                           inside);
            if (inside) {
                emit_widget_op(binding,
                               target,
                               WidgetBindings::WidgetOpKind::Toggle,
                               1.0f,
                               true);
                DeclarativeDetail::ToggleToggleChecked(space_, target.widget_path);
            }
            break;
        default:
            break;
        }
    }

    auto WidgetEventTrellisWorker::focus_target_for_widget(std::string const& widget_path) -> std::optional<TargetInfo> {
        auto kind = space_.read<std::string, std::string>(SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, "/meta/kind"));
        if (!kind) {
            auto const& error = kind.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                enqueue_error(space_, "WidgetEventTrellis failed to read widget kind for "
                        + SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, ": ") + error.message.value_or("unknown error"));
            }
            return std::nullopt;
        }
        TargetInfo info{};
        info.widget_path = widget_path;
        info.component = *kind + "/focus";
        parse_component(info);
        if (!info.valid()) {
            enqueue_error(space_, "WidgetEventTrellis could not derive focus target for "
                    + SP::UI::Runtime::Widgets::WidgetSpacePath(widget_path, " (kind=") + *kind + ")");
            return std::nullopt;
        }
        return info;
    }

    void WidgetEventTrellisWorker::handle_text_event(WindowBinding const& binding,
                           SP::IO::TextEvent const& event) {
        auto& state = pointer_state(binding.token);
        std::optional<std::string> target_widget = state.text_focus_widget;
        if (!target_widget) {
            target_widget = focused_widget_path(space_, binding);
        }
        if (!target_widget || target_widget->empty()) {
            return;
        }
        TargetInfo info{};
        info.widget_path = *target_widget;
        info.component = "input_field/text";
        info.kind = TargetKind::InputField;
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TextInput,
                       static_cast<float>(event.codepoint),
                       true);
    }

} // namespace SP::UI::Declarative
