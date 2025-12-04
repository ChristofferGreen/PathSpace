#pragma once

#include <memory>

#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>

namespace httplib {
class Server;
}

namespace SP::ServeHtml {

class AuthController {
public:
    static auto Create(HttpRequestContext& ctx) -> std::unique_ptr<AuthController>;

    void register_routes(httplib::Server& server);

    ~AuthController();

private:
    explicit AuthController(HttpRequestContext& ctx);
    auto initialize() -> bool;
    void register_local_routes(httplib::Server& server);
    void register_google_routes(httplib::Server& server);

    HttpRequestContext& ctx_;

    struct GoogleConfig;
    std::unique_ptr<GoogleConfig> google_;
};

} // namespace SP::ServeHtml
