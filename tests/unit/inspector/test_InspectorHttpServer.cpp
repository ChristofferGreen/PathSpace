#include "PathSpace.hpp"
#include "inspector/InspectorHttpServer.hpp"

#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#include "httplib.h"

#include "nlohmann/json.hpp"
#include "third_party/doctest.h"

#include <algorithm>
#include <thread>
#include <vector>

TEST_CASE("Inspector HTTP server serves snapshot JSON") {
    SP::PathSpace space;
    space.insert("/http/node/value", std::string{"demo"});

    SP::Inspector::InspectorHttpServer::Options options;
    options.host             = "127.0.0.1";
    options.port             = 0; // ephemeral
    options.snapshot.root    = "/http";
    options.snapshot.max_depth = 1;

    SP::Inspector::InspectorHttpServer server(space, options);
    auto                               started = server.start();
    REQUIRE(started);

    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(1, 0);

    httplib::Result response;
    for (int attempt = 0; attempt < 5 && !response; ++attempt) {
        response = client.Get("/inspector/tree?root=%2Fhttp");
        if (!response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    REQUIRE(response);
    CHECK(response->status == 200);

    auto json = nlohmann::json::parse(response->body);
    CHECK(json["root"]["path"] == "/http");

    server.stop();
    server.join();
}

TEST_CASE("Inspector HTTP server serves embedded UI") {
    SP::PathSpace space;

    SP::Inspector::InspectorHttpServer::Options options;
    options.host          = "127.0.0.1";
    options.port          = 0;
    options.enable_ui     = true;

    SP::Inspector::InspectorHttpServer server(space, options);
    auto                               started = server.start();
    REQUIRE(started);

    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(1, 0);

    httplib::Result response;
    for (int attempt = 0; attempt < 5 && !response; ++attempt) {
        response = client.Get("/");
        if (!response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->body.find("PathSpace Inspector") != std::string::npos);

    server.stop();
    server.join();
}

TEST_CASE("Inspector HTTP server exposes mailbox metrics") {
    SP::PathSpace space;

    auto button_root = std::string{"/system/applications/demo/windows/main/views/primary/widgets/button"};
    (void)space.insert(button_root + "/capsule/mailbox/metrics/events_total", std::uint64_t{3});
    (void)space.insert(button_root + "/capsule/mailbox/metrics/dispatch_failures_total", std::uint64_t{1});
    (void)space.insert(button_root + "/capsule/mailbox/metrics/last_event/kind", std::string{"press"});
    (void)space.insert(button_root + "/capsule/mailbox/metrics/last_event/ns", std::uint64_t{15});
    (void)space.insert(button_root + "/capsule/mailbox/subscriptions",
                       std::vector<std::string>{"press", "release"});
    (void)space.insert(button_root + "/capsule/mailbox/events/press/total", std::uint64_t{2});
    (void)space.insert(button_root + "/meta/kind", std::string{"button"});

    auto toggle_root = std::string{"/system/applications/demo/windows/main/views/primary/widgets/toggle"};
    (void)space.insert(toggle_root + "/capsule/mailbox/metrics/events_total", std::uint64_t{2});
    (void)space.insert(toggle_root + "/capsule/mailbox/metrics/dispatch_failures_total", std::uint64_t{0});
    (void)space.insert(toggle_root + "/capsule/mailbox/metrics/last_event/kind", std::string{"toggle"});
    (void)space.insert(toggle_root + "/capsule/mailbox/metrics/last_event/ns", std::uint64_t{25});
    (void)space.insert(toggle_root + "/capsule/mailbox/subscriptions",
                       std::vector<std::string>{"toggle"});
    (void)space.insert(toggle_root + "/capsule/mailbox/events/toggle/total", std::uint64_t{2});
    (void)space.insert(toggle_root + "/meta/kind", std::string{"toggle"});

    SP::Inspector::InspectorHttpServer::Options options;
    options.host = "127.0.0.1";
    options.port = 0;

    SP::Inspector::InspectorHttpServer server(space, options);
    auto                               started = server.start();
    REQUIRE(started);

    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(1, 0);

    httplib::Result response;
    for (int attempt = 0; attempt < 5 && !response; ++attempt) {
        response = client.Get("/inspector/metrics/mailbox");
        if (!response) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    REQUIRE(response);
    CHECK(response->status == 200);

    auto json = nlohmann::json::parse(response->body);
    CHECK(json["summary"]["widgets_with_mailbox"].get<std::uint64_t>() == 2);
    CHECK(json["summary"]["total_events"].get<std::uint64_t>() == 5);
    CHECK(json["summary"]["last_event_kind"].get<std::string>() == "toggle");

    auto widgets = json["widgets"].get<std::vector<nlohmann::json>>();
    CHECK(widgets.size() == 2);

    auto find_widget = [&](std::string const& path) {
        return std::find_if(widgets.begin(), widgets.end(), [&](nlohmann::json const& entry) {
            return entry.value("path", std::string{}) == path;
        });
    };

    auto button_entry = find_widget(button_root);
    REQUIRE(button_entry != widgets.end());
    CHECK(button_entry->value("events_total", 0) == 3);
    CHECK(button_entry->value("dispatch_failures_total", 0) == 1);

    auto toggle_entry = find_widget(toggle_root);
    REQUIRE(toggle_entry != widgets.end());
    CHECK(toggle_entry->value("last_event_kind", std::string{}) == "toggle");

    server.stop();
    server.join();
}
