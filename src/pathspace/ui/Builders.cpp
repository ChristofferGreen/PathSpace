#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include "DrawableUtils.hpp"

#include "core/Out.hpp"
#include "path/UnvalidatedPath.hpp"
#include "task/IFutureAny.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstdlib>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI::Builders {

namespace {

constexpr std::string_view kScenesSegment = "/scenes/";
constexpr std::string_view kRenderersSegment = "/renderers/";
constexpr std::string_view kSurfacesSegment = "/surfaces/";
constexpr std::string_view kWindowsSegment = "/windows/";

std::atomic<std::uint64_t> g_auto_render_sequence{0};
std::atomic<std::uint64_t> g_scene_dirty_sequence{0};

struct SceneRevisionRecord {
    uint64_t    revision = 0;
    int64_t     published_at_ms = 0;
    std::string author;
};

auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

auto surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>> cache;
    return cache;
}

auto surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

#if PATHSPACE_UI_METAL
auto metal_surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>> cache;
    return cache;
}

auto metal_surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}
#endif

auto before_present_hook_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

auto before_present_hook_storage()
    -> Window::TestHooks::BeforePresentHook& {
    static Window::TestHooks::BeforePresentHook hook;
    return hook;
}

void invoke_before_present_hook(PathSurfaceSoftware& surface,
                                PathWindowView::PresentPolicy& policy,
                                std::vector<std::size_t>& dirty_tiles) {
    Window::TestHooks::BeforePresentHook hook_copy;
    {
        std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
        hook_copy = before_present_hook_storage();
    }
    if (hook_copy) {
        hook_copy(surface, policy, dirty_tiles);
    }
}

auto acquire_surface_unlocked(std::string const& key,
                              Builders::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
    auto& cache = surfaces_cache();
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto surface = std::make_unique<PathSurfaceSoftware>(desc);
        auto* raw = surface.get();
        cache.emplace(key, std::move(surface));
        return *raw;
    }

    auto& surface = *it->second;
    auto const& current = surface.desc();
    if (current.size_px.width != desc.size_px.width
        || current.size_px.height != desc.size_px.height
        || current.pixel_format != desc.pixel_format
        || current.color_space != desc.color_space
        || current.premultiplied_alpha != desc.premultiplied_alpha) {
        surface.resize(desc);
    }
    return surface;
}

auto acquire_surface(std::string const& key,
                     Builders::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
    auto& mutex = surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_surface_unlocked(key, desc);
}

#if PATHSPACE_UI_METAL
auto acquire_metal_surface_unlocked(std::string const& key,
                                    Builders::SurfaceDesc const& desc) -> PathSurfaceMetal& {
    auto& cache = metal_surfaces_cache();
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto surface = std::make_unique<PathSurfaceMetal>(desc);
        auto* raw = surface.get();
        cache.emplace(key, std::move(surface));
        return *raw;
    }

    auto& surface = *it->second;
    auto const& current = surface.desc();
    if (current.size_px.width != desc.size_px.width
        || current.size_px.height != desc.size_px.height
        || current.pixel_format != desc.pixel_format
        || current.color_space != desc.color_space
        || current.premultiplied_alpha != desc.premultiplied_alpha) {
        surface.resize(desc);
    }
    return surface;
}

auto acquire_metal_surface(std::string const& key,
                           Builders::SurfaceDesc const& desc) -> PathSurfaceMetal& {
    auto& mutex = metal_surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_metal_surface_unlocked(key, desc);
}
#endif

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

