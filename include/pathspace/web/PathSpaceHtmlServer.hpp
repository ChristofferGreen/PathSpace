#pragma once

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/web/HtmlMirror.hpp>
#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/auth/Credentials.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace SP::ServeHtml {

struct RemoteMountSource {
    std::string alias;
    std::string metrics_root{"/inspector/metrics/remotes"};
    std::string mount_prefix{"/remote"};
    bool        require_healthy{true};
};

struct HtmlMirrorBootstrap {
    SP::App::AppRootPath          app_root;
    SP::UI::Runtime::WindowPath   window;
    SP::UI::Runtime::ScenePath    scene;
    HtmlMirrorConfig              mirror_config{};
    bool                          present_on_start{true};
};

struct PathSpaceHtmlServerOptions {
    ServeHtmlOptions             serve_html{};
    bool                         attach_default_targets{false};
    bool                         seed_demo_credentials{false};
    std::optional<std::string>   remote_mount_alias{}; // Deprecated: use remote_mount
    std::optional<RemoteMountSource> remote_mount{};
    std::optional<HtmlMirrorBootstrap> html_mirror{};
    std::optional<ServeHtmlLogHooks>   log_hooks{};
};

template <class Space>
class PathSpaceHtmlServer {
    static_assert(std::is_base_of_v<ServeHtmlSpace, Space>,
                  "PathSpaceHtmlServer expects Space to derive from ServeHtmlSpace");

public:
    using ServerLauncher = std::function<int(ServeHtmlSpace&,
                                             ServeHtmlOptions const&,
                                             std::atomic<bool>&,
                                             ServeHtmlLogHooks const&,
                                             std::function<void(SP::Expected<void>)>)>;

    class Builder;

    PathSpaceHtmlServer(Space&             space,
                        PathSpaceHtmlServerOptions options = {},
                        ServerLauncher             launcher = default_server_launcher());

    PathSpaceHtmlServer(PathSpaceHtmlServer const&)            = delete;
    auto operator=(PathSpaceHtmlServer const&) -> PathSpaceHtmlServer& = delete;

    ~PathSpaceHtmlServer();

    [[nodiscard]] auto start() -> SP::Expected<void>;
    void                stop();
    [[nodiscard]] auto is_running() const -> bool { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] auto space() -> Space& { return *space_; }
    [[nodiscard]] auto space() const -> Space const& { return *space_; }
    [[nodiscard]] auto options() const -> PathSpaceHtmlServerOptions const& { return options_; }
    [[nodiscard]] auto mirror_context() const -> std::optional<HtmlMirrorContext> const& {
        return html_mirror_context_;
    }

    template <typename DataType>
    auto forward_insert(std::string const& path, DataType&& data) -> SP::Expected<void>;

    template <typename DataType>
    auto forward_read(std::string const& path) const -> SP::Expected<DataType>;

    auto forward_list_children(std::string const& path) const -> SP::Expected<std::vector<std::string>>;

private:
    struct ResolvedRemoteMount {
        RemoteMountSource source;
        std::string       alias_name;
        std::string       alias_path;
        std::string       metrics_root;
    };

    static auto default_server_launcher() -> ServerLauncher;
    static auto choose_listen_port(int requested_port) -> SP::Expected<int>;
    auto        validate_options(PathSpaceHtmlServerOptions const& options) const -> SP::Expected<void>;
    auto        resolve_remote_mount(PathSpaceHtmlServerOptions const& options) const
        -> SP::Expected<std::optional<ResolvedRemoteMount>>;
    auto        apply_remote_mount(PathSpaceHtmlServerOptions& options,
                                   std::optional<ResolvedRemoteMount> const& remote) const
        -> SP::Expected<void>;
    auto        attach_default_html_targets(PathSpaceHtmlServerOptions& options,
                                            std::optional<ResolvedRemoteMount> const& remote)
        -> SP::Expected<void>;
    static auto prefix_remote_path(std::string const& path, std::optional<ResolvedRemoteMount> const& mount)
        -> SP::Expected<std::string>;
    [[nodiscard]] auto select_remote_mount(PathSpaceHtmlServerOptions const& options) const
        -> std::optional<RemoteMountSource>;
    static auto normalize_remote_alias(RemoteMountSource const& source)
        -> SP::Expected<std::pair<std::string, std::string>>;
    static auto prefix_under_alias(std::string const& alias_path, std::string const& value)
        -> SP::Expected<std::string>;

