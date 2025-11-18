#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace SP {
class PathSpace;
}

namespace SP::UI::Declarative::Detail {

auto SetButtonHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void;

auto SetButtonPressed(PathSpace& space,
                      std::string const& widget_path,
                      bool pressed) -> void;

auto SetToggleHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void;

auto ToggleToggleChecked(PathSpace& space,
                         std::string const& widget_path) -> void;

auto SetListHoverIndex(PathSpace& space,
                        std::string const& widget_path,
                        std::optional<std::int32_t> index) -> void;

auto SetListSelectionIndex(PathSpace& space,
                            std::string const& widget_path,
                            std::int32_t index) -> void;

auto SetTreeHoveredNode(PathSpace& space,
                         std::string const& widget_path,
                         std::optional<std::string> node_id) -> void;

auto SetTreeSelectedNode(PathSpace& space,
                          std::string const& widget_path,
                          std::string const& node_id) -> void;

auto ToggleTreeExpanded(PathSpace& space,
                         std::string const& widget_path,
                         std::string const& node_id) -> void;

} // namespace SP::UI::Declarative::Detail