auto maybe_schedule_auto_render_impl(PathSpace& space,
                                     std::string const& targetPath,
                                     PathWindowView::PresentStats const& stats,
                                     PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool> {
    if (!policy.auto_render_on_present) {
        return false;
    }

    std::vector<std::string> reasons;
    if (stats.skipped) {
        reasons.emplace_back("present-skipped");
    }

    if (!stats.buffered_frame_consumed) {
        if (stats.frame_age_frames > policy.max_age_frames) {
            reasons.emplace_back("age-frames");
        }
        if (stats.frame_age_ms > policy.staleness_budget_ms_value) {
            reasons.emplace_back("age-ms");
        }
    } else {
        if (stats.frame_age_frames > policy.max_age_frames) {
            reasons.emplace_back("age-frames");
        }
        if (stats.frame_age_ms > policy.staleness_budget_ms_value) {
            reasons.emplace_back("age-ms");
        }
    }

    if (reasons.empty()) {
        return false;
    }

    std::string reason = reasons.front();
    for (std::size_t i = 1; i < reasons.size(); ++i) {
        reason.append(",");
        reason.append(reasons[i]);
    }

    auto status = enqueue_auto_render_event(space, targetPath, reason, stats.frame.frame_index);
    if (!status) {
        return std::unexpected(status.error());
    }
    return true;
}

auto dirty_state_path(ScenePath const& scenePath) -> std::string {
    return std::string(scenePath.getPath()) + "/diagnostics/dirty/state";
}

auto dirty_queue_path(ScenePath const& scenePath) -> std::string {
    return std::string(scenePath.getPath()) + "/diagnostics/dirty/queue";
}

constexpr auto dirty_mask(Scene::DirtyKind kind) -> std::uint32_t {
    return static_cast<std::uint32_t>(kind);
}

constexpr auto make_dirty_kind(std::uint32_t mask) -> Scene::DirtyKind {
    return static_cast<Scene::DirtyKind>(mask & static_cast<std::uint32_t>(Scene::DirtyKind::All));
}

struct SurfaceRenderContext {
    SP::ConcretePathString target_path;
    SP::ConcretePathString renderer_path;
    SurfaceDesc            target_desc;
    RenderSettings         settings;
    RendererKind           renderer_kind = RendererKind::Software2D;
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

    if (auto value = read_bool(paramsBase + "/capture_framebuffer"); !value) {
        return std::unexpected(value.error());
    } else if (value->has_value()) {
        policy.capture_framebuffer = **value;
    }

    return policy;
}

auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext>;

auto render_into_software_surface(PathSpace& space,
                                  SP::ConcretePathStringView targetPath,
                                  RenderSettings const& settings,
                                  PathSurfaceSoftware& surface) -> SP::Expected<PathRenderer2D::RenderStats>;
#if PATHSPACE_UI_METAL
auto upload_to_metal_surface(PathSurfaceSoftware& software,
                             PathSurfaceMetal& metal,
                             std::vector<std::uint8_t>& scratch) -> SP::Expected<void>;
#endif

auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind>;
auto read_renderer_kind(PathSpace& space,
                        std::string const& path) -> SP::Expected<RendererKind>;
auto renderer_kind_to_string(RendererKind kind) -> std::string;

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

    auto const& targetStr = targetAbsolute->getPath();
    auto targetsPos = targetStr.find("/targets/");
    if (targetsPos == std::string::npos) {
        return std::unexpected(make_error("target path '" + targetStr + "' missing /targets/ segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto rendererPathStr = targetStr.substr(0, targetsPos);
    if (rendererPathStr.empty()) {
        return std::unexpected(make_error("renderer path derived from target is empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererKind = read_renderer_kind(space, rendererPathStr + "/meta/kind");
    if (!rendererKind) {
        return std::unexpected(rendererKind.error());
    }

    auto effectiveKind = *rendererKind;
#if !PATHSPACE_UI_METAL
    if (effectiveKind == RendererKind::Metal2D) {
        effectiveKind = RendererKind::Software2D;
    }
#else
    if (effectiveKind == RendererKind::Metal2D
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        effectiveKind = RendererKind::Software2D;
    }
#endif

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
            effective.surface.metal = targetDesc->metal;
            effective.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
            effective.time.time_ms = 0.0;
            effective.time.delta_ms = 16.0;
            effective.time.frame_index = 0;
        }
    }

    effective.surface.size_px.width = targetDesc->size_px.width;
    effective.surface.size_px.height = targetDesc->size_px.height;
    effective.surface.metal = targetDesc->metal;
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

    effective.renderer.backend_kind = effectiveKind;
#if PATHSPACE_UI_METAL
    effective.renderer.metal_uploads_enabled = (effectiveKind == RendererKind::Metal2D)
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") != nullptr;
#else
    effective.renderer.metal_uploads_enabled = false;
#endif

    if (auto status = Renderer::UpdateSettings(space,
                                               ConcretePathView{targetAbsolute->getPath()},
                                               effective); !status) {
        return std::unexpected(status.error());
    }

    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{targetAbsolute->getPath()},
        .renderer_path = SP::ConcretePathString{rendererPathStr},
        .target_desc = *targetDesc,
        .settings = effective,
        .renderer_kind = effectiveKind,
    };
    return context;
}

auto render_into_software_surface(PathSpace& space,
                                  SP::ConcretePathStringView targetPath,
                                  RenderSettings const& settings,
                                  PathSurfaceSoftware& surface) -> SP::Expected<PathRenderer2D::RenderStats> {
    PathRenderer2D renderer{space};
    auto stats = renderer.render({
        .target_path = targetPath,
        .settings = settings,
        .surface = surface,
        .backend_kind = RendererKind::Software2D,
    });
    if (!stats) {
        return stats;
    }
    stats->resource_gpu_bytes = 0;
    return stats;
}

#if PATHSPACE_UI_METAL
auto render_into_metal_surface(PathSpace& space,
                               SurfaceRenderContext const& context,
                               PathSurfaceSoftware& software_surface,
                               PathSurfaceMetal& metal_surface,
                               std::vector<std::uint8_t>& scratch) -> SP::Expected<PathRenderer2D::RenderStats> {
    auto stats = render_into_software_surface(space,
                                              SP::ConcretePathStringView{context.target_path.getPath()},
                                              context.settings,
                                              software_surface);
    if (!stats) {
        return stats;
    }

    if (auto upload = upload_to_metal_surface(software_surface, metal_surface, scratch); !upload) {
        return std::unexpected(upload.error());
    }
    metal_surface.update_material_descriptors(stats->materials);
    metal_surface.present_completed(stats->frame_index, stats->revision);
    stats->backend_kind = RendererKind::Metal2D;
    stats->resource_gpu_bytes = metal_surface.resident_gpu_bytes();
    return stats;
}
#endif

auto render_into_target(PathSpace& space,
                        SurfaceRenderContext const& context,
                        PathSurfaceSoftware& software_surface
#if PATHSPACE_UI_METAL
                        ,
                        PathSurfaceMetal* metal_surface,
                        std::vector<std::uint8_t>& metal_scratch
#endif
                        ) -> SP::Expected<PathRenderer2D::RenderStats> {
#if PATHSPACE_UI_METAL
    if (context.renderer_kind == RendererKind::Metal2D) {
        if (metal_surface == nullptr) {
            return std::unexpected(make_error("metal renderer requested without metal surface cache",
                                              SP::Error::Code::InvalidType));
        }
        return render_into_metal_surface(space, context, software_surface, *metal_surface, metal_scratch);
    }
    if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#else
    if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#endif

    return render_into_software_surface(space,
                                        SP::ConcretePathStringView{context.target_path.getPath()},
                                        context.settings,
                                        software_surface);
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

auto store_renderer_kind(PathSpace& space,
                         std::string const& path,
                         RendererKind kind) -> SP::Expected<void> {
    auto status = replace_single<RendererKind>(space, path, kind);
    if (status) {
        return status;
    }

    auto const& error = status.error();
    auto is_type_mismatch = (error.code == SP::Error::Code::TypeMismatch
                             || error.code == SP::Error::Code::InvalidType);
    if (!is_type_mismatch) {
        std::cerr << "store_renderer_kind: replace_single failed code="
                  << static_cast<int>(error.code)
                  << " message=" << error.message.value_or("<none>") << std::endl;
        return status;
    }

    if (auto cleared = drain_queue<std::string>(space, path); !cleared) {
        std::cerr << "store_renderer_kind: drain_queue<string> failed code="
                  << static_cast<int>(cleared.error().code)
                  << " message=" << cleared.error().message.value_or("<none>")
                  << std::endl;
        return cleared;
    }

    return replace_single<RendererKind>(space, path, kind);
}

auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind> {
    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (normalized == "software" || normalized == "software2d") {
        return RendererKind::Software2D;
    }
    if (normalized == "metal" || normalized == "metal2d") {
        return RendererKind::Metal2D;
    }
    if (normalized == "vulkan" || normalized == "vulkan2d") {
        return RendererKind::Vulkan2D;
    }
    return std::nullopt;
}

auto read_renderer_kind(PathSpace& space,
                        std::string const& path) -> SP::Expected<RendererKind> {
    auto stored = read_value<RendererKind>(space, path);
    if (stored) {
        return *stored;
    }

    auto const& error = stored.error();
    if (error.code == SP::Error::Code::TypeMismatch) {
        auto legacy = read_value<std::string>(space, path);
        if (!legacy) {
            return std::unexpected(legacy.error());
        }
        auto parsed = parse_renderer_kind(*legacy);
        if (!parsed) {
            return std::unexpected(make_error("unable to parse renderer kind '" + *legacy + "'",
                                              SP::Error::Code::InvalidType));
        }
        if (auto status = store_renderer_kind(space, path, *parsed); !status) {
            return std::unexpected(status.error());
        }
        return *parsed;
    }

    if (error.code == SP::Error::Code::NoObjectFound
        || error.code == SP::Error::Code::NoSuchPath) {
        auto fallback = RendererKind::Software2D;
        if (auto status = store_renderer_kind(space, path, fallback); !status) {
            return std::unexpected(status.error());
        }
        return fallback;
    }

    return std::unexpected(error);
}

auto renderer_kind_to_string(RendererKind kind) -> std::string {
    switch (kind) {
    case RendererKind::Software2D:
        return "Software2D";
    case RendererKind::Metal2D:
        return "Metal2D";
    case RendererKind::Vulkan2D:
        return "Vulkan2D";
    }
    return "Unknown";
}

#if PATHSPACE_UI_METAL
auto upload_to_metal_surface(PathSurfaceSoftware& software,
                             PathSurfaceMetal& metal,
                             std::vector<std::uint8_t>& scratch) -> SP::Expected<void> {
    auto const frame_bytes = software.frame_bytes();
    if (frame_bytes == 0) {
        return {};
    }

    scratch.resize(frame_bytes);
    auto copy = software.copy_buffered_frame(std::span<std::uint8_t>{scratch.data(), scratch.size()});
    if (!copy) {
        return {};
    }

    if (std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        return {};
    }

    metal.update_from_rgba8(std::span<std::uint8_t const>{scratch.data(), scratch.size()},
                            software.row_stride_bytes(),
                            copy->info.frame_index,
                            copy->info.revision);
    return {};
}
#endif

auto ensure_within_root(AppRootPathView root,
                        ConcretePathView path) -> SP::Expected<void> {
    auto status = SP::App::ensure_within_app(root, path);
    if (!status) {
        return std::unexpected(status.error());
    }
    return {};
}

} // namespace

void Window::TestHooks::SetBeforePresentHook(BeforePresentHook hook) {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = std::move(hook);
}

void Window::TestHooks::ResetBeforePresentHook() {
    std::lock_guard<std::mutex> lock{before_present_hook_mutex()};
    before_present_hook_storage() = nullptr;
}

auto maybe_schedule_auto_render(PathSpace& space,
                                std::string const& targetPath,
                                PathWindowView::PresentStats const& stats,
                                PathWindowView::PresentPolicy const& policy) -> SP::Expected<bool> {
    return maybe_schedule_auto_render_impl(space, targetPath, stats, policy);
}

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
            result.focus_path.reserve(result.focus_chain.size());
            for (std::size_t i = 0; i < result.focus_chain.size(); ++i) {
                FocusEntry entry;
                entry.path = result.focus_chain[i];
                entry.focusable = (i == 0);
                result.focus_path.push_back(std::move(entry));
            }
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
        result.position.scene_x = request.x;
        result.position.scene_y = request.y;
        if (idx < bucket->bounds_boxes.size()
            && (idx >= bucket->bounds_box_valid.size()
                || bucket->bounds_box_valid[idx] != 0)) {
            auto const& box = bucket->bounds_boxes[idx];
            result.position.local_x = request.x - box.min[0];
            result.position.local_y = request.y - box.min[1];
            result.position.has_local = true;
        }
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
        auto ensureDescription = read_optional<std::string>(space, metaBase + "/description");
        if (!ensureDescription) {
            return std::unexpected(ensureDescription.error());
        }
        if (!ensureDescription->has_value()) {
            auto descStatus = replace_single<std::string>(space, metaBase + "/description", params.description);
            if (!descStatus) {
                return std::unexpected(descStatus.error());
            }
        }

        auto kindStatus = store_renderer_kind(space, metaBase + "/kind", kind);
        if (!kindStatus) {
            return std::unexpected(kindStatus.error());
        }

        return RendererPath{resolved->getPath()};
    }

    if (auto status = replace_single<std::string>(space, namePath, params.name); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space, metaBase + "/description", params.description); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = store_renderer_kind(space, metaBase + "/kind", kind); !status) {
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

auto rectangles_touch_or_overlap(DirtyRectHint const& a, DirtyRectHint const& b) -> bool {
    auto overlaps_axis = [](float min_a, float max_a, float min_b, float max_b) {
        return !(max_a < min_b || min_a > max_b);
    };
    return overlaps_axis(a.min_x, a.max_x, b.min_x, b.max_x)
        && overlaps_axis(a.min_y, a.max_y, b.min_y, b.max_y);
}

auto merge_hints(std::vector<DirtyRectHint>& hints,
                 float tile_size,
                 float width,
                 float height) -> void {
    if (hints.empty()) {
        return;
    }
    auto fallback_to_full_surface = [&]() {
        hints.clear();
        if (width <= 0.0f || height <= 0.0f) {
            return;
        }
        hints.push_back(DirtyRectHint{
            .min_x = 0.0f,
            .min_y = 0.0f,
            .max_x = width,
            .max_y = height,
        });
    };
    if (width <= 0.0f || height <= 0.0f) {
        hints.clear();
        return;
    }
    bool merged_any = true;
    while (merged_any) {
        merged_any = false;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            for (std::size_t j = i + 1; j < hints.size(); ++j) {
                if (rectangles_touch_or_overlap(hints[i], hints[j])) {
                    hints[i].min_x = std::min(hints[i].min_x, hints[j].min_x);
                    hints[i].min_y = std::min(hints[i].min_y, hints[j].min_y);
                    hints[i].max_x = std::max(hints[i].max_x, hints[j].max_x);
                    hints[i].max_y = std::max(hints[i].max_y, hints[j].max_y);
                    hints.erase(hints.begin() + static_cast<std::ptrdiff_t>(j));
                    merged_any = true;
                    break;
                }
            }
            if (merged_any) {
                break;
            }
        }
    }
    constexpr std::size_t kMaxStoredHints = 128;
    if (hints.size() > kMaxStoredHints) {
        fallback_to_full_surface();
        return;
    }
    double total_area = 0.0;
    for (auto const& rect : hints) {
        auto const width_px = std::max(0.0f, rect.max_x - rect.min_x);
        auto const height_px = std::max(0.0f, rect.max_y - rect.min_y);
        total_area += static_cast<double>(width_px) * static_cast<double>(height_px);
    }
    auto const surface_area = static_cast<double>(width) * static_cast<double>(height);
    if (surface_area > 0.0 && total_area >= surface_area * 0.9) {
        fallback_to_full_surface();
        return;
    }

    auto approximately = [tile_size](float a, float b) {
        auto const epsilon = std::max(tile_size * 0.001f, 1e-5f);
        return std::fabs(a - b) <= epsilon;
    };

    for (auto& rect : hints) {
        if (approximately(rect.min_x, 0.0f)) {
            rect.min_x = 0.0f;
        }
        if (approximately(rect.min_y, 0.0f)) {
            rect.min_y = 0.0f;
        }
        if (approximately(rect.max_x, width)) {
            rect.max_x = width;
        }
        if (approximately(rect.max_y, height)) {
            rect.max_y = height;
        }
    }
    std::sort(hints.begin(),
              hints.end(),
              [](DirtyRectHint const& lhs, DirtyRectHint const& rhs) {
                  if (lhs.min_y == rhs.min_y) {
                      return lhs.min_x < rhs.min_x;
                  }
                  return lhs.min_y < rhs.min_y;
              });
}

auto snap_hint_to_tiles(DirtyRectHint hint, float tile_size) -> DirtyRectHint {
    if (tile_size <= 1.0f) {
        return hint;
    }
    auto align_down = [tile_size](float value) {
        return std::floor(value / tile_size) * tile_size;
    };
    auto align_up = [tile_size](float value) {
        return std::ceil(value / tile_size) * tile_size;
    };
    DirtyRectHint snapped{};
    snapped.min_x = align_down(hint.min_x);
    snapped.min_y = align_down(hint.min_y);
    snapped.max_x = align_up(hint.max_x);
    snapped.max_y = align_up(hint.max_y);
    if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
        return {};
    }
    return snapped;
}

