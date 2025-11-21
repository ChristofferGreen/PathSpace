#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/log/TaggedLogger.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/SceneUtilities.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/Telemetry.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/ui/Builders.hpp>

#include <pathspace/path/ConcretePath.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SP::UI::Declarative::SceneLifecycle {

namespace {

namespace BuilderDetail = SP::UI::Builders::Detail;
namespace BuilderRenderer = SP::UI::Builders::Renderer;
using DirtyRectHint = SP::UI::Builders::DirtyRectHint;
namespace Telemetry = SP::UI::Declarative::Telemetry;
constexpr std::string_view kPublishAuthor = "declarative-runtime";

auto to_epoch_ms(std::chrono::system_clock::time_point tp) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

auto widget_kind_to_string(SP::UI::Declarative::WidgetKind kind) -> std::string_view {
    using SP::UI::Declarative::WidgetKind;
    switch (kind) {
    case WidgetKind::Button:
        return "button";
    case WidgetKind::Toggle:
        return "toggle";
    case WidgetKind::Slider:
        return "slider";
    case WidgetKind::List:
        return "list";
    case WidgetKind::Tree:
        return "tree";
    case WidgetKind::Stack:
        return "stack";
    case WidgetKind::Label:
        return "label";
    case WidgetKind::InputField:
        return "input_field";
    case WidgetKind::PaintSurface:
        return "paint_surface";
    }
    return "unknown";
}

struct BucketCompareResult {
    bool had_previous = false;
    bool parity_ok = true;
    float diff_percent = 0.0f;
};

struct SceneLifecycleWorker {
    SceneLifecycleWorker(PathSpace& space,
                         std::string app_root,
                         std::string scene_path,
                         std::string window_path,
                         std::string view_name,
                         Options options)
        : space_(space)
        , app_root_path_(std::move(app_root))
        , scene_path_(std::move(scene_path))
        , window_path_(std::move(window_path))
        , view_name_(std::move(view_name))
        , options_(options)
        , app_root_value_(app_root_path_)
        , scene_path_value_(scene_path_)
        , snapshot_builder_(space_, SP::App::AppRootPathView{app_root_value_.getPath()}, scene_path_value_) {
        window_widgets_root_ = window_path_ + std::string{"/views/"} + view_name_ + "/widgets";
        trellis_path_ = scene_path_ + "/runtime/lifecycle/trellis";
        trellis_enable_path_ = trellis_path_ + "/_system/enable";
        trellis_disable_path_ = trellis_path_ + "/_system/disable";
        control_queue_path_ = scene_path_ + "/runtime/lifecycle/control";
        theme_invalidate_command_ = control_queue_path_ + ":invalidate_theme";
        force_publish_command_ = control_queue_path_ + ":force_publish";
        metrics_base_ = scene_path_ + "/runtime/lifecycle/metrics";
        auto renderer_leaf = window_path_ + "/views/" + view_name_ + "/renderer";
        auto renderer_relative = space_.read<std::string, std::string>(renderer_leaf);
        if (renderer_relative) {
            auto resolved = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root_value_.getPath()},
                                                          *renderer_relative);
            if (resolved) {
                renderer_target_path_ = resolved->getPath();
                has_renderer_target_ = true;
            }
        }
    }

    ~SceneLifecycleWorker() {
        stop();
    }

    auto start() -> SP::Expected<void> {
        auto mounted = mount_trellis();
        if (!mounted) {
            return mounted;
        }
        (void)register_source(control_queue_path_);
        worker_ = std::thread([this] { this->run(); });
        return {};
    }

    void stop() {
        bool expected = false;
        if (!stop_flag_.compare_exchange_strong(expected, true)) {
            return;
        }
        (void)space_.insert(control_queue_path_, control_queue_path_);
        if (worker_.joinable()) {
            worker_.join();
        }
        (void)BuilderDetail::replace_single<bool>(space_, scene_path_ + "/runtime/lifecycle/state/running", false);
    }

    void request_theme_invalidation() {
        (void)space_.insert(control_queue_path_, theme_invalidate_command_);
    }

    void request_force_publish() {
        (void)space_.insert(control_queue_path_, force_publish_command_);
    }

    [[nodiscard]] auto matches_app(std::string_view candidate) const -> bool {
        return app_root_path_ == candidate;
    }

    [[nodiscard]] auto owns_space(PathSpace& candidate) const -> bool {
        return &space_ == &candidate;
    }

