#include "PathSpace.hpp"
#include "inspector/InspectorHttpServer.hpp"
#include "InspectorDemoData.hpp"

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
        } else if (arg == "--enable-test-controls") {
            options.enable_test_controls = true;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    bool demo = true;
    auto options = parse_arguments(argc, argv, demo);

    SP::PathSpace space;
    if (demo) {
        SP::Inspector::SeedInspectorDemoData(space);
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