auto SubmitDirtyRects(PathSpace& space,
                      ConcretePathView targetPath,
                      std::span<DirtyRectHint const> rects) -> SP::Expected<void> {
    if (rects.empty()) {
        return SP::Expected<void>{};
    }
    auto hintsPath = std::string(targetPath.getPath()) + "/hints/dirtyRects";

    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto desc = read_value<SurfaceDesc>(space, descPath);
    if (!desc) {
        return std::unexpected(desc.error());
    }
    auto const tile_size = static_cast<float>(std::max(1, (*desc).progressive_tile_size_px));
    auto const width = static_cast<float>(std::max(0, (*desc).size_px.width));
    auto const height = static_cast<float>(std::max(0, (*desc).size_px.height));

    std::vector<DirtyRectHint> stored;
    stored.reserve(rects.size());
    for (auto const& hint : rects) {
        auto snapped = snap_hint_to_tiles(hint, tile_size);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        snapped.min_x = std::clamp(snapped.min_x, 0.0f, width);
        snapped.min_y = std::clamp(snapped.min_y, 0.0f, height);
        snapped.max_x = std::clamp(snapped.max_x, 0.0f, width);
        snapped.max_y = std::clamp(snapped.max_y, 0.0f, height);
        if (snapped.max_x <= snapped.min_x || snapped.max_y <= snapped.min_y) {
            continue;
        }
        stored.push_back(snapped);
    }
    merge_hints(stored, tile_size, width, height);
    return replace_single<std::vector<DirtyRectHint>>(space, hintsPath, stored);
}