private:
    auto mount_trellis() -> SP::Expected<void> {
        auto alias = std::shared_ptr<PathSpaceBase>(&space_, [](PathSpaceBase*) {});
        auto trellis = std::make_unique<PathSpaceTrellis>(alias);
        auto inserted = space_.insert(trellis_path_, std::move(trellis));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        write_metric("widgets_registered_total", widgets_registered_);
        write_metric("events_processed_total", events_processed_);
        write_metric("widgets_with_buckets", bucket_cache_size_);
        write_metric("sources_active_total", active_sources_);
        write_metric("last_revision", last_revision_);
        write_metric("pending_publish", false);
        (void)BuilderDetail::replace_single<bool>(space_, scene_path_ + "/runtime/lifecycle/state/running", true);
        return {};
    }

    void run() {
        try {
            while (!stop_flag_.load(std::memory_order_acquire)) {
                register_widget_sources();
                scan_dirty_widgets();
                flush_pending_publish();

                auto result = space_.take<std::string>(trellis_path_,
                                                       SP::Out{} & SP::Block{options_.trellis_wait});
                if (!result) {
                    auto const& error = result.error();
                    if (error.code == Error::Code::NoObjectFound || error.code == Error::Code::Timeout) {
                        continue;
                    }
                    continue;
                }

                auto const& widget_path = *result;
                if (widget_path.rfind(control_queue_path_, 0) == 0) {
                    handle_control_command(widget_path);
                    continue;
                }
                process_event(widget_path);
            }
        } catch (std::exception const& ex) {
            handle_worker_exception(ex.what());
        } catch (...) {
            handle_worker_exception("scene lifecycle worker terminated due to unknown exception");
        }
    }

    void register_widget_sources() {
        auto windows_widgets = space_.listChildren(SP::ConcretePathStringView{window_widgets_root_});
        for (auto const& widget_name : windows_widgets) {
            auto widget_root = window_widgets_root_ + "/" + widget_name;
            register_widget_subtree(widget_root);
        }
    }

    void handle_control_command(std::string const& payload) {
        if (payload == control_queue_path_) {
            return;
        }
        if (payload == theme_invalidate_command_) {
            mark_all_widgets_dirty();
            return;
        }
        if (payload == force_publish_command_) {
            pending_publish_.store(false, std::memory_order_release);
            write_metric("pending_publish", false);
            publish_scene_snapshot(scene_path_);
            return;
        }
    }

    void mark_all_widgets_dirty() {
        auto mark_root = [&](std::string const& root) {
            auto widgets = space_.listChildren(SP::ConcretePathStringView{root});
            for (auto const& widget_name : widgets) {
                enqueue_widget_subtree(root + "/" + widget_name);
            }
        };
        mark_root(window_widgets_root_);
        auto app_widgets_root = app_root_path_ + "/widgets";
        mark_root(app_widgets_root);
    }

    void enqueue_widget_subtree(std::string const& widget_root) {
        if (is_widget_removed(widget_root)) {
            cleanup_widget(widget_root);
            return;
        }
        (void)BuilderDetail::replace_single<bool>(space_, widget_root + "/render/dirty", true);
        auto event_path = widget_root + "/render/events/dirty";
        (void)space_.insert(event_path, widget_root);
        auto children_root = widget_root + "/children";
        auto children = space_.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& child_name : children) {
            enqueue_widget_subtree(children_root + "/" + child_name);
        }
    }

    void scan_dirty_widgets() {
        auto scan_root = [&](std::string const& root) {
            auto widgets = space_.listChildren(SP::ConcretePathStringView{root});
            for (auto const& widget_name : widgets) {
                scan_widget_recursive(root + "/" + widget_name);
            }
        };
        scan_root(window_widgets_root_);
        scan_root(app_root_path_ + "/widgets");
    }

    void scan_widget_recursive(std::string const& widget_root) {
        auto dirty = space_.read<bool, std::string>(widget_root + "/render/dirty");
        if (dirty && *dirty) {
            process_event(widget_root);
        }
        auto children_root = widget_root + "/children";
        auto children = space_.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& child_name : children) {
            scan_widget_recursive(children_root + "/" + child_name);
        }
    }

    void register_widget_subtree(std::string const& widget_root) {
        if (is_widget_removed(widget_root)) {
            cleanup_widget(widget_root);
            return;
        }

        bool newly_registered = register_source(widget_root + "/render/events/dirty");

        auto children_root = widget_root + "/children";
        auto children = space_.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& child_name : children) {
            register_widget_subtree(children_root + "/" + child_name);
        }

        if (newly_registered) {
            enqueue_widget_subtree(widget_root);
        }
    }

    auto register_source(std::string const& queue_path) -> bool {
        std::lock_guard<std::mutex> guard(registration_mutex_);
        auto [_, inserted] = registered_sources_.insert(queue_path);
        if (!inserted) {
            return false;
        }
        auto ret = space_.insert(trellis_enable_path_, queue_path);
        if (!ret.errors.empty()) {
            registered_sources_.erase(queue_path);
            return false;
        }
        ++widgets_registered_;
        ++active_sources_;
        write_metric("widgets_registered_total", widgets_registered_);
        write_metric("sources_active_total", active_sources_);
        return true;
    }

    void deregister_source(std::string const& queue_path) {
        std::lock_guard<std::mutex> guard(registration_mutex_);
        if (!registered_sources_.erase(queue_path)) {
            return;
        }
        auto ret = space_.insert(trellis_disable_path_, queue_path);
        if (!ret.errors.empty()) {
            return;
        }
        if (active_sources_ > 0) {
            --active_sources_;
        }
        write_metric("sources_active_total", active_sources_);
    }

    void process_event(std::string const& widget_path) {
        auto dirty_start = std::chrono::steady_clock::now();
        SP::UI::Builders::WidgetPath widget{widget_path};
        auto dirty_version = space_.read<std::uint64_t, std::string>(widget_path + "/render/dirty_version");
        std::uint64_t observed_version = dirty_version.value_or(0);
        bool cleared = false;
        auto clear_dirty = [&]() {
            if (cleared) {
                return;
            }
            auto current_version = space_.read<std::uint64_t, std::string>(widget_path + "/render/dirty_version");
            if (current_version && *current_version != observed_version) {
                return;
            }
            (void)BuilderDetail::replace_single<bool>(space_, widget_path + "/render/dirty", false);
            cleared = true;
        };
        auto schema_start = std::chrono::steady_clock::now();
        auto descriptor = LoadWidgetDescriptor(space_, widget);
        auto schema_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
                                                                              - schema_start)
                             .count();
        Telemetry::SchemaSample schema_sample{
            .widget_path = widget_path,
            .widget_kind = descriptor ? std::string(widget_kind_to_string((*descriptor).kind)) : std::string("unknown"),
            .success = descriptor.has_value(),
            .duration_ns = static_cast<std::uint64_t>(schema_ns),
            .error = descriptor ? std::string{} : descriptor.error().message.value_or("descriptor failure"),
        };
        Telemetry::RecordSchemaSample(space_, schema_sample);
        if (!descriptor) {
            clear_dirty();
            if (descriptor.error().code == Error::Code::NoObjectFound
                || descriptor.error().code == Error::Code::InvalidPath) {
                cleanup_widget(widget_path);
            }
            return;
        }
        auto bucket = BuildWidgetBucket(*descriptor);
        if (!bucket) {
            clear_dirty();
            return;
        }
        auto compare_result = compare_existing_bucket(widget_path, *bucket);
        auto relative = make_relative(widget_path);
        auto structure_base = scene_path_ + "/structure/widgets" + relative;
        auto bucket_path = structure_base + "/render/bucket";
        if (auto stored = BuilderDetail::replace_single(space_, bucket_path, *bucket); !stored) {
            clear_dirty();
            return;
        }
        store_widget_bucket(widget_path, std::move(*bucket));
        submit_dirty_hints(widget_path);
        schedule_publish(widget_path);
        clear_dirty();
        ++events_processed_;
        write_metric("events_processed_total", events_processed_);
        auto dirty_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
                                                                             - dirty_start)
                            .count();
        Telemetry::RecordRenderDirtySample(space_,
                                           Telemetry::RenderDirtySample{
                                               .scene_path = scene_path_,
                                               .widget_path = widget_path,
                                               .duration_ns = static_cast<std::uint64_t>(dirty_ns),
                                           });
        Telemetry::RenderCompareSample render_compare{
            .scene_path = scene_path_,
            .parity_ok = compare_result.parity_ok,
        };
        if (compare_result.had_previous) {
            render_compare.diff_percent = compare_result.diff_percent;
            if (!compare_result.parity_ok) {
                std::ostringstream oss;
                oss << "widget=" << widget_path << " diff=" << compare_result.diff_percent << "%";
                Telemetry::AppendRenderCompareLog(space_, scene_path_, oss.str());
            }
        }
        Telemetry::RecordRenderCompareSample(space_, render_compare);
    }

    void schedule_publish(std::string const& widget_path) {
        auto now = std::chrono::steady_clock::now();
        if (!have_published_
            || options_.publish_throttle.count() == 0
            || now - last_publish_clock_ >= options_.publish_throttle) {
            publish_scene_snapshot(widget_path);
            return;
        }
        {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            pending_publish_reason_ = widget_path;
        }
        pending_publish_.store(true, std::memory_order_release);
        write_metric("pending_publish", true);
    }

    void flush_pending_publish() {
        if (!pending_publish_.load(std::memory_order_acquire)) {
            return;
        }
        if (options_.publish_throttle.count() > 0 && have_published_) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_publish_clock_ < options_.publish_throttle) {
                return;
            }
        }
        std::string reason;
        {
            std::lock_guard<std::mutex> guard(pending_mutex_);
            reason = pending_publish_reason_;
            pending_publish_reason_.clear();
        }
        pending_publish_.store(false, std::memory_order_release);
        write_metric("pending_publish", false);
        publish_scene_snapshot(reason.empty() ? scene_path_ : reason);
    }

    void publish_scene_snapshot(std::string const& reason) {
        auto publish_start = std::chrono::steady_clock::now();
        auto aggregate = aggregate_scene_bucket();
        if (!aggregate.has_value()) {
            return;
        }
        if (aggregate->drawable_ids.empty()) {
            return;
        }
        auto now = std::chrono::system_clock::now();
        SP::UI::Scene::SnapshotPublishOptions opts{};
        opts.metadata.author = std::string{kPublishAuthor};
        opts.metadata.tool_version = std::string{kPublishAuthor};
        opts.metadata.created_at = now;
        opts.metadata.drawable_count = aggregate->drawable_ids.size();
        opts.metadata.command_count = aggregate->command_kinds.size();

        auto revision = snapshot_builder_.publish(opts, *aggregate);
        if (!revision) {
            record_publish_failure(revision.error());
            return;
        }
        auto publish_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
                                                                               - publish_start)
                              .count();
        Telemetry::RecordRenderPublishSample(space_,
                                             Telemetry::RenderPublishSample{
                                                 .scene_path = scene_path_,
                                                 .duration_ns = static_cast<std::uint64_t>(publish_ns),
                                             });

        last_revision_ = *revision;
        last_publish_clock_ = std::chrono::steady_clock::now();
        have_published_ = true;
        write_metric("last_revision", last_revision_);
        write_metric("last_published_ms", to_epoch_ms(now));
        (void)BuilderDetail::replace_single<std::string>(space_, metrics_base_ + "/last_published_widget", reason);
    }

    void record_publish_failure(SP::Error const& error) {
        (void)BuilderDetail::replace_single<std::string>(space_,
                                                         metrics_base_ + "/last_error",
                                                         error.message.value_or("scene publish failed"));
    }

    [[nodiscard]] auto aggregate_scene_bucket() -> std::optional<SP::UI::Scene::DrawableBucketSnapshot> {
        std::map<std::string, std::shared_ptr<SP::UI::Scene::DrawableBucketSnapshot>> snapshot;
        {
            std::lock_guard<std::mutex> guard(bucket_mutex_);
            if (bucket_cache_.empty()) {
                return std::nullopt;
            }
            snapshot = bucket_cache_;
        }

        SP::UI::Scene::DrawableBucketSnapshot combined{};
        for (auto const& [_, bucket_ptr] : snapshot) {
            if (!bucket_ptr) {
                continue;
            }
            SP::UI::Scene::AppendDrawableBucket(combined, *bucket_ptr);
        }
        return combined;
    }

    void store_widget_bucket(std::string const& widget_path,
                             SP::UI::Scene::DrawableBucketSnapshot&& bucket) {
        auto shared = std::make_shared<SP::UI::Scene::DrawableBucketSnapshot>(std::move(bucket));
        {
            std::lock_guard<std::mutex> guard(bucket_mutex_);
            bucket_cache_[widget_path] = std::move(shared);
            bucket_cache_size_ = bucket_cache_.size();
        }
        write_metric("widgets_with_buckets", bucket_cache_size_);
    }

    void submit_dirty_hints(std::string const& widget_path) {
        if (!has_renderer_target_) {
            return;
        }
        auto pending_path = widget_path + "/render/buffer/pendingDirty";
        auto pending = BuilderDetail::read_optional<std::vector<DirtyRectHint>>(space_, pending_path);
        if (!pending || !pending->has_value()) {
            return;
        }
        auto hints = **pending;
        if (hints.empty()) {
            return;
        }
        auto target_view = SP::ConcretePathStringView{renderer_target_path_};
        auto submitted = BuilderRenderer::SubmitDirtyRects(space_, target_view, hints);
        if (!submitted) {
            return;
        }
        (void)BuilderDetail::replace_single(space_, pending_path, std::vector<DirtyRectHint>{});
    }

    void remove_widget_bucket(std::string const& widget_path) {
        bool erased = false;
        {
            std::lock_guard<std::mutex> guard(bucket_mutex_);
            erased = bucket_cache_.erase(widget_path) > 0;
            bucket_cache_size_ = bucket_cache_.size();
        }
        if (erased) {
            write_metric("widgets_with_buckets", bucket_cache_size_);
        }
    }

    auto compare_existing_bucket(std::string const& widget_path,
                                 SP::UI::Scene::DrawableBucketSnapshot const& bucket) -> BucketCompareResult {
        std::shared_ptr<SP::UI::Scene::DrawableBucketSnapshot> previous;
        {
            std::lock_guard<std::mutex> guard(bucket_mutex_);
            auto it = bucket_cache_.find(widget_path);
            if (it != bucket_cache_.end()) {
                previous = it->second;
            }
        }
        BucketCompareResult result{};
        if (!previous) {
            return result;
        }
        result.had_previous = true;
        auto const& prev = *previous;
        bool same = prev.drawable_ids == bucket.drawable_ids
            && prev.command_kinds == bucket.command_kinds
            && prev.drawable_fingerprints == bucket.drawable_fingerprints;
        result.parity_ok = same;
        if (!same) {
            auto total = std::max(prev.drawable_ids.size(), bucket.drawable_ids.size());
            if (total == 0) {
                result.diff_percent = 0.0f;
            } else {
                std::size_t matched = 0;
                auto limit = std::min(prev.drawable_fingerprints.size(), bucket.drawable_fingerprints.size());
                if (limit == 0) {
                    limit = std::min(prev.drawable_ids.size(), bucket.drawable_ids.size());
                    for (std::size_t i = 0; i < limit; ++i) {
                        if (prev.drawable_ids[i] == bucket.drawable_ids[i]) {
                            ++matched;
                        }
                    }
                } else {
                    for (std::size_t i = 0; i < limit; ++i) {
                        if (prev.drawable_fingerprints[i] == bucket.drawable_fingerprints[i]) {
                            ++matched;
                        }
                    }
                }
                auto diff = total > matched ? total - matched : 0;
                result.diff_percent = static_cast<float>(diff) / static_cast<float>(total) * 100.0f;
            }
        } else {
            result.diff_percent = 0.0f;
        }
        return result;
    }

    void cleanup_widget(std::string const& widget_root) {
        cleanup_widget_subtree(widget_root);
        remove_widget_bucket(widget_root);

        auto relative = make_relative(widget_root);
        auto structure_base = scene_path_ + "/structure/widgets" + relative;
        auto bucket_path = structure_base + "/render/bucket";
        (void)BuilderDetail::replace_single(space_, bucket_path, SP::UI::Scene::DrawableBucketSnapshot{});
    }

    void cleanup_widget_subtree(std::string const& widget_root) {
        deregister_source(widget_root + "/render/events/dirty");
        auto children_root = widget_root + "/children";
        auto children = space_.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& child : children) {
            cleanup_widget_subtree(children_root + "/" + child);
        }
    }

    [[nodiscard]] auto is_widget_removed(std::string const& widget_root) -> bool {
        auto removed = space_.read<bool, std::string>(widget_root + "/state/removed");
        if (!removed) {
            auto code = removed.error().code;
            if (code == Error::Code::NoObjectFound || code == Error::Code::NoSuchPath) {
                return false;
            }
            return false;
        }
        return *removed;
    }

    auto make_relative(std::string const& absolute) const -> std::string {
        if (absolute.rfind(app_root_path_, 0) != 0) {
            return absolute;
        }
        auto relative = absolute.substr(app_root_path_.size());
        if (relative.empty()) {
            return "/";
        }
        if (relative.front() != '/') {
            relative.insert(relative.begin(), '/');
        }
        return relative;
    }

    template <typename T>
    void write_metric(std::string const& leaf, T const& value) {
        (void)BuilderDetail::replace_single<T>(space_, metrics_base_ + "/" + leaf, value);
    }

    void handle_worker_exception(std::string_view reason) noexcept {
        pending_publish_.store(false, std::memory_order_release);
        auto message = std::string(reason);
        sp_log("SceneLifecycleWorker[" + scene_path_ + "] terminated: " + message, "SceneLifecycle");
        try {
            write_metric("last_error", message);
        } catch (...) {
            sp_log("SceneLifecycleWorker[" + scene_path_ + "] failed to write last_error metric", "SceneLifecycle");
        }
        try {
            (void)BuilderDetail::replace_single<bool>(space_,
                                                      scene_path_ + "/runtime/lifecycle/state/running",
                                                      false);
        } catch (...) {
            sp_log("SceneLifecycleWorker[" + scene_path_ + "] failed to update running state", "SceneLifecycle");
        }
    }