    Space*                               space_{nullptr};
    PathSpaceHtmlServerOptions           options_{};
    ServerLauncher                       launcher_{};
    std::thread                          server_thread_{};
    std::shared_ptr<std::atomic<bool>>   stop_flag_{std::make_shared<std::atomic<bool>>(false)};
    std::atomic<bool>                    running_{false};
    std::optional<HtmlMirrorContext>     html_mirror_context_{};
};

template <class Space>
class PathSpaceHtmlServer<Space>::Builder {
public:
    explicit Builder(Space& space)
        : space_(space) {}

    Builder& options(PathSpaceHtmlServerOptions options) {
        options_ = std::move(options);
        return *this;
    }

    Builder& serve_html_options(ServeHtmlOptions options) {
        options_.serve_html = std::move(options);
        return *this;
    }

    Builder& attach_default_targets(bool enable) {
        options_.attach_default_targets = enable;
        return *this;
    }

    Builder& html_mirror(HtmlMirrorBootstrap bootstrap) {
        options_.html_mirror            = std::move(bootstrap);
        options_.attach_default_targets = true;
        return *this;
    }

    Builder& seed_demo_credentials(bool enable) {
        options_.seed_demo_credentials = enable;
        return *this;
    }

    Builder& remote_mount_alias(std::optional<std::string> alias) {
        options_.remote_mount_alias = std::move(alias);
        options_.remote_mount.reset();
        return *this;
    }

    Builder& remote_mount(RemoteMountSource source) {
        options_.remote_mount      = std::move(source);
        options_.remote_mount_alias.reset();
        return *this;
    }

    Builder& log_hooks(ServeHtmlLogHooks hooks) {
        options_.log_hooks = std::move(hooks);
        return *this;
    }

    Builder& launcher(ServerLauncher launcher) {
        launcher_ = std::move(launcher);
        return *this;
    }

    auto build() -> PathSpaceHtmlServer {
        auto launcher = launcher_.value_or(PathSpaceHtmlServer::default_server_launcher());
        return PathSpaceHtmlServer{space_, options_, std::move(launcher)};
    }

private:
    Space&                             space_;
    PathSpaceHtmlServerOptions         options_{};
    std::optional<ServerLauncher>      launcher_{};
};

template <class Space>
auto PathSpaceHtmlServer<Space>::default_server_launcher() -> ServerLauncher {
    return [](ServeHtmlSpace&                     space,
              ServeHtmlOptions const&            options,
              std::atomic<bool>&                 stop_flag,
              ServeHtmlLogHooks const&           log_hooks,
              std::function<void(SP::Expected<void>)> on_listen) {
        return RunServeHtmlServerWithStopFlag(space, options, stop_flag, log_hooks, std::move(on_listen));
    };
}

template <class Space>
auto PathSpaceHtmlServer<Space>::choose_listen_port(int requested_port) -> SP::Expected<int> {
    if (requested_port < 0) {
        return std::unexpected(
            SP::Error{SP::Error::Code::MalformedInput, "serve-html port must be >= 0"});
    }

    if (requested_port > 0) {
        return requested_port;
    }

    std::random_device                 rd;
    std::mt19937                       gen{rd()};
    std::uniform_int_distribution<int> dist(20000, 60000);
    int                                port = dist(gen);

    if (port <= 0) {
        return std::unexpected(
            SP::Error{SP::Error::Code::InvalidError, "failed to choose serve-html listen port"});
    }

    return port;
}

template <class Space>
PathSpaceHtmlServer<Space>::PathSpaceHtmlServer(Space&                       space,
                                                PathSpaceHtmlServerOptions   options,
                                                ServerLauncher               launcher)
    : space_(&space)
    , options_(std::move(options))
    , launcher_(std::move(launcher)) {
    if (!launcher_) {
        launcher_ = default_server_launcher();
    }
}

template <class Space>
PathSpaceHtmlServer<Space>::~PathSpaceHtmlServer() {
    stop();
}

template <class Space>
auto PathSpaceHtmlServer<Space>::validate_options(PathSpaceHtmlServerOptions const& options) const
    -> SP::Expected<void> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    if (auto validation_error = ValidateServeHtmlOptions(options.serve_html)) {
        return std::unexpected(SP::Error{SP::Error::Code::MalformedInput, *validation_error});
    }

    return {};
}

