#pragma once

#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/DetailShared.hpp>
#include <pathspace/ui/PathRenderer2D.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>
#include "DrawableUtils.hpp"

#include "core/Out.hpp"
#include "core/PathSpaceContext.hpp"
#include "path/UnvalidatedPath.hpp"
#include "task/IFutureAny.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI::Runtime {
namespace Detail {

inline constexpr std::string_view kScenesSegment = "/scenes/";
inline constexpr std::string_view kRenderersSegment = "/renderers/";
inline constexpr std::string_view kSurfacesSegment = "/surfaces/";
inline constexpr std::string_view kWindowsSegment = "/windows/";
inline constexpr std::string_view kWidgetAuthoringMarker = "/authoring/";
inline std::atomic<std::uint64_t> g_auto_render_sequence{0};
inline std::atomic<std::uint64_t>& g_scene_dirty_sequence =
    SP::UI::DetailShared::scene_dirty_sequence();
inline std::atomic<std::uint64_t>& g_widget_op_sequence =
    SP::UI::DetailShared::widget_op_sequence();

struct SceneRevisionRecord {
    uint64_t    revision = 0;
    int64_t     published_at_ms = 0;
    std::string author;
};

inline auto make_error(std::string message,
                SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

namespace SceneData = SP::UI::Scene;

template <typename T>
inline auto replace_single(PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void>;

inline auto combine_relative(AppRootPathView root, std::string spec) -> SP::Expected<ConcretePath>;

inline auto make_scene_meta(ScenePath const& scenePath, std::string const& leaf) -> std::string;

template <typename T>
inline auto read_optional(PathSpace const& space, std::string const& path) -> SP::Expected<std::optional<T>>;

inline auto surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceSoftware>> cache;
    return cache;
}

inline auto surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

#if PATHSPACE_UI_METAL
inline auto metal_surfaces_cache() -> std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>>& {
    static std::unordered_map<std::string, std::unique_ptr<PathSurfaceMetal>> cache;
    return cache;
}

inline auto metal_surfaces_cache_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}
#endif

struct SurfaceCacheWatchEntry {
    std::string                               target_key;
    std::string                               watch_path;
    std::weak_ptr<SP::PathSpaceContext>       context;
    PathSpace*                                space = nullptr;
    std::thread                               worker;
    std::atomic<bool>                         stop{false};
    std::atomic<bool>                         finished{false};
};

inline auto surface_cache_watch_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

inline auto surface_cache_watch_entries()
    -> std::unordered_map<std::string, std::unique_ptr<SurfaceCacheWatchEntry>>& {
    static std::unordered_map<std::string, std::unique_ptr<SurfaceCacheWatchEntry>> entries;
    return entries;
}

inline auto collect_finished_surface_cache_watches_locked()
    -> std::vector<std::unique_ptr<SurfaceCacheWatchEntry>> {
    std::vector<std::unique_ptr<SurfaceCacheWatchEntry>> finished;
    auto&                                               entries = surface_cache_watch_entries();
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->second && it->second->finished.load(std::memory_order_acquire)) {
            finished.push_back(std::move(it->second));
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    return finished;
}

inline void stop_and_join_surface_cache_watch(std::unique_ptr<SurfaceCacheWatchEntry> entry) {
    if (!entry) {
        return;
    }
    entry->stop.store(true, std::memory_order_release);
    if (auto ctx = entry->context.lock()) {
        ctx->notify(entry->watch_path);
    }
    if (entry->worker.joinable()) {
        entry->worker.join();
    }
}

inline void shutdown_surface_cache_watches();

inline void register_surface_cache_watch_shutdown_hook() {
    static std::once_flag once;
    std::call_once(once, []() {
        std::atexit([]() {
            shutdown_surface_cache_watches();
        });
    });
}

inline void prune_surface_cache_watches() {
    std::vector<std::unique_ptr<SurfaceCacheWatchEntry>> finished;
    {
        std::lock_guard<std::mutex> guard(surface_cache_watch_mutex());
        finished = collect_finished_surface_cache_watches_locked();
    }
    for (auto& entry : finished) {
        stop_and_join_surface_cache_watch(std::move(entry));
    }
}

inline void shutdown_surface_cache_watches() {
    std::vector<std::unique_ptr<SurfaceCacheWatchEntry>> pending;
    {
        std::lock_guard<std::mutex> guard(surface_cache_watch_mutex());
        auto&                       entries = surface_cache_watch_entries();
        for (auto& [_, entry] : entries) {
            pending.push_back(std::move(entry));
        }
        entries.clear();
    }
    for (auto& entry : pending) {
        stop_and_join_surface_cache_watch(std::move(entry));
    }
}

inline void evict_surface_cache_entry(std::string const& key) {
    {
        std::lock_guard<std::mutex> lock(surfaces_cache_mutex());
        surfaces_cache().erase(key);
    }
#if PATHSPACE_UI_METAL
    {
        std::lock_guard<std::mutex> lock(metal_surfaces_cache_mutex());
        metal_surfaces_cache().erase(key);
    }
#endif
}

inline auto surface_cache_watch_marker_missing(PathSpace& space,
                                                std::string const& watch_path) -> bool {
    auto marker = read_optional<bool>(space, watch_path);
    if (!marker) {
        return false;
    }
    return !marker->has_value();
}

inline void run_surface_cache_watch(SurfaceCacheWatchEntry* entry) {
    auto const& watch_path = entry->watch_path;
    while (!entry->stop.load(std::memory_order_acquire)) {
        auto ctx = entry->context.lock();
        if (!ctx || ctx->isShuttingDown()) {
            break;
        }

        if (surface_cache_watch_marker_missing(*entry->space, watch_path)) {
            evict_surface_cache_entry(entry->target_key);
            break;
        }

        auto guard    = ctx->wait(watch_path);
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
        guard.wait_until(deadline, [&]() {
            return entry->stop.load(std::memory_order_acquire)
                   || ctx->isShuttingDown()
                   || surface_cache_watch_marker_missing(*entry->space, watch_path);
        });
    }
    entry->finished.store(true, std::memory_order_release);
}

inline void activate_surface_cache_watch(PathSpace& space,
                                  std::string const& target_key) {
    auto context = space.sharedContext();
    if (!context) {
        return;
    }

    register_surface_cache_watch_shutdown_hook();

    std::vector<std::unique_ptr<SurfaceCacheWatchEntry>> finished;
    std::unique_ptr<SurfaceCacheWatchEntry>               replaced;

    {
        std::lock_guard<std::mutex> guard(surface_cache_watch_mutex());
        auto&                       entries = surface_cache_watch_entries();
        finished = collect_finished_surface_cache_watches_locked();
        auto it = entries.find(target_key);
        if (it != entries.end()) {
            if (!it->second->finished.load(std::memory_order_acquire)) {
                return;
            }
            replaced = std::move(it->second);
            entries.erase(it);
        }

        auto entry = std::make_unique<SurfaceCacheWatchEntry>();
        entry->target_key = target_key;
        entry->watch_path = target_key + std::string{"/diagnostics/cacheWatch"};
        entry->context    = context;
        entry->space      = &space;
        entry->worker     = std::thread(run_surface_cache_watch, entry.get());
        entries.emplace(target_key, std::move(entry));
    }

    for (auto& entry : finished) {
        stop_and_join_surface_cache_watch(std::move(entry));
    }
    if (replaced) {
        stop_and_join_surface_cache_watch(std::move(replaced));
    }
}

inline auto before_present_hook_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

inline auto before_present_hook_storage()
    -> Window::TestHooks::BeforePresentHook& {
    static Window::TestHooks::BeforePresentHook hook;
    return hook;
}

inline void invoke_before_present_hook(PathSurfaceSoftware& surface,
                                PathWindowView::PresentPolicy& policy,
                                std::vector<std::size_t>& dirty_tiles) {
    Window::TestHooks::BeforePresentHook hook_copy;
    {
        std::lock_guard<std::mutex> lock{Detail::before_present_hook_mutex()};
        hook_copy = before_present_hook_storage();
    }
    if (hook_copy) {
        hook_copy(surface, policy, dirty_tiles);
    }
}

inline auto acquire_surface_unlocked(std::string const& key,
                              Runtime::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
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

inline auto acquire_surface(std::string const& key,
                     Runtime::SurfaceDesc const& desc) -> PathSurfaceSoftware& {
    auto& mutex = surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_surface_unlocked(key, desc);
}

#if PATHSPACE_UI_METAL
inline auto acquire_metal_surface_unlocked(std::string const& key,
                                    Runtime::SurfaceDesc const& desc) -> PathSurfaceMetal& {
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

inline auto acquire_metal_surface(std::string const& key,
                           Runtime::SurfaceDesc const& desc) -> PathSurfaceMetal& {
    auto& mutex = metal_surfaces_cache_mutex();
    std::lock_guard<std::mutex> lock{mutex};
    return acquire_metal_surface_unlocked(key, desc);
}
#endif

inline auto enqueue_auto_render_event(PathSpace& space,
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

inline auto maybe_schedule_auto_render_impl(PathSpace& space,
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

inline auto dirty_state_path(ScenePath const& scenePath) -> std::string {
    return std::string(scenePath.getPath()) + "/diagnostics/dirty/state";
}

inline auto dirty_queue_path(ScenePath const& scenePath) -> std::string {
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
inline auto read_optional(PathSpace const& space,
                   std::string const& path) -> SP::Expected<std::optional<T>>;

inline auto present_mode_to_string(PathWindowView::PresentMode mode) -> std::string {
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

inline auto parse_present_mode(std::string_view text) -> SP::Expected<PathWindowView::PresentMode> {
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

inline auto read_present_policy(PathSpace const& space,
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

inline auto prepare_surface_render_context(PathSpace& space,
                                    SurfacePath const& surfacePath,
                                    std::optional<RenderSettings> const& settingsOverride)
    -> SP::Expected<SurfaceRenderContext>;

inline auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind>;
inline auto read_renderer_kind(PathSpace& space,
                        std::string const& path) -> SP::Expected<RendererKind>;
inline auto renderer_kind_to_string(RendererKind kind) -> std::string;

inline auto ensure_non_empty(std::string_view value,
                      std::string_view what) -> SP::Expected<void> {
    if (value.empty()) {
        return std::unexpected(make_error(std::string(what) + " must not be empty",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

inline auto ensure_identifier(std::string_view value,
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
inline auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
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
inline auto replace_single(PathSpace& space,
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

inline auto ensure_surface_cache_watch(PathSpace& space,
                                std::string const& target_key) -> SP::Expected<void> {
    if (auto* disable = std::getenv("PATHSPACE_DISABLE_SURFACE_CACHE_WATCH")) {
        if (*disable != '\0' && std::strcmp(disable, "0") != 0) {
            return {};
        }
    }
    prune_surface_cache_watches();
    auto watch_path = target_key + std::string{"/diagnostics/cacheWatch"};
    auto marker     = read_optional<bool>(space, watch_path);
    if (!marker) {
        return std::unexpected(marker.error());
    }
    if (!marker->has_value()) {
        if (auto status = replace_single<bool>(space, watch_path, true); !status) {
            return status;
        }
    }
    activate_surface_cache_watch(space, target_key);
    return {};
}

template <typename T>
inline auto read_value(PathSpace const& space,
                std::string const& path,
                SP::Out const& out = {}) -> SP::Expected<T> {
    auto const& base = static_cast<PathSpaceBase const&>(space);
    return base.template read<T, std::string>(path, out);
}

template <typename T>
inline auto read_optional(PathSpace const& space,
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
inline auto combine_relative(AppRootPathView root,
                       std::string relative) -> SP::Expected<ConcretePath> {
    return SP::App::resolve_app_relative(root, std::move(relative));
}

inline auto relative_to_root(AppRootPathView root,
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

inline auto derive_app_root_for(ConcretePathView absolute) -> SP::Expected<AppRootPath> {
    return SP::App::derive_app_root(absolute);
}

inline auto derive_window_root_for(std::string_view absolute) -> SP::Expected<WindowPath> {
    auto path = std::string{absolute};
    auto windows_pos = path.find("/windows/");
    if (windows_pos == std::string::npos) {
        return std::unexpected(
            make_error("path '" + path + "' missing '/windows/<id>' segment",
                       SP::Error::Code::InvalidPath));
    }
    windows_pos += std::strlen("/windows/");
    auto next_slash = path.find('/', windows_pos);
    if (next_slash == std::string::npos) {
        return WindowPath{path};
    }
    return WindowPath{std::string(path.substr(0, next_slash))};
}

inline auto window_component_for(std::string_view absolute) -> SP::Expected<std::string> {
    auto path = std::string{absolute};
    auto windows_pos = path.find("/windows/");
    if (windows_pos == std::string::npos) {
        return std::unexpected(
            make_error("path '" + path + "' missing '/windows/<id>' segment",
                       SP::Error::Code::InvalidPath));
    }
    windows_pos += std::strlen("/windows/");
    auto next_slash = path.find('/', windows_pos);
    if (next_slash == std::string::npos) {
        next_slash = path.size();
    }
    return std::string(path.substr(windows_pos, next_slash - windows_pos));
}

inline auto ensure_contains_segment(ConcretePathView path,
                             std::string_view segment) -> SP::Expected<void> {
    if (path.getPath().find(segment) == std::string::npos) {
        return std::unexpected(make_error("path '" + std::string(path.getPath()) + "' missing segment '"
                                          + std::string(segment) + "'",
                                          SP::Error::Code::InvalidPath));
    }
    return {};
}

inline auto same_app(ConcretePathView lhs,
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

inline auto prepare_surface_render_context(PathSpace& space,
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

inline auto render_into_target(PathSpace& space,
                        SurfaceRenderContext const& context,
                        PathSurfaceSoftware& software_surface
#if PATHSPACE_UI_METAL
                        ,
                        PathSurfaceMetal* metal_surface
#endif
                        ) -> SP::Expected<PathRenderer2D::RenderStats> {
#if PATHSPACE_UI_METAL
    if (context.renderer_kind == RendererKind::Metal2D) {
        if (metal_surface == nullptr) {
            return std::unexpected(make_error("metal renderer requested without metal surface cache",
                                              SP::Error::Code::InvalidType));
        }
    } else if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#else
    if (context.renderer_kind != RendererKind::Software2D) {
        return std::unexpected(make_error("Unsupported renderer kind for render target",
                                          SP::Error::Code::InvalidType));
    }
#endif

    PathRenderer2D renderer{space};
    PathRenderer2D::RenderParams params{
        .target_path = SP::ConcretePathStringView{context.target_path.getPath()},
        .settings = context.settings,
        .surface = software_surface,
        .backend_kind = context.renderer_kind,
#if PATHSPACE_UI_METAL
        .metal_surface = metal_surface,
#endif
    };
    return renderer.render(params);
}

inline auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

inline auto to_epoch_ns(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
}

inline auto from_epoch_ms(int64_t ms) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

inline auto to_record(SceneRevisionDesc const& desc) -> SceneRevisionRecord {
    SceneRevisionRecord record{};
    record.revision = desc.revision;
    record.published_at_ms = to_epoch_ms(desc.published_at);
    record.author = desc.author;
    return record;
}

inline auto from_record(SceneRevisionRecord const& record) -> SceneRevisionDesc {
    SceneRevisionDesc desc{};
    desc.revision = record.revision;
    desc.published_at = from_epoch_ms(record.published_at_ms);
    desc.author = record.author;
    return desc;
}

inline auto format_revision(uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

inline auto is_safe_asset_path(std::string_view path) -> bool {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return false;
    }
    if (path.find("..") != std::string_view::npos) {
        return false;
    }
    return true;
}

inline auto guess_mime_type(std::string_view logical_path) -> std::string {
    auto dot = logical_path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= logical_path.size()) {
        return "application/octet-stream";
    }
    std::string ext;
    ext.reserve(logical_path.size() - (dot + 1));
    for (std::size_t i = dot + 1; i < logical_path.size(); ++i) {
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(logical_path[i]))));
    }

    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "webp") return "image/webp";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "avif") return "image/avif";
    if (ext == "bmp")  return "image/bmp";

    if (ext == "woff2") return "font/woff2";
    if (ext == "woff")  return "font/woff";
    if (ext == "ttf")   return "font/ttf";
    if (ext == "otf")   return "font/otf";

    if (ext == "css")   return "text/css";
    if (ext == "js" || ext == "mjs") return "text/javascript";
    if (ext == "json")  return "application/json";

    return "application/octet-stream";
}

inline auto hydrate_html_assets(PathSpace& space,
                         std::string const& revisionBase,
                         std::vector<Html::Asset>& assets) -> SP::Expected<void> {
    for (auto& asset : assets) {
        bool const needs_lookup = asset.bytes.empty()
                                  || asset.mime_type == Html::kImageAssetReferenceMime
                                  || asset.mime_type == Html::kFontAssetReferenceMime;
        if (!needs_lookup) {
            continue;
        }

        if (!is_safe_asset_path(asset.logical_path)) {
            return std::unexpected(make_error("html asset logical path unsafe: " + asset.logical_path,
                                              SP::Error::Code::InvalidPath));
        }

        std::string full_path = revisionBase;
        if (asset.logical_path.rfind("assets/", 0) == 0) {
            full_path.append("/").append(asset.logical_path);
        } else {
            full_path.append("/assets/").append(asset.logical_path);
        }

        auto bytes = space.read<std::vector<std::uint8_t>>(full_path);
        if (!bytes) {
            auto const error = bytes.error();
            std::string message = "read html asset '" + asset.logical_path + "'";
            if (error.message) {
                message.append(": ").append(*error.message);
            }
            return std::unexpected(make_error(std::move(message), error.code));
        }

        asset.bytes = std::move(*bytes);
        if (asset.mime_type == Html::kImageAssetReferenceMime
            || asset.mime_type == Html::kFontAssetReferenceMime
            || asset.mime_type.empty()) {
            asset.mime_type = guess_mime_type(asset.logical_path);
        }
    }
    return {};
}

inline auto make_revision_base(ScenePath const& scenePath,
                        std::string const& revisionStr) -> std::string {
    return std::string(scenePath.getPath()) + "/builds/" + revisionStr;
}

inline auto make_scene_meta(ScenePath const& scenePath,
                     std::string const& leaf) -> std::string {
    return std::string(scenePath.getPath()) + "/meta/" + leaf;
}

inline auto bytes_from_span(std::span<std::byte const> bytes) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

inline auto resolve_renderer_spec(AppRootPathView appRoot,
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

inline auto leaf_component(ConcretePathView path) -> SP::Expected<std::string> {
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

inline auto read_relative_string(PathSpace const& space,
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

inline auto store_desc(PathSpace& space,
                std::string const& path,
                SurfaceDesc const& desc) -> SP::Expected<void> {
    return replace_single<SurfaceDesc>(space, path, desc);
}

inline auto store_renderer_kind(PathSpace& space,
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

inline auto parse_renderer_kind(std::string_view text) -> std::optional<RendererKind> {
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

inline auto read_renderer_kind(PathSpace& space,
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

inline auto renderer_kind_to_string(RendererKind kind) -> std::string {
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

inline auto ensure_within_root(AppRootPathView root,
                        ConcretePathView path) -> SP::Expected<void> {
    auto status = SP::App::ensure_within_app(root, path);
    if (!status) {
        return std::unexpected(status.error());
    }
    return {};
}

} // namespace Detail

} // namespace SP::UI::Runtime