private:
    PathSpace& space_;
    std::string app_root_path_;
    std::string scene_path_;
    std::string window_path_;
    std::string view_name_;
    Options options_;
    SP::App::AppRootPath app_root_value_;
    SP::UI::Scene::ScenePath scene_path_value_;
    SP::UI::Scene::SceneSnapshotBuilder snapshot_builder_;
    std::string window_widgets_root_;
    std::string trellis_path_;
    std::string trellis_enable_path_;
    std::string trellis_disable_path_;
    std::string control_queue_path_;
    std::string theme_invalidate_command_;
    std::string force_publish_command_;
    std::string metrics_base_;
    std::string renderer_target_path_;
    std::thread worker_;
    std::atomic<bool> stop_flag_{false};
    std::mutex registration_mutex_;
    std::unordered_set<std::string> registered_sources_;
    std::uint64_t widgets_registered_ = 0;
    std::uint64_t events_processed_ = 0;
    std::uint64_t active_sources_ = 0;
    std::uint64_t bucket_cache_size_ = 0;
    std::uint64_t last_revision_ = 0;
    bool have_published_ = false;
    std::atomic<bool> pending_publish_{false};
    std::mutex pending_mutex_;
    std::string pending_publish_reason_;
    std::chrono::steady_clock::time_point last_publish_clock_{};
    std::mutex bucket_mutex_;
    std::map<std::string, std::shared_ptr<SP::UI::Scene::DrawableBucketSnapshot>> bucket_cache_;
    bool has_renderer_target_ = false;
};