auto TriggerRender(PathSpace& space,
                   ConcretePathView targetPath,
                   RenderSettings const& settings) -> SP::Expected<SP::FutureAny> {
    auto descPath = std::string(targetPath.getPath()) + "/desc";
    auto surfaceDesc = read_value<SurfaceDesc>(space, descPath);
    if (!surfaceDesc) {
        return std::unexpected(surfaceDesc.error());
    }

    auto const targetStr = std::string(targetPath.getPath());
    auto targetsPos = targetStr.find("/targets/");
    if (targetsPos == std::string::npos) {
        return std::unexpected(make_error("target path '" + targetStr + "' missing /targets/ segment",
                                          SP::Error::Code::InvalidPath));
    }
    auto rendererPathStr = targetStr.substr(0, targetsPos);
    if (rendererPathStr.empty()) {
        return std::unexpected(make_error("renderer path derived from target is empty",
                                          SP::Error::Code::InvalidPath));
    }

    auto rendererKind = read_renderer_kind(space, rendererPathStr + "/meta/kind");
    if (!rendererKind) {
        return std::unexpected(rendererKind.error());
    }

    auto effectiveKind = *rendererKind;
#if !PATHSPACE_UI_METAL
    if (effectiveKind == RendererKind::Metal2D) {
        effectiveKind = RendererKind::Software2D;
    }
#else
    if (effectiveKind == RendererKind::Metal2D
        && std::getenv("PATHSPACE_ENABLE_METAL_UPLOADS") == nullptr) {
        effectiveKind = RendererKind::Software2D;
    }
#endif

    PathSurfaceSoftware surface{*surfaceDesc};
    SurfaceRenderContext context{
        .target_path = SP::ConcretePathString{std::string{targetPath.getPath()}},
        .renderer_path = SP::ConcretePathString{rendererPathStr},
        .target_desc = *surfaceDesc,
        .settings = settings,
        .renderer_kind = effectiveKind,
    };

#if PATHSPACE_UI_METAL
    std::vector<std::uint8_t> metal_scratch;
    PathSurfaceMetal* metal_surface = nullptr;
    std::unique_ptr<PathSurfaceMetal> metal_owned;
    if (context.renderer_kind == RendererKind::Metal2D) {
        metal_owned = std::make_unique<PathSurfaceMetal>(*surfaceDesc);
        metal_surface = metal_owned.get();
    }
    auto stats = render_into_target(space, context, surface, metal_surface, metal_scratch);
#else
    auto stats = render_into_target(space, context, surface);
#endif
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

#if PATHSPACE_UI_METAL
    std::vector<std::uint8_t> metal_scratch;
#endif

    auto& surface = acquire_surface(std::string(context->target_path.getPath()), context->target_desc);
#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(std::string(context->target_path.getPath()),
                                               context->target_desc);
    }
    auto stats = render_into_target(space,
                                    *context,
                                    surface,
                                    metal_surface,
                                    metal_scratch);
