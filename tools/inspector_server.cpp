#include "PathSpace.hpp"
#include "inspector/InspectorHttpServer.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <system_error>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_should_stop = false;

void handle_signal(int) {
    g_should_stop.store(true);
}

auto parse_unsigned(std::string_view value, std::size_t fallback) -> std::size_t {
    if (value.empty()) {
        return fallback;
    }
    std::size_t parsed = fallback;
    auto        result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return parsed;
}

auto parse_arguments(int argc, char** argv, bool& demo) -> SP::Inspector::InspectorHttpServer::Options {
    SP::Inspector::InspectorHttpServer::Options options;
    demo = true;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        auto next_value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "--host") {
            if (auto value = next_value()) {
                options.host = std::string{*value};
            }
        } else if (arg == "--port") {
            if (auto value = next_value()) {
                int parsed = options.port;
                auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
                if (result.ec == std::errc{}) {
                    options.port = parsed;
                }
            }
        } else if (arg == "--root") {
            if (auto value = next_value()) {
                options.snapshot.root = std::string{*value};
            }
        } else if (arg == "--max-depth") {
            if (auto value = next_value()) {
                options.snapshot.max_depth = parse_unsigned(*value, options.snapshot.max_depth);
            }
        } else if (arg == "--max-children") {
            if (auto value = next_value()) {
                options.snapshot.max_children = parse_unsigned(*value, options.snapshot.max_children);
            }
        } else if (arg == "--diagnostics-root") {
            if (auto value = next_value()) {
                options.paint_card.diagnostics_root = std::string{*value};
            }
        } else if (arg == "--no-demo") {
            demo = false;
        } else if (arg == "--ui-root") {
            if (auto value = next_value()) {
                options.ui_root = std::string{*value};
            }
        } else if (arg == "--no-ui") {
            options.enable_ui = false;
        }
    }
    return options;
}

void seed_demo_data(SP::PathSpace& space) {
    auto insert_string = [&](std::string const& path, std::string value) {
        auto result = space.insert(path, std::move(value));
        (void)result;
    };
    auto insert_uint = [&](std::string const& path, std::uint64_t value) {
        auto result = space.insert(path, value);
        (void)result;
    };
    auto insert_bool = [&](std::string const& path, bool value) {
        auto result = space.insert(path, value);
        (void)result;
    };

    insert_string("/demo/widgets/button/meta/label", "Declarative Button");
    insert_bool("/demo/widgets/button/state/enabled", true);
    insert_string("/demo/widgets/button/log/lastPress", "n/a");
    insert_uint("/demo/widgets/slider/state/value", 42);
    insert_uint("/demo/widgets/slider/state/range/min", 0);
    insert_uint("/demo/widgets/slider/state/range/max", 100);
    insert_string("/demo/widgets/list/items/alpha/meta/label", "Alpha");
    insert_string("/demo/widgets/list/items/beta/meta/label", "Beta");
    insert_string("/demo/widgets/list/items/gamma/meta/label", "Gamma");
    insert_bool("/demo/widgets/list/items/beta/state/selected", true);
    insert_uint("/demo/metrics/widgets_total", 5);
}

} // namespace

int main(int argc, char** argv) {
    bool demo = true;
    auto options = parse_arguments(argc, argv, demo);

    SP::PathSpace space;
    if (demo) {
        seed_demo_data(space);
    }

    SP::Inspector::InspectorHttpServer server(space, options);
    auto                                started = server.start();
    if (!started) {
        std::cerr << "Failed to start inspector server: "
                  << SP::describeError(started.error()) << "\n";
        return 1;
    }

    std::cout << "Inspector server listening on " << options.host << ":" << server.port()
              << "\nPress Ctrl+C to stop.\n";

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    while (!g_should_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    server.join();
    return 0;
}
