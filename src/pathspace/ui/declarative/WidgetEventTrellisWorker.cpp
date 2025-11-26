#include "WidgetEventTrellisWorker.hpp"

#include "WidgetStateMutators.hpp"
#include "widgets/Common.hpp"

#include "../BuildersDetail.hpp"

#include <pathspace/app/AppPaths.hpp>

#include <chrono>

namespace SP::UI::Declarative {

namespace BuilderDetail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;
using BuilderDetail::derive_app_root_for;
using BuilderDetail::read_optional;
using BuilderDetail::replace_single;
using BuilderDetail::to_epoch_ns;
using SP::UI::ScenePath;

WidgetEventTrellisWorker::WidgetEventTrellisWorker(PathSpace& space,
                                                   WidgetEventTrellisOptions options)
    : space_(space)
    , options_(std::move(options))
    , windows_root_(normalize_root(options_.windows_root))
    , events_root_(normalize_root(options_.events_root))
    , metrics_root_(options_.metrics_root.empty()
              ? std::string{"/system/widgets/runtime/events/metrics"}
              : options_.metrics_root)
    , log_root_(options_.log_root.empty()
          ? std::string{"/system/widgets/runtime/events/log"}
          : options_.log_root)
    , state_path_(options_.state_path.empty()
          ? std::string{"/system/widgets/runtime/events/state/running"}
          : options_.state_path) {}

WidgetEventTrellisWorker::~WidgetEventTrellisWorker() {
    stop();
}

auto WidgetEventTrellisWorker::make_scene_absolute(std::string const& app_root,
                                    std::string const& stored) -> std::string {
        if (stored.empty()) {
            return {};
        }
        if (!stored.empty() && stored.front() == '/') {
            return stored;
        }
        std::string absolute = app_root;
        if (!absolute.empty() && absolute.back() != '/') {
            absolute.push_back('/');
        }
        absolute.append(stored);
        return absolute;
    }

auto WidgetEventTrellisWorker::start() -> SP::Expected<void> {
        if (auto ensured = ensure_roots(); !ensured) {
            return ensured;
        }
        worker_ = std::thread([self = shared_from_this()] { self->run(); });
        return {};
    }

void WidgetEventTrellisWorker::stop() {
        bool expected = false;
        if (!stop_requested_.compare_exchange_strong(expected, true)) {
            return;
        }
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

auto WidgetEventTrellisWorker::ensure_roots() -> SP::Expected<void> {
        if (auto status = replace_single<bool>(space_, state_path_, false); !status) {
            return status;
        }
        auto pointer_path = metrics_root_ + "/pointer_events_total";
        auto button_path = metrics_root_ + "/button_events_total";
        auto ops_path = metrics_root_ + "/widget_ops_total";
        auto hit_path = metrics_root_ + "/hit_test_failures_total";
        auto dispatch_path = metrics_root_ + "/last_dispatch_ns";
        if (auto status = replace_single<std::uint64_t>(space_, pointer_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, button_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, ops_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, hit_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, dispatch_path, 0); !status) {
            return status;
        }
        return {};
    }

void WidgetEventTrellisWorker::publish_running(bool value) {
        (void)replace_single<bool>(space_, state_path_, value);
    }

void WidgetEventTrellisWorker::run() {
        publish_running(true);
        auto next_refresh = std::chrono::steady_clock::now();

        while (!stop_flag_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= next_refresh) {
                refresh_bindings();
                next_refresh = std::chrono::steady_clock::now() + options_.refresh_interval;
            }

            bool processed = false;
            for (auto& binding : bindings_) {
                processed |= drain_pointer(binding);
                processed |= drain_button(binding);
                processed |= drain_text(binding);
            }

            if (!processed) {
                std::this_thread::sleep_for(options_.idle_sleep);
            } else {
                publish_metrics();
            }
        }

        publish_running(false);
        publish_metrics();
    }

void WidgetEventTrellisWorker::refresh_bindings() {
        auto tokens = list_children(space_, windows_root_);
        std::unordered_map<std::string, WindowBinding> updated;

        for (auto const& token : tokens) {
            auto maybe_binding = build_binding(token);
            if (!maybe_binding) {
                continue;
            }
            updated.emplace(token, std::move(*maybe_binding));
        }

        bindings_.clear();
        for (auto& [token, binding] : updated) {
            bindings_.push_back(std::move(binding));
        }
    }

auto WidgetEventTrellisWorker::build_binding(std::string const& token) -> std::optional<WindowBinding> {
        std::string base = windows_root_;
        base.push_back('/');
        base.append(token);

        auto window_path = read_optional<std::string>(space_, base + "/window");
        if (!window_path || !window_path->has_value()) {
            return std::nullopt;
        }

        auto app_root = derive_app_root_for(SP::App::ConcretePathView{**window_path});
        if (!app_root) {
            return std::nullopt;
        }

        WindowBinding binding{};
        binding.token = token;
        binding.window_path = **window_path;
        binding.app_root = app_root->getPath();
        binding.pointer_queue = events_root_ + "/" + token + "/pointer/queue";
        binding.button_queue = events_root_ + "/" + token + "/button/queue";
        binding.text_queue = events_root_ + "/" + token + "/text/queue";
        binding.scene_path = resolve_scene_path(binding);
        return binding;
    }

auto WidgetEventTrellisWorker::resolve_scene_path(WindowBinding const& binding) -> std::string {
        std::string views_root = binding.window_path + "/views";
        auto views = list_children(space_, views_root);
        for (auto const& view_name : views) {
            auto scene_rel = read_optional<std::string>(
                space_, views_root + "/" + view_name + "/scene");
            if (!scene_rel || !scene_rel->has_value()) {
                continue;
            }
            auto absolute = make_scene_absolute(binding.app_root, **scene_rel);
            if (!absolute.empty()) {
                return absolute;
            }
        }
        return {};
    }

auto WidgetEventTrellisWorker::drain_pointer(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::PointerEvent, std::string>(binding.pointer_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis pointer read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            ++pointer_events_total_;
            handle_pointer_event(binding, *taken);
        }
        return processed;
    }

auto WidgetEventTrellisWorker::drain_button(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::ButtonEvent, std::string>(binding.button_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis button read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            ++button_events_total_;
            handle_button_event(binding, *taken);
        }
        return processed;
    }

auto WidgetEventTrellisWorker::drain_text(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::TextEvent, std::string>(binding.text_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis text read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            handle_text_event(binding, *taken);
        }
        return processed;
    }

auto WidgetEventTrellisWorker::run_hit_test(WindowBinding const& binding,
                      PointerState const& state) -> SP::Expected<BuildersScene::HitTestResult> {
        auto override_hit = options_.hit_test_override;
        if (override_hit) {
            return override_hit(space_, binding.scene_path, state.x, state.y);
        }

        BuildersScene::HitTestRequest request{};
        request.x = state.x;
        request.y = state.y;
        request.max_results = 1;
        auto scene_path = ScenePath{binding.scene_path};
        return BuildersScene::HitTest(space_, scene_path, request);
    }

auto WidgetEventTrellisWorker::pointer_state(std::string const& token) -> PointerState& {
        std::lock_guard<std::mutex> guard(pointer_mutex_);
        return pointer_states_[token];
    }

void WidgetEventTrellisWorker::publish_metrics() {
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/pointer_events_total", pointer_events_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/button_events_total", button_events_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/widget_ops_total", widget_ops_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/hit_test_failures_total", hit_test_failures_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/last_dispatch_ns", last_dispatch_ns_);
    }

void WidgetEventTrellisWorker::emit_widget_op(WindowBinding const& binding,
                        TargetInfo const& target,
                        WidgetBindings::WidgetOpKind kind,
                        float value,
                        bool inside,
                        std::optional<WidgetBindings::PointerInfo> pointer_override) {
        if (target.kind == TargetKind::Unknown) {
            return;
        }

        auto const& ptr_state = pointer_state(binding.token);
        WidgetBindings::PointerInfo pointer = pointer_override.value_or(
            WidgetBindings::PointerInfo::Make(ptr_state.x, ptr_state.y));
        if (!pointer_override) {
            pointer.WithInside(inside);
            pointer.WithPrimary(true);
            if (target.has_local) {
                pointer.WithLocal(target.local_x, target.local_y);
            }
        }

        WidgetBindings::WidgetOp op{};
        op.kind = kind;
        op.widget_path = target.widget_path;
        op.target_id = target.component;
        op.pointer = pointer;
        op.value = value;
        op.sequence = BuilderDetail::g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

        auto queue_path = target.widget_path + "/ops/inbox/queue";
        auto inserted = space_.insert(queue_path, op);
        if (!inserted.errors.empty()) {
            enqueue_error(space_, "WidgetEventTrellis failed to write WidgetOp for "
                    + target.widget_path + ": " + inserted.errors.front().message.value_or("unknown error"));
            return;
        }

        ++widget_ops_total_;
        last_dispatch_ns_ = now_ns();
    }

} // namespace SP::UI::Declarative