#else
    auto stats = render_into_target(space, *context, surface);
#endif
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
              std::string_view viewName) -> SP::Expected<WindowPresentResult> {
    if (auto status = ensure_identifier(viewName, "view name"); !status) {
        return std::unexpected(status.error());
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
    auto presentPolicy = *policy;

    auto target_key = std::string(context->target_path.getPath());
    auto& surface = acquire_surface(target_key, context->target_desc);

#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
    std::vector<std::uint8_t> metal_scratch;
    if (context->renderer_kind == RendererKind::Metal2D) {
        metal_surface = &acquire_metal_surface(target_key, context->target_desc);
    }
#endif

#if PATHSPACE_UI_METAL
    auto renderStats = render_into_target(space,
                                          *context,
                                          surface,
                                          metal_surface,
                                          metal_scratch);
#else
    auto renderStats = render_into_target(space, *context, surface);
#endif
    if (!renderStats) {
        return std::unexpected(renderStats.error());
    }
    auto stats_value = *renderStats;

    PathSurfaceMetal::TextureInfo metal_texture{};
    bool has_metal_texture = false;
#if PATHSPACE_UI_METAL
    if (metal_surface) {
        metal_texture = metal_surface->acquire_texture();
        has_metal_texture = (metal_texture.texture != nullptr);
    }
#endif

    auto dirty_tiles = surface.consume_progressive_dirty_tiles();
    invoke_before_present_hook(surface, presentPolicy, dirty_tiles);

    PathWindowView presenter;
    std::vector<std::uint8_t> framebuffer;
    std::span<std::uint8_t> framebuffer_span{};
#if PATHSPACE_UI_METAL
    if (metal_surface) {
        framebuffer.swap(metal_scratch);
        framebuffer.resize(surface.frame_bytes(), 0);
        framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
    }
#endif
#if !defined(__APPLE__)
    if (framebuffer.empty()) {
        framebuffer.resize(surface.frame_bytes(), 0);
    }
    framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
#else
    if (framebuffer_span.empty()
        && (presentPolicy.capture_framebuffer || !surface.has_buffered())) {
        framebuffer.resize(surface.frame_bytes(), 0);
        framebuffer_span = std::span<std::uint8_t>{framebuffer.data(), framebuffer.size()};
    }
#endif
    auto now = std::chrono::steady_clock::now();
    auto vsync_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(presentPolicy.frame_timeout);
    if (vsync_budget < std::chrono::steady_clock::duration::zero()) {
        vsync_budget = std::chrono::steady_clock::duration::zero();
    }
#if defined(__APPLE__)
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_texture = metal_texture,
        .allow_iosurface_sharing = true,
    };
#else
    PathWindowView::PresentRequest request{
        .now = now,
        .vsync_deadline = now + vsync_budget,
        .framebuffer = framebuffer_span,
        .dirty_tiles = dirty_tiles,
        .surface_width_px = context->target_desc.size_px.width,
        .surface_height_px = context->target_desc.size_px.height,
        .has_metal_texture = has_metal_texture,
        .metal_texture = metal_texture,
    };
#endif
    auto presentStats = presenter.present(surface, presentPolicy, request);
    if (renderStats) {
        presentStats.frame.frame_index = renderStats->frame_index;
        presentStats.frame.revision = renderStats->revision;
        presentStats.frame.render_ms = renderStats->render_ms;
        presentStats.backend_kind = renderer_kind_to_string(renderStats->backend_kind);
    }
#if defined(__APPLE__)
    auto copy_iosurface_into = [&](PathSurfaceSoftware::SharedIOSurface const& handle,
                                   std::vector<std::uint8_t>& out) {
        auto retained = handle.retain_for_external_use();
        if (!retained) {
            return;
        }
        bool locked = IOSurfaceLock(retained, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess;
        auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(retained));
        auto const row_bytes = IOSurfaceGetBytesPerRow(retained);
        auto const height = handle.height();
        auto const row_stride = surface.row_stride_bytes();
        auto const copy_bytes = std::min<std::size_t>(row_bytes, row_stride);
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));
        if (locked && base && copy_bytes > 0 && height > 0) {
            out.resize(total_bytes);
            for (int row = 0; row < height; ++row) {
                auto* dst = out.data() + static_cast<std::size_t>(row) * row_stride;
                auto const* src = base + static_cast<std::size_t>(row) * row_bytes;
                std::memcpy(dst, src, copy_bytes);
            }
        }
        if (locked) {
            IOSurfaceUnlock(retained, kIOSurfaceLockAvoidSync, nullptr);
        }
        CFRelease(retained);
    };

    if (presentStats.iosurface && presentStats.iosurface->valid()) {
        auto const row_stride = surface.row_stride_bytes();
        auto const height = presentStats.iosurface->height();
        auto const total_bytes = row_stride * static_cast<std::size_t>(std::max(height, 0));

        if (presentPolicy.capture_framebuffer) {
            copy_iosurface_into(*presentStats.iosurface, framebuffer);
        } else {
            framebuffer.clear();
        }
    }
    if (presentStats.buffered_frame_consumed && framebuffer.empty()) {
        auto required = static_cast<std::size_t>(surface.frame_bytes());
        framebuffer.resize(required);
        auto copy = surface.copy_buffered_frame(framebuffer);
        if (!copy) {
            framebuffer.clear();
        }
    }
