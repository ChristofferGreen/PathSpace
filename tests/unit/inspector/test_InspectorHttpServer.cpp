#include "PathSpace.hpp"
#include "inspector/InspectorHttpServer.hpp"

#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_NO_EXCEPTIONS
#endif
#include "httplib.h"

#include "nlohmann/json.hpp"
#include "third_party/doctest.h"

#include <thread>

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
