#include "WidgetStateMutators.hpp"

#include "WidgetEventCommon.hpp"
#include <pathspace/ui/declarative/Detail.hpp>

#include <pathspace/ui/BuildersShared.hpp>

#include <algorithm>
#include <optional>
#include <string>

namespace SP::UI::Declarative::Detail {

namespace BuilderWidgets = SP::UI::Builders::Widgets;

namespace {

template <typename State, typename MutateFn>
auto mutate_widget_state(PathSpace& space,
                         std::string const& widget_path,
                         std::string_view state_name,
                         MutateFn&& mutate) -> bool {
    auto state_path = widget_path + "/state";
    State updated{};
    auto stored = space.read<State, std::string>(state_path);
    if (!stored) {
        auto const& error = stored.error();
        if (error.code != SP::Error::Code::NoObjectFound
            && error.code != SP::Error::Code::NoSuchPath) {
            enqueue_error(space,
                          "WidgetEventTrellis failed to read "
                              + std::string(state_name) + " for " + widget_path);
            return false;
        }
    } else {
        updated = *stored;
    }

    bool changed = mutate(updated);
    if (!changed) {
        return true;
    }

    auto status = replace_single<State>(space, state_path, updated);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write "
                          + std::string(state_name) + " for " + widget_path);
        return false;
    }

    mark_widget_dirty(space, widget_path);
    return true;
}

} // namespace

auto SetButtonHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void {
    (void)mutate_widget_state<BuilderWidgets::ButtonState>(space,
                                                           widget_path,
                                                           "button state",
                                                           [hovered](auto& state) {
                                                               if (!state.enabled
                                                                   || state.hovered == hovered) {
                                                                   return false;
                                                               }
                                                               state.hovered = hovered;
                                                               if (!hovered) {
                                                                   state.pressed = false;
                                                               }
                                                               return true;
                                                           });
}

auto SetButtonPressed(PathSpace& space,
                      std::string const& widget_path,
                      bool pressed) -> void {
    (void)mutate_widget_state<BuilderWidgets::ButtonState>(space,
                                                           widget_path,
                                                           "button state",
                                                           [pressed](auto& state) {
                                                               if (!state.enabled
                                                                   || state.pressed == pressed) {
                                                                   return false;
                                                               }
                                                               state.pressed = pressed;
                                                               return true;
                                                           });
}

auto SetToggleHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void {
    (void)mutate_widget_state<BuilderWidgets::ToggleState>(space,
                                                           widget_path,
                                                           "toggle state",
                                                           [hovered](auto& state) {
                                                               if (!state.enabled
                                                                   || state.hovered == hovered) {
                                                                   return false;
                                                               }
                                                               state.hovered = hovered;
                                                               return true;
                                                           });
}

auto ToggleToggleChecked(PathSpace& space,
                         std::string const& widget_path) -> void {
    (void)mutate_widget_state<BuilderWidgets::ToggleState>(space,
                                                           widget_path,
                                                           "toggle state",
                                                           [](auto& state) {
                                                               if (!state.enabled) {
                                                                   return false;
                                                               }
                                                               state.checked = !state.checked;
                                                               return true;
                                                           });
}

auto SetListHoverIndex(PathSpace& space,
                       std::string const& widget_path,
                       std::optional<std::int32_t> index) -> void {
    (void)mutate_widget_state<BuilderWidgets::ListState>(space,
                                                         widget_path,
                                                         "list state",
                                                         [&index](auto& state) {
                                                             if (!state.enabled
                                                             || (state.hovered_index == index.value_or(-1))) {
                                                                return false;
                                                            }
                                                             state.hovered_index = index.value_or(-1);
                                                             return true;
                                                         });
}

auto SetListSelectionIndex(PathSpace& space,
                           std::string const& widget_path,
                           std::int32_t index) -> void {
    (void)mutate_widget_state<BuilderWidgets::ListState>(space,
                                                         widget_path,
                                                         "list state",
                                                         [index](auto& state) {
                                                             if (!state.enabled
                                                                 || state.selected_index == index) {
                                                                 return false;
                                                             }
                                                             state.selected_index = index;
                                                             return true;
                                                         });
}

auto SetTreeHoveredNode(PathSpace& space,
                        std::string const& widget_path,
                        std::optional<std::string> node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             if (!state.enabled
                                                             || state.hovered_id == node_id.value_or(std::string{})) {
                                                                return false;
                                                            }
                                                             state.hovered_id = node_id.value_or(std::string{});
                                                             return true;
                                                         });
}

auto SetTreeSelectedNode(PathSpace& space,
                         std::string const& widget_path,
                         std::string const& node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             if (!state.enabled
                                                                 || state.selected_id == node_id) {
                                                                 return false;
                                                             }
                                                             state.selected_id = node_id;
                                                             return true;
                                                         });
}

auto ToggleTreeExpanded(PathSpace& space,
                        std::string const& widget_path,
                        std::string const& node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             if (!state.enabled) {
                                                                 return false;
                                                             }
                                                             auto it = std::find(state.expanded_ids.begin(),
                                                                                state.expanded_ids.end(),
                                                                                node_id);
                                                             if (it == state.expanded_ids.end()) {
                                                                 state.expanded_ids.push_back(node_id);
                                                             } else {
                                                                 state.expanded_ids.erase(it);
                                                             }
                                                             return true;
                                                         });
}

} // namespace SP::UI::Declarative::Detail
