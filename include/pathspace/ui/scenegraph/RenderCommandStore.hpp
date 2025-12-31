#pragma once

#include <pathspace/ui/DrawCommands.hpp>

#include <parallel_hashmap/phmap.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace SP::UI::SceneGraph {

struct IntRect {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;

    [[nodiscard]] auto empty() const -> bool {
        return min_x >= max_x || min_y >= max_y;
    }
};

using CommandId = std::uint32_t;

struct CommandDescriptor {
    IntRect                 bbox{};
    int32_t                 z = 0;
    float                   opacity = 1.0f;
    Scene::DrawCommandKind  kind = Scene::DrawCommandKind::Rect;
    std::uint64_t           payload_handle = 0;
    std::uint64_t           entity_id = 0;
};

struct UpsertResult {
    CommandId                id = 0;
    bool                     replaced = false;
    std::optional<IntRect>   previous_bbox;
};

class RenderCommandStore {
public:
    RenderCommandStore() = default;

    auto upsert(CommandDescriptor const& command) -> UpsertResult;
    auto remove_entity(std::uint64_t entity_id) -> std::optional<IntRect>;
    auto clear() -> void;

    [[nodiscard]] auto active_count() const -> std::size_t {
        return active_count_;
    }

    [[nodiscard]] auto entity_index(std::uint64_t entity_id) const -> std::optional<CommandId>;

    [[nodiscard]] auto bbox(CommandId id) const -> IntRect const&;
    [[nodiscard]] auto z(CommandId id) const -> int32_t;
    [[nodiscard]] auto opacity(CommandId id) const -> float;
    [[nodiscard]] auto kind(CommandId id) const -> Scene::DrawCommandKind;
    [[nodiscard]] auto payload_handle(CommandId id) const -> std::uint64_t;
    [[nodiscard]] auto entity_id(CommandId id) const -> std::uint64_t;

    [[nodiscard]] auto active_ids() const -> std::vector<CommandId>;

private:
    [[nodiscard]] auto ensure_slot() -> CommandId;
    [[nodiscard]] auto valid(CommandId id) const -> bool;

    std::vector<IntRect>                bboxes_{};
    std::vector<int32_t>                z_{};
    std::vector<float>                  opacity_{};
    std::vector<Scene::DrawCommandKind> kind_{};
    std::vector<std::uint64_t>          payload_{};
    std::vector<std::uint64_t>          entity_{};
    std::vector<bool>                   active_{};

    phmap::flat_hash_map<std::uint64_t, CommandId> entity_index_{};
    std::vector<CommandId>                         free_list_{};
    std::size_t                                    active_count_ = 0;
};

} // namespace SP::UI::SceneGraph