#endif

    auto metricsBase = std::string(context->target_path.getPath()) + "/output/v1/common";
    std::uint64_t previous_age_frames = 0;
    if (auto previous = read_optional<uint64_t>(space, metricsBase + "/presentedAgeFrames"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_frames = **previous;
    }

    double previous_age_ms = 0.0;
    if (auto previous = read_optional<double>(space, metricsBase + "/presentedAgeMs"); !previous) {
        return std::unexpected(previous.error());
    } else if (previous->has_value()) {
        previous_age_ms = **previous;
    }

    auto frame_timeout_ms = static_cast<double>(presentPolicy.frame_timeout.count());
    bool reuse_previous_frame = !presentStats.buffered_frame_consumed;
#if defined(__APPLE__)
    if (presentStats.used_iosurface) {
        reuse_previous_frame = false;
    }
#endif
    if (!reuse_previous_frame && presentStats.skipped) {
        reuse_previous_frame = true;
    }

    if (reuse_previous_frame) {
        presentStats.frame_age_frames = previous_age_frames + 1;
        presentStats.frame_age_ms = previous_age_ms + frame_timeout_ms;
    } else {
        presentStats.frame_age_frames = 0;
        presentStats.frame_age_ms = 0.0;
    }
    presentStats.stale = presentStats.frame_age_frames > presentPolicy.max_age_frames;

    if (auto scheduled = maybe_schedule_auto_render(space,
                                                    std::string(context->target_path.getPath()),
                                                    presentStats,
                                                    presentPolicy); !scheduled) {
        return std::unexpected(scheduled.error());
    }

    if (auto status = Diagnostics::WritePresentMetrics(space,
                                                       SP::ConcretePathStringView{context->target_path.getPath()},
                                                       presentStats,
                                                       presentPolicy); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = Diagnostics::WriteResidencyMetrics(space,
                                                         SP::ConcretePathStringView{context->target_path.getPath()},
                                                         stats_value.resource_cpu_bytes,
                                                         stats_value.resource_gpu_bytes,
                                                         context->settings.cache.cpu_soft_bytes,
                                                         context->settings.cache.cpu_hard_bytes,
                                                         context->settings.cache.gpu_soft_bytes,
                                                         context->settings.cache.gpu_hard_bytes); !status) {
        return std::unexpected(status.error());
    }

    SoftwareFramebuffer stored_framebuffer{};
    stored_framebuffer.width = context->target_desc.size_px.width;
    stored_framebuffer.height = context->target_desc.size_px.height;
    stored_framebuffer.row_stride_bytes = static_cast<std::uint32_t>(surface.row_stride_bytes());
    stored_framebuffer.pixel_format = context->target_desc.pixel_format;
    stored_framebuffer.color_space = context->target_desc.color_space;
    stored_framebuffer.premultiplied_alpha = context->target_desc.premultiplied_alpha;
    stored_framebuffer.pixels = std::move(framebuffer);

    auto framebufferPath = std::string(context->target_path.getPath()) + "/output/v1/software/framebuffer";
    if (auto status = replace_single<SoftwareFramebuffer>(space, framebufferPath, stored_framebuffer); !status) {
        return std::unexpected(status.error());
    }

    WindowPresentResult result{};
    result.stats = presentStats;
    result.framebuffer = std::move(stored_framebuffer.pixels);
    return result;
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

    if (auto value = read_value<double>(space, base + "/gpuEncodeMs"); value) {
        metrics.gpu_encode_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<double>(space, base + "/gpuPresentMs"); value) {
        metrics.gpu_present_ms = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<bool>(space, base + "/usedMetalTexture"); value) {
        metrics.used_metal_texture = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<std::string>(space, base + "/backendKind"); value) {
        metrics.backend_kind = *value;
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

    if (auto value = read_value<uint64_t>(space, base + "/materialCount"); value) {
        metrics.material_count = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto descriptors = read_optional<std::vector<MaterialDescriptor>>(space, base + "/materialDescriptors"); !descriptors) {
        return std::unexpected(descriptors.error());
    } else if (descriptors->has_value()) {
        metrics.materials = std::move(**descriptors);
        if (metrics.material_count == 0) {
            metrics.material_count = static_cast<uint64_t>(metrics.materials.size());
        }
    }

    auto residencyBase = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuBytes"); value) {
        metrics.cpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuSoftBytes"); value) {
        metrics.cpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/cpuHardBytes"); value) {
        metrics.cpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuBytes"); value) {
        metrics.gpu_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuSoftBytes"); value) {
        metrics.gpu_soft_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    if (auto value = read_value<uint64_t>(space, residencyBase + "/gpuHardBytes"); value) {
        metrics.gpu_hard_bytes = *value;
    } else if (value.error().code != SP::Error::Code::NoObjectFound
               && value.error().code != SP::Error::Code::NoSuchPath) {
        return std::unexpected(value.error());
    }

    metrics.last_error.clear();
    metrics.last_error_code = 0;
    metrics.last_error_revision = 0;
    metrics.last_error_severity = PathSpaceError::Severity::Info;
    metrics.last_error_timestamp_ns = 0;
    metrics.last_error_detail.clear();

    auto diagPath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto errorValue = read_optional<PathSpaceError>(space, diagPath); !errorValue) {
        return std::unexpected(errorValue.error());
    } else if (errorValue->has_value() && !(*errorValue)->message.empty()) {
        metrics.last_error = (*errorValue)->message;
        metrics.last_error_code = (*errorValue)->code;
        metrics.last_error_revision = (*errorValue)->revision;
        metrics.last_error_severity = (*errorValue)->severity;
        metrics.last_error_timestamp_ns = (*errorValue)->timestamp_ns;
        metrics.last_error_detail = (*errorValue)->detail;
    } else {
        if (auto value = read_value<std::string>(space, base + "/lastError"); value) {
            metrics.last_error = *value;
        } else if (value.error().code != SP::Error::Code::NoObjectFound
                   && value.error().code != SP::Error::Code::NoSuchPath) {
            return std::unexpected(value.error());
        }
    }

    return metrics;
}