template <class Space>
auto PathSpaceHtmlServer<Space>::start() -> SP::Expected<void> {
    if (server_thread_.joinable()) {
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected(
                SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer already running"});
        }
        server_thread_.join();
    }

    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    PathSpaceHtmlServerOptions options = options_;
    html_mirror_context_.reset();

    auto resolved_remote = resolve_remote_mount(options);
    if (!resolved_remote) {
        return std::unexpected(resolved_remote.error());
    }

    if (auto remote_applied = apply_remote_mount(options, resolved_remote.value()); !remote_applied) {
        return remote_applied;
    }

    auto selected_port = choose_listen_port(options.serve_html.port);
    if (!selected_port) {
        return std::unexpected(selected_port.error());
    }
    options.serve_html.port = *selected_port;

    if (auto attach = attach_default_html_targets(options, resolved_remote.value()); !attach) {
        return attach;
    }

    if (auto validated = validate_options(options); !validated) {
        return validated;
    }

    if (options.seed_demo_credentials) {
        SeedDemoCredentials(*space_, options.serve_html);
    }

    stop_flag_->store(false, std::memory_order_release);
    ResetServeHtmlStopFlag();
    running_.store(true, std::memory_order_release);

    Space*                             space           = space_;
    auto                               stop_flag       = stop_flag_;
    ServerLauncher                     launcher        = launcher_;
    std::atomic<bool>*                 running_ptr     = &running_;
    auto                               ready_promise   = std::make_shared<std::promise<SP::Expected<void>>>();
    auto                               ready_future    = ready_promise->get_future();
    auto                               ready_reported  = std::make_shared<std::atomic<bool>>(false);
    ServeHtmlLogHooks                  log_hooks       = options.log_hooks.value_or(ServeHtmlLogHooks{});

    options_ = options;

    server_thread_ = std::thread([space,
                                  options,
                                  stop_flag,
                                  launcher,
                                  running_ptr,
                                  ready_promise,
                                  ready_reported,
                                  log_hooks]() mutable {
        auto on_listen = [ready_promise, ready_reported](SP::Expected<void> status) {
            bool expected = false;
            if (!ready_reported->compare_exchange_strong(expected, true)) {
                return;
            }
            ready_promise->set_value(std::move(status));
        };

        auto exit_code = launcher(*space, options.serve_html, *stop_flag, log_hooks, on_listen);
        if (!ready_reported->load(std::memory_order_acquire)) {
            ready_promise->set_value(SP::Expected<void>{});
        }
        (void)exit_code;
        running_ptr->store(false, std::memory_order_release);
    });

    auto wait_status = ready_future.wait_for(std::chrono::milliseconds(750));
    if (wait_status == std::future_status::ready) {
        auto status = ready_future.get();
        if (!status) {
            stop_flag->store(true, std::memory_order_release);
            RequestServeHtmlStop();
            server_thread_.join();
            ResetServeHtmlStopFlag();
            running_.store(false, std::memory_order_release);
            return std::unexpected(status.error());
        }
    }

    return {};
}

template <class Space>
void PathSpaceHtmlServer<Space>::stop() {
    if (!server_thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        return;
    }

    if (stop_flag_) {
        stop_flag_->store(true, std::memory_order_release);
    }

    RequestServeHtmlStop();

    server_thread_.join();
    ResetServeHtmlStopFlag();
    running_.store(false, std::memory_order_release);
}

template <class Space>
template <typename DataType>
auto PathSpaceHtmlServer<Space>::forward_insert(std::string const& path, DataType&& data)
    -> SP::Expected<void> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    auto resolved = resolve_remote_mount(options_);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto prefixed = prefix_remote_path(path, resolved.value());
    if (!prefixed) {
        return std::unexpected(prefixed.error());
    }

    auto inserted = space_->insert(*prefixed, std::forward<DataType>(data));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }

    return {};
}

template <class Space>
template <typename DataType>
auto PathSpaceHtmlServer<Space>::forward_read(std::string const& path) const -> SP::Expected<DataType> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    auto resolved = resolve_remote_mount(options_);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto prefixed = prefix_remote_path(path, resolved.value());
    if (!prefixed) {
        return std::unexpected(prefixed.error());
    }

    return space_->template read<DataType, std::string>(*prefixed);
}

