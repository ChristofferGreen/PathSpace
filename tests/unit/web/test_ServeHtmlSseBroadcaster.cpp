#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#include "third_party/doctest.h"

#include "pathspace/web/ServeHtmlServer.hpp"
#include "pathspace/web/serve_html/Metrics.hpp"
#include "pathspace/web/serve_html/PathSpaceUtils.hpp"
#include "pathspace/web/serve_html/streaming/SseBroadcaster.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>

namespace {

struct CollectingSink {
    httplib::DataSink sink;
    std::string       buffer;

    CollectingSink() {
        sink.write = [this](char const* data, size_t len) {
            buffer.append(data, len);
            return true;
        };
        sink.done = []() {};
    }
};

auto make_base_paths() {
    std::string base = "/apps/demo/renderers/default/targets/html/main";
    return std::tuple{
        base + "/output/v1/html",
        base + "/output/v1/common",
        base + "/diagnostics/errors/live",
        base + "/**"};
}

} // namespace

TEST_SUITE("web.servehtml.sse.broadcaster") {
TEST_CASE("HtmlEventStreamSessionSseBroadcaster") {
    auto [html_base, common_base, diag_path, watch_glob] = make_base_paths();

    SUBCASE("emits initial frame and diagnostic snapshot") {
        SP::ServeHtml::ServeHtmlSpace space;
        auto write_value = [&](std::string const& path, std::uint64_t value) {
            auto status = SP::ServeHtml::replace_single_value(space, path, value);
            REQUIRE(status);
        };
        write_value(common_base + "/frameIndex", std::uint64_t{7});
        write_value(html_base + "/revision", std::uint64_t{3});

        SP::ServeHtml::MetricsCollector metrics;
        std::atomic<bool>              stop_flag{false};

        SP::ServeHtml::HtmlEventStreamSession session(space,
                                                      html_base,
                                                      common_base,
                                                      diag_path,
                                                      watch_glob,
                                                      0,
                                                      &metrics,
                                                      stop_flag);

        CollectingSink sink;
        auto           keep_running = session.pump(sink.sink);
        CHECK(keep_running);
        CHECK(sink.buffer.find("retry: 2000") != std::string::npos);
        CHECK(sink.buffer.find("event: frame") != std::string::npos);
        CHECK(sink.buffer.find("event: diagnostic") != std::string::npos);
    }

    SUBCASE("emits reload events on revision gaps") {
        SP::ServeHtml::ServeHtmlSpace space;
        auto write_value = [&](std::string const& path, std::uint64_t value) {
            auto status = SP::ServeHtml::replace_single_value(space, path, value);
            REQUIRE(status);
        };
        write_value(common_base + "/frameIndex", std::uint64_t{1});
        write_value(html_base + "/revision", std::uint64_t{1});

        SP::ServeHtml::MetricsCollector metrics;
        std::atomic<bool>              stop_flag{false};
        SP::ServeHtml::HtmlEventStreamSession session(space,
                                                      html_base,
                                                      common_base,
                                                      diag_path,
                                                      watch_glob,
                                                      0,
                                                      &metrics,
                                                      stop_flag);

        CollectingSink first_chunk;
        CHECK(session.pump(first_chunk.sink));

        CollectingSink second_chunk;
        bool           pump_result = false;
        std::thread    stream_thread([&]() {
            pump_result = session.pump(second_chunk.sink);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        write_value(common_base + "/frameIndex", std::uint64_t{9});
        write_value(html_base + "/revision", std::uint64_t{4});

        stream_thread.join();

        CHECK(pump_result);
        CHECK(second_chunk.buffer.find("event: reload") != std::string::npos);
        CHECK(second_chunk.buffer.find("event: frame") != std::string::npos);
    }
}
}
