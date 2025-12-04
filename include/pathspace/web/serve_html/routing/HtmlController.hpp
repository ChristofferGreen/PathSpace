#pragma once

#include <memory>

namespace httplib {
class Server;
class Request;
class Response;
} // namespace httplib

namespace SP::ServeHtml {

struct HtmlPayload;
struct HttpRequestContext;

class HtmlController {
public:
    static auto Create(HttpRequestContext& ctx) -> std::unique_ptr<HtmlController>;

    void register_routes(httplib::Server& server);

    ~HtmlController();

private:
    explicit HtmlController(HttpRequestContext& ctx);

    HttpRequestContext& ctx_;

    void handle_apps_request(httplib::Request const& req, httplib::Response& res);
    void handle_assets_request(httplib::Request const& req, httplib::Response& res);
};

} // namespace SP::ServeHtml