template <class Space>
auto PathSpaceHtmlServer<Space>::forward_list_children(std::string const& path) const
    -> SP::Expected<std::vector<std::string>> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    auto resolved = resolve_remote_mount(options_);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto prefixed = prefix_remote_path(path, resolved.value());
    if (!prefixed) {
        return std::unexpected(prefixed.error());
    }

    SP::ConcretePathStringView validated{*prefixed};
    auto                        canonical = validated.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }

    SP::ConcretePathStringView canonical_view{canonical->getPath()};
    auto                       children = space_->listChildren(canonical_view);
    return children;
}

template <class Space>
auto PathSpaceHtmlServer<Space>::select_remote_mount(PathSpaceHtmlServerOptions const& options) const
    -> std::optional<RemoteMountSource> {
    if (options.remote_mount) {
        return options.remote_mount;
    }
    if (options.remote_mount_alias) {
        RemoteMountSource source{};
        source.alias = *options.remote_mount_alias;
        return source;
    }
    return std::nullopt;
}

template <class Space>
auto PathSpaceHtmlServer<Space>::resolve_remote_mount(PathSpaceHtmlServerOptions const& options) const
    -> SP::Expected<std::optional<ResolvedRemoteMount>> {
    if (space_ == nullptr) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError, "PathSpaceHtmlServer missing space"});
    }

    auto remote = select_remote_mount(options);
    if (!remote) {
        return std::optional<ResolvedRemoteMount>{};
    }

    auto normalized = normalize_remote_alias(*remote);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }

    std::string metrics_root = remote->metrics_root.empty() ? std::string{"/inspector/metrics/remotes"}
                                                            : remote->metrics_root;
    if (!metrics_root.empty() && metrics_root.front() != '/') {
        metrics_root.insert(metrics_root.begin(), '/');
    }
    while (metrics_root.size() > 1 && metrics_root.back() == '/') {
        metrics_root.pop_back();
    }

    if (remote->require_healthy) {
        std::string connected_path = metrics_root + "/" + normalized->first + "/client/connected";
        auto        connected      = space_->template read<int>(connected_path);
        if (!connected) {
            return std::unexpected(connected.error());
        }
        if (connected.value() == 0) {
            return std::unexpected(
                SP::Error{SP::Error::Code::InvalidError, "remote mount not connected: " + normalized->first});
        }
    }

    ResolvedRemoteMount resolved{
        .source       = *remote,
        .alias_name   = normalized->first,
        .alias_path   = normalized->second,
        .metrics_root = std::move(metrics_root),
    };

    return resolved;
}

template <class Space>
auto PathSpaceHtmlServer<Space>::normalize_remote_alias(RemoteMountSource const& source)
    -> SP::Expected<std::pair<std::string, std::string>> {
    if (source.alias.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                         "remote mount alias must not be empty"});
    }

    std::string alias = source.alias;
    while (!alias.empty() && alias.front() == '/') {
        alias.erase(alias.begin());
    }

    constexpr std::string_view kRemotePrefix{"remote/"};
    if (alias.starts_with(kRemotePrefix)) {
        alias.erase(0, static_cast<std::string::size_type>(kRemotePrefix.size()));
    }

    if (alias.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                         "remote mount alias must contain a name"});
    }

    if (alias.find('/') != std::string::npos) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                         "remote mount alias must not contain '/'"});
    }

    std::string mount_prefix = source.mount_prefix.empty() ? std::string{"/remote"} : source.mount_prefix;
    if (!mount_prefix.empty() && mount_prefix.front() != '/') {
        mount_prefix.insert(mount_prefix.begin(), '/');
    }
    while (mount_prefix.size() > 1 && mount_prefix.back() == '/') {
        mount_prefix.pop_back();
    }
    if (mount_prefix.empty()) {
        mount_prefix = "/remote";
    }

    std::string alias_path = mount_prefix;
    alias_path.push_back('/');
    alias_path.append(alias);

    return std::make_pair(alias, alias_path);
}

template <class Space>
auto PathSpaceHtmlServer<Space>::prefix_under_alias(std::string const& alias_path,
                                                    std::string const& value)
    -> SP::Expected<std::string> {
    if (alias_path.empty()) {
        return std::unexpected(
            SP::Error{SP::Error::Code::InvalidError, "remote alias path missing while prefixing roots"});
    }

    if (value.empty()) {
        return alias_path;
    }

    if (value.starts_with(alias_path)) {
        return value;
    }

    if (value.starts_with("/remote/")) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                         "ServeHtml roots point to a different remote alias"});
    }

    std::string normalized = alias_path;
    if (value.front() == '/') {
        normalized.append(value);
    } else {
        normalized.push_back('/');
        normalized.append(value);
    }
    return normalized;
}

