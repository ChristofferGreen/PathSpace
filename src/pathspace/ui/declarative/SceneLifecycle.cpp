#include <pathspace/ui/declarative/SceneLifecycle.hpp>

#include <pathspace/layer/PathSpaceTrellis.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/declarative/Descriptor.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/path/ConcretePath.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SP::UI::Declarative::SceneLifecycle {

namespace {

namespace BuilderDetail = SP::UI::Builders::Detail;

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
        , options_(options) {
        window_widgets_root_ = window_path_ + "/widgets";
        trellis_path_ = scene_path_ + "/runtime/lifecycle/trellis";
        trellis_enable_path_ = trellis_path_ + "/_system/enable";
        control_queue_path_ = scene_path_ + "/runtime/lifecycle/control";
        metrics_base_ = scene_path_ + "/runtime/lifecycle/metrics";
    }

    ~SceneLifecycleWorker() {
        stop();
    }

    auto start() -> SP::Expected<void> {
        auto mounted = mount_trellis();
        if (!mounted) {
            return mounted;
        }
        register_source(control_queue_path_);
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

private:
    auto mount_trellis() -> SP::Expected<void> {
        auto alias = std::shared_ptr<PathSpaceBase>(&space_, [](PathSpaceBase*) {});
        auto trellis = std::make_unique<PathSpaceTrellis>(alias);
        auto inserted = space_.insert(trellis_path_, std::move(trellis));
        if (!inserted.errors.empty()) {
            return std::unexpected(inserted.errors.front());
        }
        (void)BuilderDetail::replace_single<bool>(space_, scene_path_ + "/runtime/lifecycle/state/running", true);
        (void)BuilderDetail::replace_single<std::uint64_t>(space_, metrics_base_ + "/widgets_registered_total", widgets_registered_);
        (void)BuilderDetail::replace_single<std::uint64_t>(space_, metrics_base_ + "/events_processed_total", events_processed_);
        return {};
    }

    void run() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            register_widget_sources();

            auto result = space_.take<std::string>(trellis_path_, SP::Out{} & SP::Block{options_.trellis_wait});
            if (!result) {
                auto const& error = result.error();
                if (error.code == Error::Code::NoObjectFound || error.code == Error::Code::Timeout) {
                    continue;
                }
                continue;
            }

            auto const& widget_path = *result;
            if (widget_path == control_queue_path_) {
                continue;
            }
            process_event(widget_path);
        }
    }

    void register_widget_sources() {
        auto windows_widgets = space_.listChildren(SP::ConcretePathStringView{window_widgets_root_});
        for (auto const& widget_name : windows_widgets) {
            auto widget_root = window_widgets_root_ + "/" + widget_name;
            register_widget_subtree(widget_root);
        }
    }

    void register_widget_subtree(std::string const& widget_root) {
        register_source(widget_root + "/render/events/dirty");

        auto children_root = widget_root + "/children";
        auto children = space_.listChildren(SP::ConcretePathStringView{children_root});
        for (auto const& child_name : children) {
            register_widget_subtree(children_root + "/" + child_name);
        }
    }

    void register_source(std::string const& queue_path) {
        std::lock_guard<std::mutex> guard(registration_mutex_);
        auto [_, inserted] = registered_sources_.insert(queue_path);
        if (!inserted) {
            return;
        }
        auto ret = space_.insert(trellis_enable_path_, queue_path);
        if (!ret.errors.empty()) {
            registered_sources_.erase(queue_path);
            return;
        }
        ++widgets_registered_;
        (void)BuilderDetail::replace_single<std::uint64_t>(space_, metrics_base_ + "/widgets_registered_total", widgets_registered_);
    }

    void process_event(std::string const& widget_path) {
        SP::UI::Builders::WidgetPath widget{widget_path};
        bool cleared = false;
        auto clear_dirty = [&]() {
            if (cleared) {
                return;
            }
            (void)BuilderDetail::replace_single<bool>(space_, widget_path + "/render/dirty", false);
            cleared = true;
        };
        auto descriptor = LoadWidgetDescriptor(space_, widget);
        if (!descriptor) {
            clear_dirty();
            return;
        }
        auto bucket = BuildWidgetBucket(*descriptor);
        if (!bucket) {
            clear_dirty();
            return;
        }
        auto relative = make_relative(widget_path);
        auto structure_base = scene_path_ + "/structure/widgets" + relative;
        auto bucket_path = structure_base + "/render/bucket";
        if (auto stored = BuilderDetail::replace_single(space_, bucket_path, *bucket); !stored) {
            clear_dirty();
            return;
        }
        clear_dirty();
        ++events_processed_;
        (void)BuilderDetail::replace_single<std::uint64_t>(space_, metrics_base_ + "/events_processed_total", events_processed_);
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

private:
    PathSpace& space_;
    std::string app_root_path_;
    std::string scene_path_;
    std::string window_path_;
    std::string view_name_;
    Options options_;
    std::string window_widgets_root_;
    std::string trellis_path_;
    std::string trellis_enable_path_;
    std::string control_queue_path_;
    std::string metrics_base_;
    std::thread worker_;
    std::atomic<bool> stop_flag_{false};
    std::mutex registration_mutex_;
    std::unordered_set<std::string> registered_sources_;
    std::uint64_t widgets_registered_ = 0;
    std::uint64_t events_processed_ = 0;
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

} // namespace SP::UI::Declarative::SceneLifecycle