auto ClearTargetError(PathSpace& space,
                      ConcretePathView targetPath) -> SP::Expected<void> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    PathSpaceError cleared{};
    if (auto status = replace_single<PathSpaceError>(space, livePath, cleared); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, std::string{});
}

auto WriteTargetError(PathSpace& space,
                      ConcretePathView targetPath,
                      PathSpaceError const& error) -> SP::Expected<void> {
    if (error.message.empty()) {
        return ClearTargetError(space, targetPath);
    }

    PathSpaceError stored = error;
    if (stored.path.empty()) {
        stored.path = std::string(targetPath.getPath());
    }
    if (stored.timestamp_ns == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        stored.timestamp_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    if (auto status = replace_single<PathSpaceError>(space, livePath, stored); !status) {
        return status;
    }
    auto lastErrorPath = std::string(targetPath.getPath()) + "/output/v1/common/lastError";
    return replace_single<std::string>(space, lastErrorPath, stored.message);
}

auto ReadTargetError(PathSpace const& space,
                     ConcretePathView targetPath) -> SP::Expected<std::optional<PathSpaceError>> {
    auto livePath = std::string(targetPath.getPath()) + "/diagnostics/errors/live";
    return read_optional<PathSpaceError>(space, livePath);
}

auto ReadSoftwareFramebuffer(PathSpace const& space,
                              ConcretePathView targetPath) -> SP::Expected<SoftwareFramebuffer> {
    auto framebufferPath = std::string(targetPath.getPath()) + "/output/v1/software/framebuffer";
    return read_value<SoftwareFramebuffer>(space, framebufferPath);
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
    if (auto status = replace_single<double>(space, base + "/gpuEncodeMs", stats.gpu_encode_ms); !status) {
        return status;
    }
    if (auto status = replace_single<double>(space, base + "/gpuPresentMs", stats.gpu_present_ms); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/lastPresentSkipped", stats.skipped); !status) {
        return status;
    }
    if (auto status = replace_single<bool>(space, base + "/usedMetalTexture", stats.used_metal_texture); !status) {
        return status;
    }
    if (auto status = replace_single<std::string>(space, base + "/backendKind",
                                                  stats.backend_kind); !status) {
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
    auto progressive_tiles_copied = static_cast<uint64_t>(stats.progressive_tiles_copied);
    if (progressive_tiles_copied == 0) {
        auto existing_tiles = space.read<uint64_t>(base + "/progressiveTilesCopied");
        if (existing_tiles) {
            progressive_tiles_copied = *existing_tiles;
        }
    }
    if (auto status = replace_single<uint64_t>(space, base + "/progressiveTilesCopied",
                                               progressive_tiles_copied); !status) {
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

    if (!stats.error.empty()) {
        PathSpaceError error{};
        error.code = 3000;
        error.severity = PathSpaceError::Severity::Recoverable;
        error.message = stats.error;
        error.path = std::string(targetPath.getPath());
        error.revision = stats.frame.revision;
        if (auto status = WriteTargetError(space, targetPath, error); !status) {
            return status;
        }
    } else {
        if (auto status = ClearTargetError(space, targetPath); !status) {
           return status;
       }
    }
    return {};
}

auto WriteResidencyMetrics(PathSpace& space,
                           ConcretePathView targetPath,
                           std::uint64_t cpu_bytes,
                           std::uint64_t gpu_bytes,
                           std::uint64_t cpu_soft_bytes,
                           std::uint64_t cpu_hard_bytes,
                           std::uint64_t gpu_soft_bytes,
                           std::uint64_t gpu_hard_bytes) -> SP::Expected<void> {
    auto base = std::string(targetPath.getPath()) + "/diagnostics/metrics/residency";
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuBytes", cpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuSoftBytes", cpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/cpuHardBytes", cpu_hard_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuBytes", gpu_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuSoftBytes", gpu_soft_bytes); !status) {
        return status;
    }
    if (auto status = replace_single<std::uint64_t>(space, base + "/gpuHardBytes", gpu_hard_bytes); !status) {
        return status;
    }
    return {};
}

} // namespace Diagnostics

} // namespace SP::UI::Builders
