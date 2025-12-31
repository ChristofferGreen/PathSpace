#include <pathspace/ui/scenegraph/RenderCommandStore.hpp>

#include <algorithm>

namespace SP::UI::SceneGraph {

auto RenderCommandStore::ensure_slot() -> CommandId {
    if (!free_list_.empty()) {
        auto const id = free_list_.back();
        free_list_.pop_back();
        return id;
    }
    auto const id = static_cast<CommandId>(bboxes_.size());
    bboxes_.push_back(IntRect{});
    z_.push_back(0);
    opacity_.push_back(1.0f);
    kind_.push_back(Scene::DrawCommandKind::Rect);
    payload_.push_back(0);
    entity_.push_back(0);
    active_.push_back(false);
    return id;
}

auto RenderCommandStore::valid(CommandId id) const -> bool {
    return id < active_.size() && active_[id];
}

auto RenderCommandStore::upsert(CommandDescriptor const& command) -> UpsertResult {
    auto const found = entity_index_.find(command.entity_id);
    CommandId  id{};
    bool       replaced = false;
    std::optional<IntRect> previous_bbox{};

    if (found != entity_index_.end()) {
        id = found->second;
        replaced = true;
        previous_bbox = bboxes_[id];
    } else {
        id = ensure_slot();
        entity_index_[command.entity_id] = id;
        active_[id] = true;
        ++active_count_;
    }

    bboxes_[id] = command.bbox;
    z_[id] = command.z;
    opacity_[id] = command.opacity;
    kind_[id] = command.kind;
    payload_[id] = command.payload_handle;
    entity_[id] = command.entity_id;

    return UpsertResult{.id = id, .replaced = replaced, .previous_bbox = previous_bbox};
}

auto RenderCommandStore::remove_entity(std::uint64_t entity_id) -> std::optional<IntRect> {
    auto const found = entity_index_.find(entity_id);
    if (found == entity_index_.end()) {
        return std::nullopt;
    }
    auto const id = found->second;
    if (!valid(id)) {
        entity_index_.erase(found);
        return std::nullopt;
    }

    entity_index_.erase(found);
    active_[id] = false;
    free_list_.push_back(id);
    --active_count_;
    return bboxes_[id];
}

auto RenderCommandStore::clear() -> void {
    bboxes_.clear();
    z_.clear();
    opacity_.clear();
    kind_.clear();
    payload_.clear();
    entity_.clear();
    active_.clear();
    entity_index_.clear();
    free_list_.clear();
    active_count_ = 0;
}

auto RenderCommandStore::entity_index(std::uint64_t entity_id) const -> std::optional<CommandId> {
    auto const found = entity_index_.find(entity_id);
    if (found == entity_index_.end()) {
        return std::nullopt;
    }
    return found->second;
}

auto RenderCommandStore::bbox(CommandId id) const -> IntRect const& {
    assert(valid(id));
    return bboxes_[id];
}

auto RenderCommandStore::z(CommandId id) const -> int32_t {
    assert(valid(id));
    return z_[id];
}

auto RenderCommandStore::opacity(CommandId id) const -> float {
    assert(valid(id));
    return opacity_[id];
}

auto RenderCommandStore::kind(CommandId id) const -> Scene::DrawCommandKind {
    assert(valid(id));
    return kind_[id];
}

auto RenderCommandStore::payload_handle(CommandId id) const -> std::uint64_t {
    assert(valid(id));
    return payload_[id];
}

auto RenderCommandStore::entity_id(CommandId id) const -> std::uint64_t {
    assert(valid(id));
    return entity_[id];
}

auto RenderCommandStore::active_ids() const -> std::vector<CommandId> {
    std::vector<CommandId> ids;
    ids.reserve(active_count_);
    for (CommandId id = 0; id < active_.size(); ++id) {
        if (active_[id]) {
            ids.push_back(id);
        }
    }
    return ids;
}

} // namespace SP::UI::SceneGraph
