#include "BuildersDetail.hpp"

namespace SP::UI::Builders::Scene {

using namespace Detail;

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SceneParams const& params) -> SP::Expected<ScenePath> {
    if (auto status = ensure_identifier(params.name, "scene name"); !status) {
        return std::unexpected(status.error());
    }

    auto resolved = combine_relative(appRoot, std::string("scenes/") + params.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto metaNamePath = make_scene_meta(ScenePath{resolved->getPath()}, "name");
    auto existing = read_optional<std::string>(space, metaNamePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return ScenePath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, metaNamePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    auto metaDescPath = make_scene_meta(ScenePath{resolved->getPath()}, "description");
    if (auto status = replace_single<std::string>(space, metaDescPath, params.description); !status) {
        return std::unexpected(status.error());
    }

    return ScenePath{resolved->getPath()};
}

auto EnsureAuthoringRoot(PathSpace& /*space*/,
                          ScenePath const& scenePath) -> SP::Expected<void> {
    if (!scenePath.isValid()) {
        return std::unexpected(make_error("scene path is not valid",
                                          SP::Error::Code::InvalidPath));
    }
    if (auto status = ensure_contains_segment(ConcretePathView{scenePath.getPath()}, kScenesSegment); !status) {
        return status;
    }
    return {};
}

auto PublishRevision(PathSpace& space,
                      ScenePath const& scenePath,
                      SceneRevisionDesc const& revision,
                      std::span<std::byte const> drawableBucket,
                      std::span<std::byte const> metadata) -> SP::Expected<void> {
    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return status;
    }

    auto record = to_record(revision);
    auto revisionStr = format_revision(revision.revision);
    auto revisionBase = make_revision_base(scenePath, revisionStr);

    if (auto status = replace_single<SceneRevisionRecord>(space, revisionBase + "/desc", record); !status) {
        return status;
    }
    if (auto status = replace_single<std::vector<std::uint8_t>>(space,
                                                                revisionBase + "/drawable_bucket",
                                                                bytes_from_span(drawableBucket)); !status) {
        return status;
    }
    if (auto status = replace_single<std::vector<std::uint8_t>>(space,
                                                                revisionBase + "/metadata",
                                                                bytes_from_span(metadata)); !status) {
        return status;
    }

    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    if (auto status = replace_single<uint64_t>(space, currentRevisionPath, revision.revision); !status) {
        return status;
    }

    return {};
}

auto ReadCurrentRevision(PathSpace const& space,
                          ScenePath const& scenePath) -> SP::Expected<SceneRevisionDesc> {
    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    auto revisionValue = read_value<uint64_t>(space, currentRevisionPath);
    if (!revisionValue) {
        return std::unexpected(revisionValue.error());
    }

    auto revisionStr = format_revision(*revisionValue);
    auto descPath = make_revision_base(scenePath, revisionStr) + "/desc";
    auto record = read_value<SceneRevisionRecord>(space, descPath);
    if (!record) {
        return std::unexpected(record.error());
    }
    return from_record(*record);
}

auto WaitUntilReady(PathSpace& space,
                     ScenePath const& scenePath,
                     std::chrono::milliseconds timeout) -> SP::Expected<void> {
    auto currentRevisionPath = std::string(scenePath.getPath()) + "/current_revision";
    auto result = read_value<uint64_t>(space, currentRevisionPath, SP::Out{} & SP::Block{timeout});
    if (!result) {
        return std::unexpected(result.error());
    }
    (void)result;
    return {};
}

auto HitTest(PathSpace& space,
             ScenePath const& scenePath,
             HitTestRequest const& request) -> SP::Expected<HitTestResult> {
    auto sceneRoot = derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }

    auto revision = ReadCurrentRevision(space, scenePath);
    if (!revision) {
        return std::unexpected(revision.error());
    }

    auto revisionStr = format_revision(revision->revision);
    auto revisionBase = make_revision_base(scenePath, revisionStr);
    auto bucket = SP::UI::Scene::SceneSnapshotBuilder::decode_bucket(space, revisionBase);
    if (!bucket) {
        return std::unexpected(bucket.error());
    }

    std::optional<std::string> auto_render_target;
    if (request.schedule_render) {
        if (!request.auto_render_target) {
            return std::unexpected(make_error("auto render target required when scheduling render",
                                              SP::Error::Code::InvalidPath));
        }
        auto targetRoot = derive_app_root_for(ConcretePathView{request.auto_render_target->getPath()});
        if (!targetRoot) {
            return std::unexpected(targetRoot.error());
        }
        if (targetRoot->getPath() != sceneRoot->getPath()) {
            return std::unexpected(make_error("auto render target must belong to the same application as the scene",
                                              SP::Error::Code::InvalidPath));
        }
        auto_render_target = request.auto_render_target->getPath();
    }

    auto order = detail::build_draw_order(*bucket);
    HitTestResult result{};
    auto max_results = request.max_results == 0 ? std::size_t{1} : request.max_results;
    bool const enqueue_render = request.schedule_render && auto_render_target.has_value();
    bool render_enqueued = false;

    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        std::size_t drawable_index = *it;
        if (drawable_index >= bucket->drawable_ids.size()) {
            continue;
        }
        if (drawable_index < bucket->visibility.size()
            && bucket->visibility[drawable_index] == 0) {
            continue;
        }
        if (!detail::point_inside_clip(request.x, request.y, *bucket, drawable_index)) {
            continue;
        }
        if (!detail::point_inside_bounds(request.x, request.y, *bucket, drawable_index)) {
            continue;
        }

        HitCandidate candidate{};
        candidate.target.drawable_id = bucket->drawable_ids[drawable_index];

        if (drawable_index < bucket->authoring_map.size()) {
            auto const& author = bucket->authoring_map[drawable_index];
            candidate.target.authoring_node_id = author.authoring_node_id;
            candidate.target.drawable_index_within_node = author.drawable_index_within_node;
            candidate.target.generation = author.generation;
            candidate.focus_chain = detail::build_focus_chain(author.authoring_node_id);
            candidate.focus_path.reserve(candidate.focus_chain.size());
            for (std::size_t i = 0; i < candidate.focus_chain.size(); ++i) {
                FocusEntry entry;
                entry.path = candidate.focus_chain[i];
                entry.focusable = (i == 0);
                candidate.focus_path.push_back(std::move(entry));
            }
        }

        candidate.position.scene_x = request.x;
        candidate.position.scene_y = request.y;
        if (drawable_index < bucket->bounds_boxes.size()
            && (drawable_index >= bucket->bounds_box_valid.size()
                || bucket->bounds_box_valid[drawable_index] != 0)) {
            auto const& box = bucket->bounds_boxes[drawable_index];
            candidate.position.local_x = request.x - box.min[0];
            candidate.position.local_y = request.y - box.min[1];
            candidate.position.has_local = true;
        }

        if (enqueue_render && !render_enqueued) {
            auto status = enqueue_auto_render_event(space,
                                                    *auto_render_target,
                                                    "hit-test",
                                                    0);
            if (!status) {
                return std::unexpected(status.error());
            }
            render_enqueued = true;
        }

        result.hits.push_back(std::move(candidate));
        if (result.hits.size() >= max_results) {
            break;
        }
    }

    if (!result.hits.empty()) {
        result.hit = true;
        auto const& primary = result.hits.front();
        result.target = primary.target;
        result.position = primary.position;
        result.focus_chain = primary.focus_chain;
        result.focus_path = primary.focus_path;
    }

    return result;
}