std::mutex g_lifecycle_mutex;
std::unordered_map<std::string, std::shared_ptr<SceneLifecycleWorker>> g_lifecycle_workers;

} // namespace

auto Start(PathSpace& space,
           SP::App::AppRootPathView app_root,
           SP::UI::Builders::ScenePath const& scene_path,
           SP::UI::Builders::WindowPath const& window_path,
           std::string_view view_name,
           Options const& options) -> SP::Expected<void> {
    auto key = std::string(scene_path.getPath());
    std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
    if (g_lifecycle_workers.contains(key)) {
        return {};
    }

    auto worker = std::make_shared<SceneLifecycleWorker>(space,
                                                         std::string(app_root.getPath()),
                                                         key,
                                                         std::string(window_path.getPath()),
                                                         std::string(view_name),
                                                         options);
    auto status = worker->start();
    if (!status) {
        return status;
    }
    g_lifecycle_workers.emplace(key, worker);
    return {};
}

auto Stop(PathSpace& space,
          SP::UI::Builders::ScenePath const& scene_path) -> SP::Expected<void> {
    (void)space;
    std::shared_ptr<SceneLifecycleWorker> worker;
    {
        std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
        auto key = std::string(scene_path.getPath());
        auto it = g_lifecycle_workers.find(key);
        if (it != g_lifecycle_workers.end()) {
            worker = it->second;
            g_lifecycle_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
    return {};
}

auto ForcePublish(PathSpace& space,
                  SP::UI::Builders::ScenePath const& scene_path) -> SP::Expected<void> {
    (void)space;
    std::shared_ptr<SceneLifecycleWorker> worker;
    {
        std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
        auto key = std::string(scene_path.getPath());
        auto it = g_lifecycle_workers.find(key);
        if (it != g_lifecycle_workers.end()) {
            worker = it->second;
        }
    }
    if (worker) {
        worker->request_force_publish();
    }
    return {};
}

auto InvalidateThemes(PathSpace& space,
                      SP::App::AppRootPathView app_root) -> void {
    (void)space;
    std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
    for (auto const& [_, worker] : g_lifecycle_workers) {
        if (worker && worker->matches_app(app_root.getPath())) {
            worker->request_theme_invalidation();
        }
    }
}

auto StopAll(PathSpace& space) -> void {
    std::vector<std::shared_ptr<SceneLifecycleWorker>> workers;
    {
        std::lock_guard<std::mutex> guard(g_lifecycle_mutex);
        for (auto it = g_lifecycle_workers.begin(); it != g_lifecycle_workers.end();) {
            if (it->second && it->second->owns_space(space)) {
                workers.push_back(it->second);
                it = g_lifecycle_workers.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& worker : workers) {
        worker->stop();
    }
}

} // namespace SP::UI::Declarative::SceneLifecycle