template <class Space>
auto PathSpaceHtmlServer<Space>::prefix_remote_path(std::string const& path,
                                                    std::optional<ResolvedRemoteMount> const& mount)
    -> SP::Expected<std::string> {
    if (!mount) {
        return path;
    }
    return prefix_under_alias(mount->alias_path, path);
}

template <class Space>
auto PathSpaceHtmlServer<Space>::attach_default_html_targets(PathSpaceHtmlServerOptions& options,
                                                             std::optional<ResolvedRemoteMount> const& remote)
    -> SP::Expected<void> {
    if (!options.attach_default_targets && !options.html_mirror) {
        return {};
    }

    if (!options.html_mirror) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidError,
                                         "attach_default_targets requires html_mirror configuration"});
    }

    auto            mirror_bootstrap = *options.html_mirror;
    HtmlMirrorConfig mirror_config   = mirror_bootstrap.mirror_config;

    if (mirror_config.renderer_name.empty()) {
        mirror_config.renderer_name = "html";
    }
    if (mirror_config.target_name.empty()) {
        mirror_config.target_name = "web";
    }
    if (mirror_config.view_name.empty()) {
        mirror_config.view_name = "web";
    }

    options.serve_html.renderer    = mirror_config.renderer_name;
    options.attach_default_targets = true;
    options.html_mirror            = mirror_bootstrap;

    auto prefix_value = [&](std::string const& value) -> SP::Expected<std::string> {
        if (!remote) {
            return value;
        }
        return prefix_under_alias(remote->alias_path, value);
    };

    auto app_root_path = prefix_value(std::string{mirror_bootstrap.app_root.getPath()});
    if (!app_root_path) {
        return std::unexpected(app_root_path.error());
    }

    auto window_path = prefix_value(std::string{mirror_bootstrap.window.getPath()});
    if (!window_path) {
        return std::unexpected(window_path.error());
    }

    auto scene_path = prefix_value(std::string{mirror_bootstrap.scene.getPath()});
    if (!scene_path) {
        return std::unexpected(scene_path.error());
    }

    SP::App::AppRootPath        app_root{*app_root_path};
    SP::UI::Runtime::WindowPath window{*window_path};
    SP::UI::Runtime::ScenePath  scene{*scene_path};

    auto mirror_context = SetupHtmlMirror(*space_, app_root, window, scene, mirror_config);
    if (!mirror_context) {
        return std::unexpected(mirror_context.error());
    }

    if (mirror_bootstrap.present_on_start) {
        auto present = PresentHtmlMirror(*space_, *mirror_context);
        if (!present) {
            return std::unexpected(present.error());
        }
    }

    html_mirror_context_ = *mirror_context;
    return {};
}

template <class Space>
auto PathSpaceHtmlServer<Space>::apply_remote_mount(PathSpaceHtmlServerOptions& options,
                                                    std::optional<ResolvedRemoteMount> const& remote) const
    -> SP::Expected<void> {
    auto resolved = remote;
    if (!resolved) {
        auto computed = resolve_remote_mount(options);
        if (!computed) {
            return std::unexpected(computed.error());
        }
        resolved = computed.value();
    }

    if (!resolved) {
        return {};
    }

    auto apps_root = prefix_under_alias(resolved->alias_path, options.serve_html.apps_root);
    if (!apps_root) {
        return std::unexpected(apps_root.error());
    }
    options.serve_html.apps_root = *apps_root;

    auto users_root = prefix_under_alias(resolved->alias_path, options.serve_html.users_root);
    if (!users_root) {
        return std::unexpected(users_root.error());
    }
    options.serve_html.users_root = *users_root;

    auto session_store_root = prefix_under_alias(resolved->alias_path, options.serve_html.session_store_path);
    if (!session_store_root) {
        return std::unexpected(session_store_root.error());
    }
    options.serve_html.session_store_path = *session_store_root;

    if (!options.serve_html.google_users_root.empty()) {
        auto google_users_root = prefix_under_alias(resolved->alias_path, options.serve_html.google_users_root);
        if (!google_users_root) {
            return std::unexpected(google_users_root.error());
        }
        options.serve_html.google_users_root = *google_users_root;
    }

    if (!options.remote_mount) {
        options.remote_mount = resolved->source;
    }

    return {};
}

} // namespace SP::ServeHtml