auto MarkDirty(PathSpace& space,
               ScenePath const& scenePath,
               DirtyKind kinds,
               std::chrono::system_clock::time_point timestamp) -> SP::Expected<std::uint64_t> {
    if (kinds == DirtyKind::None) {
        return std::unexpected(make_error("dirty kinds must not be empty", SP::Error::Code::InvalidType));
    }

    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return std::unexpected(status.error());
    }

    auto statePath = dirty_state_path(scenePath);
    auto queuePath = dirty_queue_path(scenePath);

    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }

    DirtyState state{};
    if (existing->has_value()) {
        state = **existing;
    }

    auto seq = g_scene_dirty_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    auto combined_mask = dirty_mask(state.pending) | dirty_mask(kinds);
    state.pending = make_dirty_kind(combined_mask);
    state.sequence = seq;
    state.timestamp_ms = to_epoch_ms(timestamp);

    if (auto status = replace_single<DirtyState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }

    DirtyEvent event{
        .sequence = seq,
        .kinds = kinds,
        .timestamp_ms = state.timestamp_ms,
    };
    auto inserted = space.insert(queuePath, event);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return seq;
}

auto ClearDirty(PathSpace& space,
                ScenePath const& scenePath,
                DirtyKind kinds) -> SP::Expected<void> {
    if (kinds == DirtyKind::None) {
        return {};
    }

    if (auto status = EnsureAuthoringRoot(space, scenePath); !status) {
        return std::unexpected(status.error());
    }

    auto statePath = dirty_state_path(scenePath);
    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return {};
    }

    auto state = **existing;
    auto current_mask = dirty_mask(state.pending);
    auto cleared_mask = current_mask & ~dirty_mask(kinds);
    if (cleared_mask == current_mask) {
        return {};
    }

    state.pending = make_dirty_kind(cleared_mask);
    state.timestamp_ms = to_epoch_ms(std::chrono::system_clock::now());

    if (auto status = replace_single<DirtyState>(space, statePath, state); !status) {
        return std::unexpected(status.error());
    }
    return {};
}

auto ReadDirtyState(PathSpace const& space,
                    ScenePath const& scenePath) -> SP::Expected<DirtyState> {
    auto statePath = dirty_state_path(scenePath);
    auto existing = read_optional<DirtyState>(space, statePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!existing->has_value()) {
        return DirtyState{};
    }
    return **existing;
}

auto TakeDirtyEvent(PathSpace& space,
                    ScenePath const& scenePath,
                    std::chrono::milliseconds timeout) -> SP::Expected<DirtyEvent> {
    auto queuePath = dirty_queue_path(scenePath);
    auto event = space.take<DirtyEvent>(queuePath, SP::Out{} & SP::Block{timeout});
    if (!event) {
        return std::unexpected(event.error());
    }
    return *event;
}

} // namespace SP::UI::Builders::Scene
