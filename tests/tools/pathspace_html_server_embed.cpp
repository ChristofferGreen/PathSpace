#include <csignal>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <pathspace/core/Error.hpp>
#include <pathspace/web/PathSpaceHtmlServer.hpp>
#include <pathspace/web/ServeHtmlOptions.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>

namespace {

std::atomic<bool> g_should_stop{false};

void handle_signal(int) {
    g_should_stop.store(true, std::memory_order_release);
    SP::ServeHtml::RequestServeHtmlStop();
}

} // namespace

int main(int argc, char** argv) {
    auto options_opt = SP::ServeHtml::ParseServeHtmlArguments(argc, argv);
    if (!options_opt) {
        return EXIT_FAILURE;
    }

    auto options = *options_opt;
    if (options.show_help) {
        SP::ServeHtml::PrintServeHtmlUsage();
        return EXIT_SUCCESS;
    }

    SP::ServeHtml::ServeHtmlSpace space{};

    SP::ServeHtml::PathSpaceHtmlServerOptions server_options{};
    server_options.serve_html           = options;
    server_options.seed_demo_credentials = options.seed_demo;

    SP::ServeHtml::PathSpaceHtmlServer server{space, server_options};

    auto start_status = server.start();
    if (!start_status) {
        std::cerr << "[PathSpaceHtmlServerEmbed] Failed to start: "
                  << SP::describeError(start_status.error()) << "\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    while (!g_should_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    server.stop();
    return EXIT_SUCCESS;
}
