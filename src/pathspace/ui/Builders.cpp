#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include "DrawableUtils.hpp"

#include "core/Out.hpp"
#include "path/UnvalidatedPath.hpp"
#include "task/IFutureAny.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Builders {

namespace {

constexpr std::string_view kScenesSegment = "/scenes/";
constexpr std::string_view kRenderersSegment = "/renderers/";
constexpr std::string_view kSurfacesSegment = "/surfaces/";
constexpr std::string_view kWindowsSegment = "/windows/";

std::atomic<std::uint64_t> g_auto_render_sequence{0};

struct SceneRevisionRecord {
    uint64_t    revision = 0;
    int64_t     published_at_ms = 0;
    std::string author;
};

auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

auto enqueue_auto_render_event(PathSpace& space,
                               std::string const& targetPath,
                               std::string_view reason,
                               std::uint64_t frame_index) -> SP::Expected<void> {
    auto queuePath = targetPath + "/events/renderRequested/queue";
    AutoRenderRequestEvent event{
        .sequence = g_auto_render_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        .reason = std::string(reason),
        .frame_index = frame_index,
    };
    auto inserted = space.insert(queuePath, event);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

struct SurfaceRenderContext {
    SP::ConcretePathString target_path;
    SurfaceDesc            target_desc;
    RenderSettings         settings;
};

template <typename T>
auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>>;

auto present_mode_to_string(PathWindowView::PresentMode mode) -> std::string {
    switch (mode) {
    case PathWindowView::PresentMode::AlwaysFresh:
        return "AlwaysFresh";
    case PathWindowView::PresentMode::PreferLatestCompleteWithBudget:
        return "PreferLatestCompleteWithBudget";
    case PathWindowView::PresentMode::AlwaysLatestComplete:
        return "AlwaysLatestComplete";
    }
    return "Unknown";
}

auto parse_present_mode(std::string_view text) -> SP::Expected<PathWindowView::PresentMode> {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized.empty()) {
        return std::unexpected(make_error("present policy string must not be empty",
                                          SP::Error::Code::InvalidType));
    }
    if (normalized == "alwaysfresh") {
        return PathWindowView::PresentMode::AlwaysFresh;
    }
    if (normalized == "preferlatestcompletewithbudget"
        || normalized == "preferlatestcomplete") {
        return PathWindowView::PresentMode::PreferLatestCompleteWithBudget;
    }
    if (normalized == "alwayslatestcomplete") {
        return PathWindowView::PresentMode::AlwaysLatestComplete;
    }
    return std::unexpected(make_error("unknown present policy '" + std::string(text) + "'",
                                      SP::Error::Code::InvalidType));
}

auto read_present_policy(PathSpace const& space,
                         std::string const& viewBase) -> SP::Expected<PathWindowView::PresentPolicy> {
    PathWindowView::PresentPolicy policy{};
    auto policyPath = viewBase + "/present/policy";
    auto policyValue = read_optional<std::string>(space, policyPath);
    if (!policyValue) {
        return std::unexpected(policyValue.error());
    }
    if (policyValue->has_value()) {
        auto mode = parse_present_mode(**policyValue);
        if (!mode) {
            return std::unexpected(mode.error());
        }
        policy.mode = *mode;
    }

    auto read_double = [&](std::string const& path) -> SP::Expected<std::optional<double>> {
        return read_optional<double>(space, path);
    };
    auto read_uint64 = [&](std::string const& path) -> SP::Expected<std::optional<std::uint64_t>> {
        return read_optional<std::uint64_t>(space, path);
    };
    auto read_bool = [&](std::string const& path) -> SP::Expected<std::optional<bool>> {
        return read_optional<bool>(space, path);
    };

    auto paramsBase = viewBase + "/present/params";
    if (auto value = read_double(paramsBase + "/staleness_budget_ms"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.staleness_budget_ms_value = **value;
        auto duration = std::chrono::duration<double, std::milli>(**value);
        policy.staleness_budget = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    } else {
        policy.staleness_budget_ms_value = static_cast<double>(policy.staleness_budget.count());
    }

    if (auto value = read_double(paramsBase + "/frame_timeout_ms"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.frame_timeout_ms_value = **value;
        auto duration = std::chrono::duration<double, std::milli>(**value);
        policy.frame_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    } else {
        policy.frame_timeout_ms_value = static_cast<double>(policy.frame_timeout.count());
    }

    if (auto value = read_uint64(paramsBase + "/max_age_frames"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.max_age_frames = static_cast<std::uint32_t>(**value);
    }

    if (auto value = read_bool(paramsBase + "/vsync_align"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.vsync_align = **value;
    }

    if (auto value = read_bool(paramsBase + "/auto_render_on_present"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.auto_render_on_present = **value;
    }

    return policy;
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext>;

auto render_into_surface(PathSpace& space,
                         SP::ConcretePathStringView targetPath,
                         RenderSettings const& settings,
                         PathSurfaceSoftware& surface) -> SP::Expected<PathRenderer2D::RenderStats>;

auto ensure_non_empty(std::string_view value,
                      std::string_view what) -> SP::Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(std::string(what) + " must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto ensure_identifier(std::string_view value,
                       std::string_view what) -> SP::Expected<void> {
    if (auto status = ensure_non_empty(value, what); !status) {
        return status;
    }
    if (value == "." || value == "..") {
        return std::unexpected(make_error(std::string(what) + " must not be '.' or '..'",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    if (value.find('/') != std::string_view::npos) {
        return std::unexpected(make_error(std::string(what) + " must not contain '/' characters",
                                          SP::Error::Code::InvalidPathSubcomponent));
    }
    return {};
}

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single(PathSpace& space,
                   std::string const& path,
                   T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

template <typename T>
auto read_value(PathSpace const& space,
                std::string const& path,
                SP::Out const& out = {}) -> SP::Expected<T> {
    auto const& base = static_cast<PathSpaceBase const&>(space);
    return base.template read<T, std::string>(path, out);
}

template <typename T>
auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>> {
    auto value = read_value<T>(space, path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}
auto combine_relative(AppRootPathView root,
                       std::string relative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, std::move(relative));
}

auto relative_to_root(AppRootPathView root,
                      ConcretePathView absolute) -> SP::Expected<std::string> {
    auto ensured = SP::App::ensure_within_app(root, absolute);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    auto const& rootStr = root.getPath();
    auto const& absStr = absolute.getPath();
    if (absStr.size() == rootStr.size()) {
        return std::string{};
    }
    if (absStr.size() <= rootStr.size() + 1) {
        return std::string{};
    }
    return std::string(absStr.substr(rootStr.size() + 1));
}

auto derive_app_root_for(ConcretePathView absolute) -> SP::Expected<AppRootPath> {
    return SP::App::derive_app_root(absolute);
}

auto ensure_contains_segment(ConcretePathView path,
                             std::string_view segment) -> SP::Expected<void> {
    if (path.getPath().find(segment) == std::string::npos) {
        return std::unexpected(make_error("path '" + std::string(path.getPath()) + "' missing segment '"
                                          + std::string(segment) + "'",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto same_app(ConcretePathView lhs,
             ConcretePathView rhs) -> SP::Expected<void> {
    auto lhsRoot = derive_app_root_for(lhs);
    if (!lhsRoot) {
        return std::unexpected(lhsRoot.error());
    }
    auto rhsRoot = derive_app_root_for(rhs);
    if (!rhsRoot) {
        return std::unexpected(rhsRoot.error());
    }
    if (lhsRoot->getPath() != rhsRoot->getPath()) {
        return std::unexpected(make_error("paths belong to different application roots",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext> {
    auto surfaceRoot = derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = read_value<std::string>(space, targetField);
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                                        *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto descPath = targetAbsolute->getPath() + "/desc";
    auto targetDesc = read_value<SurfaceDesc>(space, descPath);
    if (!targetDesc) {
        return std::unexpected(targetDesc.error());
    }

    RenderSettings effective{};
    if (settingsOverride) {
        effective = *settingsOverride;
    } else {
        auto stored = Renderer::ReadSettings(space, ConcretePathView{targetAbsolute->getPath()});
        if (stored) {
            effective = *stored;
        } else {
            auto const& error = stored.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
            effective.surface.size_px.width = targetDesc->size_px.width;
            effective.surface.size_px.height = targetDesc->size_px.height;
            effective.surface.dpi_scale = 1.0f;
            effective.surface.visibility = true;
            effective.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
            effective.time.time_ms = 0.0;
            effective.time.delta_ms = 16.0;
            effective.time.frame_index = 0;
        }
    }

    effective.surface.size_px.width = targetDesc->size_px.width;
    effective.surface.size_px.height = targetDesc->size_px.height;
    if (effective.surface.dpi_scale == 0.0f) {
        effective.surface.dpi_scale = 1.0f;
    }

    if (!settingsOverride) {
        if (effective.time.delta_ms == 0.0) {
            effective.time.delta_ms = 16.0;
        }
        effective.time.time_ms += effective.time.delta_ms;
        effective.time.frame_index += 1;
    }

    if (auto status = Renderer::UpdateSettings(space,
                                               ConcretePathView{targetAbsolute->getPath()},
                                               effective); !status) {
        return std::unexpected(status.error());
    }

    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{targetAbsolute->getPath()},
        .target_desc = *targetDesc,
        .settings = effective,
    };
    return context;
}

auto render_into_surface(PathSpace& space,
                         SP::ConcretePathStringView targetPath,
                         RenderSettings const& settings,
                         PathSurfaceSoftware& surface) -> SP::Expected<PathRenderer2D::RenderStats> {
    PathRenderer2D renderer{space};
    return renderer.render({
        .target_path = targetPath,
        .settings = settings,
        .surface = surface,
    });
}

auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

auto from_epoch_ms(int64_t ms) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

auto to_record(SceneRevisionDesc const& desc) -> SceneRevisionRecord {
    SceneRevisionRecord record{};
    record.revision = desc.revision;
    record.published_at_ms = to_epoch_ms(desc.published_at);
    record.author = desc.author;
    return record;
}

auto from_record(SceneRevisionRecord const& record) -> SceneRevisionDesc {
    SceneRevisionDesc desc{};
    desc.revision = record.revision;
    desc.published_at = from_epoch_ms(record.published_at_ms);
    desc.author = record.author;
    return desc;
}

auto format_revision(uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto make_revision_base(ScenePath const& scenePath,
                        std::string const& revisionStr) -> std::string {
    return std::string(scenePath.getPath()) + "/builds/" + revisionStr;
}

auto make_scene_meta(ScenePath const& scenePath,
                     std::string const& leaf) -> std::string {
    return std::string(scenePath.getPath()) + "/meta/" + leaf;
}

auto bytes_from_span(std::span<std::byte const> bytes) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

auto resolve_renderer_spec(AppRootPathView appRoot,
                           std::string const& spec) -> SP::Expected<ConcretePath> {
    if (spec.empty()) {
        return std::unexpected(make_error("renderer spec must not be empty",
                                          SP::Error::Code::InvalidPath));
    }

    if (spec.front() == '/') {
        return SP::App::resolve_app_relative(appRoot, spec);
    }

    std::string candidate = spec;
    if (spec.find('/') == std::string::npos) {
        candidate = "renderers/" + spec;
    }
    return SP::App::resolve_app_relative(appRoot, candidate);
}

auto leaf_component(ConcretePathView path) -> SP::Expected<std::string> {
    SP::UnvalidatedPathView raw{path.getPath()};
    auto components = raw.split_absolute_components();
    if (!components) {
        return std::unexpected(components.error());
    }
    if (components->empty()) {
        return std::unexpected(make_error("path has no components",
                                          SP::Error::Code::InvalidPath));
    }
    return std::string(components->back());
}

auto read_relative_string(PathSpace const& space,
                          std::string const& path) -> SP::Expected<std::string> {
    auto value = read_value<std::string>(space, path);
    if (value) {
        return *value;
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound) {
        return std::string{};
    }
    return std::unexpected(error);
}

auto store_desc(PathSpace& space,
                std::string const& path,
                SurfaceDesc const& desc) -> SP::Expected<void> {
    return replace_single<SurfaceDesc>(space, path, desc);
}

auto ensure_within_root(AppRootPathView root,
                        ConcretePathView path) -> SP::Expected<void> {
    auto status = SP::App::ensure_within_app(root, path);
    if (!status) {
        return std::unexpected(status.error());
    }
    return {};
}

} // namespace

auto resolve_app_relative(AppRootPathView root,
                          UnvalidatedPathView maybeRelative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, maybeRelative);
}

auto derive_target_base(AppRootPathView root,
                        ConcretePathView rendererPath,
                        ConcretePathView targetPath) -> SP::Expected<ConcretePath> {
    return SP::App::derive_target_base(root, rendererPath, targetPath);
}

namespace Scene {

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
    std::optional<std::size_t> hit_index;

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
        hit_index = drawable_index;
        break;
    }

    if (hit_index) {
        auto idx = *hit_index;
        result.hit = true;
        result.target.drawable_id = bucket->drawable_ids[idx];
        if (idx < bucket->authoring_map.size()) {
            auto const& author = bucket->authoring_map[idx];
            result.target.authoring_node_id = author.authoring_node_id;
            result.target.drawable_index_within_node = author.drawable_index_within_node;
            result.target.generation = author.generation;
            result.focus_chain = detail::build_focus_chain(author.authoring_node_id);
        }
        if (request.schedule_render && auto_render_target) {
            auto status = enqueue_auto_render_event(space,
                                                    *auto_render_target,
                                                    "hit-test",
                                                    0);
            if (!status) {
                return std::unexpected(status.error());
            }
        }
    }

    return result;
}

} // namespace Scene

namespace Renderer {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             RendererParams const& params,
             RendererKind kind) -> SP::Expected<RendererPath> {
    if (auto status = ensure_identifier(params.name, "renderer name"); !status) {
        return std::unexpected(status.error());
    }

    auto resolved = combine_relative(appRoot, std::string("renderers/") + params.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto metaBase = std::string(resolved->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return RendererPath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/description", params.description); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<RendererKind>(space, metaBase + "/kind", kind); !status) {
        return std::unexpected(status.error());
    }

    return RendererPath{resolved->getPath()};
}

auto ResolveTargetBase(PathSpace const& /*space*/,
                        AppRootPathView appRoot,
                        RendererPath const& rendererPath,
                        std::string_view targetSpec) -> SP::Expected<ConcretePath> {
    if (auto status = ensure_non_empty(targetSpec, "target spec"); !status) {
        return std::unexpected(status.error());
    }

    if (auto status = SP::App::ensure_within_app(appRoot, ConcretePathView{rendererPath.getPath()}); !status) {
        return std::unexpected(status.error());
    }

    std::string spec{targetSpec};
    if (!spec.empty() && spec.front() == '/') {
        auto resolved = combine_relative(appRoot, std::move(spec));
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        return *resolved;
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath.getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    std::string combined = *rendererRelative;
    if (!combined.empty()) {
        combined.push_back('/');
    }
    combined.append(spec);

    auto resolved = combine_relative(appRoot, std::move(combined));
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return *resolved;
}

auto UpdateSettings(PathSpace& space,
                     ConcretePathView targetPath,
                     RenderSettings const& settings) -> SP::Expected<void> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return replace_single<RenderSettings>(space, settingsPath, settings);
}

auto ReadSettings(PathSpace const& space,
                   ConcretePathView targetPath) -> SP::Expected<RenderSettings> {
    auto settingsPath = std::string(targetPath.getPath()) + "/settings";
    return read_value<RenderSettings>(space, settingsPath);
}

auto TriggerRender(PathSpace& space,
                   ConcretePathView targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto surfaceDesc = read_value<SurfaceDesc>(space, descPath);
    if (!surfaceDesc) {
        return std::unexpected(surfaceDesc.error());
    }

    PathSurfaceSoftware surface{*surfaceDesc};
    auto stats = render_into_surface(space,
                                     SP::ConcretePathStringView{targetPath.getPath()},
                                     settings,
                                     surface);
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

} // namespace Renderer

namespace Surface {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             SurfaceParams const& params) -> SP::Expected<SurfacePath> {
    if (auto status = ensure_identifier(params.name, "surface name"); !status) {
        return std::unexpected(status.error());
    }

    auto surfacePath = combine_relative(appRoot, std::string("surfaces/") + params.name);
    if (!surfacePath) {
        return std::unexpected(surfacePath.error());
    }

    auto rendererPath = resolve_renderer_spec(appRoot, params.renderer);
    if (!rendererPath) {
        return std::unexpected(rendererPath.error());
    }

    if (auto status = ensure_contains_segment(ConcretePathView{surfacePath->getPath()}, kSurfacesSegment); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = ensure_contains_segment(ConcretePathView{rendererPath->getPath()}, kRenderersSegment); !status) {
        return std::unexpected(status.error());
    }

    auto metaBase = std::string(surfacePath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return SurfacePath{surfacePath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }

    auto descPath = std::string(surfacePath->getPath()) + "/desc";
    if (auto status = store_desc(space, descPath, params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto rendererRelative = relative_to_root(appRoot, ConcretePathView{rendererPath->getPath()});
    if (!rendererRelative) {
        return std::unexpected(rendererRelative.error());
    }

    auto rendererField = std::string(surfacePath->getPath()) + "/renderer";
    if (auto status = replace_single<std::string>(space, rendererField, *rendererRelative); !status) {
        return std::unexpected(status.error());
    }

    auto targetSpec = std::string("targets/surfaces/") + params.name;
    auto targetBase = Renderer::ResolveTargetBase(space, appRoot, *rendererPath, targetSpec);
    if (!targetBase) {
        return std::unexpected(targetBase.error());
    }

    auto targetRelative = relative_to_root(appRoot, ConcretePathView{targetBase->getPath()});
    if (!targetRelative) {
        return std::unexpected(targetRelative.error());
    }

    if (auto status = store_desc(space, std::string(targetBase->getPath()) + "/desc", params.desc); !status) {
        return std::unexpected(status.error());
    }

    auto targetField = std::string(surfacePath->getPath()) + "/target";
    if (auto status = replace_single<std::string>(space, targetField, *targetRelative); !status) {
        return std::unexpected(status.error());
    }

    return SurfacePath{surfacePath->getPath()};
}

auto SetScene(PathSpace& space,
               SurfacePath const& surfacePath,
               ScenePath const& scenePath) -> SP::Expected<void> {
    auto surfaceRoot = derive_app_root_for(ConcretePathView{surfacePath.getPath()});
    if (!surfaceRoot) {
        return std::unexpected(surfaceRoot.error());
    }
    auto sceneRoot = derive_app_root_for(ConcretePathView{scenePath.getPath()});
    if (!sceneRoot) {
        return std::unexpected(sceneRoot.error());
    }
    if (surfaceRoot->getPath() != sceneRoot->getPath()) {
        return std::unexpected(make_error("surface and scene belong to different applications",
                                          SP::Error::Code::InvalidPath));
    }

    auto sceneRelative = relative_to_root(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                          ConcretePathView{scenePath.getPath()});
    if (!sceneRelative) {
        return std::unexpected(sceneRelative.error());
    }

    auto sceneField = std::string(surfacePath.getPath()) + "/scene";
    if (auto status = replace_single<std::string>(space, sceneField, *sceneRelative); !status) {
        return status;
    }

    auto targetField = std::string(surfacePath.getPath()) + "/target";
    auto targetRelative = read_value<std::string>(space, targetField);
    if (!targetRelative) {
        if (targetRelative.error().code == SP::Error::Code::NoObjectFound) {
            return std::unexpected(make_error("surface missing target binding",
                                              SP::Error::Code::InvalidPath));
        }
        return std::unexpected(targetRelative.error());
    }

    auto targetAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{surfaceRoot->getPath()},
                                                        *targetRelative);
    if (!targetAbsolute) {
        return std::unexpected(targetAbsolute.error());
    }

    auto targetScenePath = targetAbsolute->getPath() + "/scene";
    return replace_single<std::string>(space, targetScenePath, *sceneRelative);
}

auto RenderOnce(PathSpace& space,
                 SurfacePath const& surfacePath,
                 std::optional<RenderSettings> settingsOverride) -> SP::Expected<SP::FutureAny> {
    auto context = prepare_surface_render_context(space, surfacePath, settingsOverride);
    if (!context) {
        return std::unexpected(context.error());
    }

    PathSurfaceSoftware surface{context->target_desc};
    auto stats = render_into_surface(space,
                                     SP::ConcretePathStringView{context->target_path.getPath()},
                                     context->settings,
                                     surface);
    if (!stats) {
        return std::unexpected(stats.error());
    }

    auto state = std::make_shared<SP::SharedState<bool>>();
    state->set_value(true);
    return SP::FutureT<bool>{state}.to_any();
}

} // namespace Surface

namespace Window {

auto Create(PathSpace& space,
             AppRootPathView appRoot,
             WindowParams const& params) -> SP::Expected<WindowPath> {
    if (auto status = ensure_identifier(params.name, "window name"); !status) {
        return std::unexpected(status.error());
    }

    auto windowPath = combine_relative(appRoot, std::string("windows/") + params.name);
    if (!windowPath) {
        return std::unexpected(windowPath.error());
    }

    auto metaBase = std::string(windowPath->getPath()) + "/meta";
    auto namePath = metaBase + "/name";
    auto existing = read_optional<std::string>(space, namePath);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return WindowPath{windowPath->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/title", params.title); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/width", params.width); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<int>(space, metaBase + "/height", params.height); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<float>(space, metaBase + "/scale", params.scale); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/background", params.background); !status) {
        return std::unexpected(status.error());
    }

    return WindowPath{windowPath->getPath()};
}

auto AttachSurface(PathSpace& space,
                    WindowPath const& windowPath,
                    std::string_view viewName,
                    SurfacePath const& surfacePath) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    if (auto status = same_app(ConcretePathView{windowPath.getPath()},
                               ConcretePathView{surfacePath.getPath()}); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    auto surfaceRelative = relative_to_root(SP::App::AppRootPathView{windowRoot->getPath()},
                                            ConcretePathView{surfacePath.getPath()});
    if (!surfaceRelative) {
        return std::unexpected(surfaceRelative.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    if (auto status = replace_single<std::string>(space, viewBase + "/surface", *surfaceRelative); !status) {
        return status;
    }
    (void)drain_queue<std::string>(space, viewBase + "/windowTarget");
    return {};
}

auto Present(PathSpace& space,
              WindowPath const& windowPath,
              std::string_view viewName) -> SP::Expected<void> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return status;
    }

    auto windowRoot = derive_app_root_for(ConcretePathView{windowPath.getPath()});
    if (!windowRoot) {
        return std::unexpected(windowRoot.error());
    }

    std::string viewBase = std::string(windowPath.getPath()) + "/views/" + std::string(viewName);
    auto surfaceRel = read_value<std::string>(space, viewBase + "/surface");
    if (!surfaceRel) {
        return std::unexpected(surfaceRel.error());
    }
    if (surfaceRel->empty()) {
        return std::unexpected(make_error("view is not bound to a surface",
                                          SP::Error::Code::InvalidPath));
    }

    auto surfacePath = SP::App::resolve_app_relative(SP::App::AppRootPathView{windowRoot->getPath()},
                                                     *surfaceRel);
   if (!surfacePath) {
       return std::unexpected(surfacePath.error());
   }

    auto context = prepare_surface_render_context(space,
                                                  SurfacePath{surfacePath->getPath()},
                                                  std::nullopt);
    if (!context) {
        return std::unexpected(context.error());
    }

    auto policy = read_present_policy(space, viewBase);
    if (!policy) {
        return std::unexpected(policy.error());
    }

    PathSurfaceSoftware surface{context->target_desc};
    auto renderStats = render_into_surface(space,
                                           SP::ConcretePathStringView{context->target_path.getPath()},
                                           context->settings,
                                           surface);
    if (!renderStats) {
        return std::unexpected(renderStats.error());
    }

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();

    PathWindowView presenter;
    std::vector<std::uint8_t> framebuffer(surface.frame_bytes(), 0);
    auto now = std::chrono::steady_clock::now();
    auto vsync_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(policy->frame_timeout);
    if (vsync_budget < std::chrono::steady_clock::duration::zero()) {
        vsync_budget = std::chrono::steady_clock::duration::zero();
    }
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .framebuffer = framebuffer,
        .dirty_tiles = dirty_tiles,
    };
    auto presentStats = presenter.present(surface, *policy, request);
    if (renderStats) {
        presentStats.frame.frame_index = renderStats->frame_index;
        presentStats.frame.revision = renderStats->revision;
        presentStats.frame.render_ms = renderStats->render_ms;
    }

    auto metricsBase = std::string(context->target_path.getPath()) + "/output/v1/common";
    std::uint64_t previous_frame_index = 0;
    if (auto previous = read_optional<uint64_t>(space, metricsBase + "/frameIndex"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_frame_index = **previous;
    }

    if (presentStats.frame.frame_index >= previous_frame_index) {
        presentStats.frame_age_frames = presentStats.frame.frame_index - previous_frame_index;
    } else {
        presentStats.frame_age_frames = 0;
    }
    presentStats.frame_age_ms = static_cast<double>(presentStats.frame_age_frames)
                                * static_cast<double>(policy->frame_timeout.count());
    presentStats.stale = presentStats.frame_age_frames > policy->max_age_frames;

    if (auto status = Diagnostics::WritePresentMetrics(space,
                                                       SP::ConcretePathStringView{context->target_path.getPath()},
                                                       presentStats,
                                                       *policy); !status) {
        return std::unexpected(status.error());
    }

    return {};
}

} // namespace Window

namespace Diagnostics {

auto ReadTargetMetrics(PathSpace const& space,
                        ConcretePathView targetPath) -> SP::Expected<TargetMetrics> {
    TargetMetrics metrics{};

    auto base = std::string(targetPath.getPath()) + "/output/v1/common";

    if (auto value = read_value<uint64_t>(space, base + "/frameIndex"); value) {
        metrics.frame_index = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, base + "/revision"); value) {
        metrics.revision = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/renderMs"); value) {
        metrics.render_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/presentMs"); value) {
        metrics.present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/lastPresentSkipped"); value) {
        metrics.last_present_skipped = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, base + "/lastError"); value) {
        metrics.last_error = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    return metrics;
}

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void> {
    auto path = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, path, std::string{});
}

auto WritePresentMetrics(PathSpace& space,
                          ConcretePathView targetPath,
                          PathWindowPresentStats const& stats,
                          PathWindowPresentPolicy const& policy) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/output/v1/common";

    if (auto status = replace_single<uint64_t>(space, base + "/frameIndex", stats.frame.frame_index); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/revision", stats.frame.revision); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/renderMs", stats.frame.render_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentMs", stats.present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/lastPresentSkipped", stats.skipped); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/presented", stats.presented); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/bufferedFrameConsumed", stats.buffered_frame_consumed); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedProgressive", stats.used_progressive); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/presentedAgeMs", stats.frame_age_ms); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/presentedAgeFrames",
                                               stats.frame_age_frames); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/stale", stats.stale); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space,
                                                  base + "/presentMode",
                                                  present_mode_to_string(stats.mode)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/progressiveTilesCopied",
                                               static_cast<uint64_t>(stats.progressive_tiles_copied)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/progressiveRectsCoalesced",
                                               static_cast<uint64_t>(stats.progressive_rects_coalesced)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/progressiveSkipOddSeq",
                                               static_cast<uint64_t>(stats.progressive_skip_seq_odd)); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space, base + "/progressiveRecopyAfterSeqChange",
                                               static_cast<uint64_t>(stats.progressive_recopy_after_seq_change)); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/waitBudgetMs", stats.wait_budget_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/stalenessBudgetMs",
                                             policy.staleness_budget_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space,
                                             base + "/frameTimeoutMs",
                                             policy.frame_timeout_ms_value); !status) {
        return status;
    }
    if (auto status = replace_single<uint64_t>(space,
                                               base + "/maxAgeFrames",
                                               static_cast<uint64_t>(policy.max_age_frames)); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/autoRenderOnPresent",
                                           policy.auto_render_on_present); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space,
                                           base + "/vsyncAlign",
                                           policy.vsync_align); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/lastError", stats.error); !status) {
        return status;
    }
    return {};
}

} // namespace Diagnostics

} // namespace SP::UI::Builders
