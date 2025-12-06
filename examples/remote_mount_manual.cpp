#include <pathspace/PathSpace.hpp>
#include <pathspace/distributed/RemoteMountLoopback.hpp>
#include <pathspace/distributed/RemoteMountManager.hpp>
#include <pathspace/distributed/RemoteMountServer.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using SP::Block;
using SP::Out;
using SP::PathSpace;
using SP::Distributed::AuthContext;
using SP::Distributed::AuthKind;
using SP::Distributed::Loopback::makeFactory;
using SP::Distributed::RemoteMountClientOptions;
using SP::Distributed::RemoteMountExportOptions;
using SP::Distributed::RemoteMountManager;
using SP::Distributed::RemoteMountManagerOptions;
using SP::Distributed::RemoteMountServer;
using SP::Distributed::RemoteMountServerOptions;
using SP::Examples::CLI::ExampleCli;

namespace {

struct HarnessOptions {
    std::string alias{"alpha"};
    std::string export_root{"/apps/demo"};
    std::string read_relative{"state"};
    std::string wait_relative{"events"};
    std::string initial_value{"demo-ready"};
    std::string wait_value{"event-received"};
    std::string client_id{"remote-mount-manual"};
    std::string audience{"pathspace"};
    std::string subject{"CN=manual-client"};
    std::string fingerprint{"sha256:manual-client"};
    std::string proof{"sha256:manual-proof"};
    int         insert_delay_ms{200};
    int         wait_timeout_ms{1500};
    bool        verbose{true};
};

void print_usage() {
    std::cout << "Usage: remote_mount_manual [options]\n\n"
              << "Exercises RemoteMountServer/Manager locally via a loopback session.\n"
              << "Options:\n"
              << "  --alias=<name>           Alias exposed under /remote/<alias> (default alpha)\n"
              << "  --export-root=<path>     Remote PathSpace root to export (default /apps/demo)\n"
              << "  --read=<path>            Relative path (under export root) to read (default state)\n"
              << "  --wait=<path>            Relative path to wait on (default events)\n"
              << "  --initial=<value>        Initial value seeded at the read path\n"
              << "  --wait-value=<value>     Value inserted after delay at the wait path\n"
              << "  --delay-ms=<int>         Delay before remote insert, in milliseconds\n"
              << "  --timeout-ms=<int>       Wait timeout, in milliseconds\n"
              << "  --quiet                  Reduce informational logging\n";
}

auto join_paths(std::string root, std::string const& tail) -> std::string {
    std::string base{std::move(root)};
    if (base.empty()) {
        base = "/";
    }
    if (!tail.empty()) {
        if (base.back() != '/') {
            base.push_back('/');
        }
        if (!tail.empty() && tail.front() == '/') {
            base.append(tail.substr(1));
        } else {
            base.append(tail);
        }
    }
    return base;
}

auto make_auth(HarnessOptions const& options) -> AuthContext {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    AuthContext auth;
    auth.kind          = AuthKind::MutualTls;
    auth.subject       = options.subject;
    auth.audience      = options.audience;
    auth.proof         = options.proof;
    auth.fingerprint   = options.fingerprint;
    auth.issued_at_ms  = static_cast<std::uint64_t>(now.count());
    auth.expires_at_ms = auth.issued_at_ms + 3'600'000;
    return auth;
}

void log(std::string_view message, HarnessOptions const& options) {
    if (options.verbose) {
        std::cout << message << '\n';
    }
}

auto read_metric(PathSpace& space, std::string const& path) -> std::optional<int> {
    auto value = space.read<int>(path);
    if (value.has_value()) {
        return value.value();
    }
    return std::nullopt;
}

auto run_harness(HarnessOptions const& options) -> int {
    PathSpace remote;
    PathSpace local;
    PathSpace client_metrics;
    PathSpace server_metrics;
    PathSpace diagnostics;

    auto remote_state_path = join_paths(options.export_root, options.read_relative);
    remote.insert(remote_state_path, options.initial_value);

    RemoteMountExportOptions export_options;
    export_options.alias       = options.alias;
    export_options.export_root = options.export_root;
    export_options.space       = &remote;

    RemoteMountServerOptions server_options;
    server_options.exports.push_back(export_options);
    server_options.metrics_space     = &server_metrics;
    server_options.diagnostics_space = &diagnostics;

    auto server  = std::make_shared<RemoteMountServer>(server_options);
    auto factory = makeFactory(server);

    RemoteMountClientOptions client;
    client.alias       = options.alias;
    client.export_root = options.export_root;
    client.client_id   = options.client_id;
    client.capabilities.push_back({.name = "read"});
    client.capabilities.push_back({.name = "wait"});
    client.auth = make_auth(options);

    RemoteMountManagerOptions manager_options;
    manager_options.root_space    = &local;
    manager_options.metrics_space = &client_metrics;
    manager_options.mounts.push_back(client);

    RemoteMountManager manager(manager_options, factory);
    manager.start();

    std::string local_root = std::string{"/remote/"} + options.alias;
    auto local_state  = join_paths(local_root, options.read_relative);
    auto local_events = join_paths(local_root, options.wait_relative);

    auto read_string = [&](std::string const& path, Out const& opts) {
        return static_cast<SP::PathSpaceBase const&>(local)
            .template read<std::string, std::string>(path, opts);
    };

    log("[1/3] Performing initial read...", options);
    auto state = read_string(local_state, Out{});
    if (!state.has_value()) {
        std::cerr << "Failed to read initial state at " << local_state << '\n';
        manager.stop();
        return 1;
    }
    std::cout << "Remote value at " << local_state << ": " << state.value() << '\n';

    log("[2/3] Waiting for remote insert...", options);
    std::thread inserter([&remote, remote_events = join_paths(options.export_root, options.wait_relative),
                         options] {
        std::this_thread::sleep_for(std::chrono::milliseconds{options.insert_delay_ms});
        remote.insert(remote_events, options.wait_value);
    });

    auto waited = read_string(
        local_events, Out{} & Block{std::chrono::milliseconds{options.wait_timeout_ms}});
    inserter.join();
    if (!waited.has_value()) {
        std::cerr << "Timed out waiting for remote event at " << local_events << '\n';
        manager.stop();
        return 1;
    }
    std::cout << "Wait completed with value: " << waited.value() << '\n';

    auto client_metric = join_paths(std::string{"/inspector/metrics/remotes/"} + options.alias,
                                    "client/connected");
    if (auto connected = read_metric(client_metrics, client_metric); connected.has_value()) {
        std::cout << "client/connected metric: " << connected.value() << '\n';
    }

    auto server_metric = join_paths(std::string{"/inspector/metrics/remotes/"} + options.alias,
                                    "server/sessions");
    if (auto sessions = read_metric(server_metrics, server_metric); sessions.has_value()) {
        std::cout << "server/sessions metric: " << sessions.value() << '\n';
    }

    for (auto const& status : manager.statuses()) {
        std::cout << "status alias=" << status.alias << " connected=" << status.connected
                  << " message=" << status.message << '\n';
    }

    manager.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    HarnessOptions options;
    bool           show_help = false;

    ExampleCli cli;
    cli.set_program_name("remote_mount_manual");
    cli.set_error_logger([](std::string const& message) { std::cerr << message << '\n'; });

    auto add_value_option = [&](std::string_view name,
                                std::function<void(std::string)> handler) {
        ExampleCli::ValueOption option{};
        option.on_value = [handler](std::optional<std::string_view> value)
            -> ExampleCli::ParseError {
            if (!value.has_value()) {
                return std::string{"Missing value for option"};
            }
            handler(std::string{*value});
            return std::nullopt;
        };
        cli.add_value(name, option);
    };

    add_value_option("--alias", [&](std::string value) { options.alias = std::move(value); });
    add_value_option("--export-root",
                     [&](std::string value) { options.export_root = std::move(value); });
    add_value_option("--read",
                     [&](std::string value) { options.read_relative = std::move(value); });
    add_value_option("--wait",
                     [&](std::string value) { options.wait_relative = std::move(value); });
    add_value_option("--initial",
                     [&](std::string value) { options.initial_value = std::move(value); });
    add_value_option("--wait-value",
                     [&](std::string value) { options.wait_value = std::move(value); });
    add_value_option("--client-id",
                     [&](std::string value) { options.client_id = std::move(value); });
    add_value_option("--audience",
                     [&](std::string value) { options.audience = std::move(value); });
    add_value_option("--subject",
                     [&](std::string value) { options.subject = std::move(value); });
    add_value_option("--fingerprint",
                     [&](std::string value) { options.fingerprint = std::move(value); });
    add_value_option("--proof",
                     [&](std::string value) { options.proof = std::move(value); });

    ExampleCli::IntOption delay_option{};
    delay_option.on_value = [&](int value) { options.insert_delay_ms = value; };
    cli.add_int("--delay-ms", delay_option);

    ExampleCli::IntOption timeout_option{};
    timeout_option.on_value = [&](int value) { options.wait_timeout_ms = value; };
    cli.add_int("--timeout-ms", timeout_option);

    ExampleCli::FlagOption quiet_flag{};
    quiet_flag.on_set = [&] { options.verbose = false; };
    cli.add_flag("--quiet", quiet_flag);

    ExampleCli::FlagOption help_flag{};
    help_flag.on_set = [&] { show_help = true; };
    cli.add_flag("--help", help_flag);
    cli.add_alias("-h", "--help");

    cli.set_unknown_argument_handler([&](std::string_view) {
        print_usage();
        return false;
    });

    if (!cli.parse(argc, argv)) {
        print_usage();
        return 1;
    }

    if (show_help) {
        print_usage();
        return 0;
    }

    return run_harness(options);
}
