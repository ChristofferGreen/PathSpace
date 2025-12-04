#include <pathspace/web/serve_html/routing/HtmlController.hpp>

#include <pathspace/web/ServeHtmlIdentifier.hpp>
#include <pathspace/web/ServeHtmlServer.hpp>
#include <pathspace/web/serve_html/AssetPath.hpp>
#include <pathspace/web/serve_html/HtmlPayload.hpp>
#include <pathspace/web/serve_html/Metrics.hpp>
#include <pathspace/web/serve_html/auth/SessionStore.hpp>
#include <pathspace/web/serve_html/PathSpaceUtils.hpp>
#include <pathspace/web/serve_html/Routes.hpp>
#include <pathspace/web/serve_html/routing/HttpHelpers.hpp>

#include "core/Error.hpp"

#include "httplib.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::ServeHtml {

namespace {

using json = nlohmann::json;

struct AssetLocator {
    std::string                           view;
    std::optional<std::uint64_t>          revision;
    std::chrono::steady_clock::time_point updated_at{};
};

std::mutex                                   g_asset_index_mutex;
std::unordered_map<std::string, AssetLocator> g_asset_index;

std::string MakeAssetIndexKey(std::string_view app, std::string_view asset_path) {
    std::string key;
    key.reserve(app.size() + asset_path.size() + 1);
    key.append(app);
    key.push_back('\x1f');
    key.append(asset_path);
    return key;
}

std::optional<AssetLocator> LookupAssetLocator(std::string const& app,
                                               std::string const& asset_path) {
    std::lock_guard const lock{g_asset_index_mutex};
    auto const            key = MakeAssetIndexKey(app, asset_path);
    auto                  it  = g_asset_index.find(key);
    if (it == g_asset_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RecordAssetManifest(std::string const& app,
                         std::string const& view,
                         HtmlPayload const& payload) {
    if (payload.asset_manifest.empty()) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    std::lock_guard const lock{g_asset_index_mutex};
    for (auto const& logical : payload.asset_manifest) {
        if (!IsAssetPath(logical)) {
            continue;
        }
        AssetLocator locator{};
        locator.view       = view;
        locator.revision   = payload.revision;
        locator.updated_at = now;
        g_asset_index[MakeAssetIndexKey(app, logical)] = std::move(locator);
    }
}

auto LoadHtmlPayload(ServeHtmlSpace const& space, std::string const& html_base)
    -> SP::Expected<HtmlPayload> {
    HtmlPayload payload{};

    auto dom_value = space.read<std::string, std::string>(html_base + "/dom");
    if (!dom_value) {
        return std::unexpected(dom_value.error());
    }
    payload.dom = *dom_value;

    auto css_value = read_optional_value<std::string>(space, html_base + "/css");
    if (!css_value) {
        return std::unexpected(css_value.error());
    }
    payload.css = std::move(*css_value);

    auto js_value = read_optional_value<std::string>(space, html_base + "/js");
    if (!js_value) {
        return std::unexpected(js_value.error());
    }
    payload.js = std::move(*js_value);

    auto commands_value = read_optional_value<std::string>(space, html_base + "/commands");
    if (!commands_value) {
        return std::unexpected(commands_value.error());
    }
    payload.commands = std::move(*commands_value);

    auto revision_value = read_optional_value<std::uint64_t>(space, html_base + "/revision");
    if (!revision_value) {
        return std::unexpected(revision_value.error());
    }
    payload.revision = std::move(*revision_value);

    auto manifest_value = read_optional_value<std::vector<std::string>>(space,
                                                                        html_base + "/assets/manifest");
    if (!manifest_value) {
        return std::unexpected(manifest_value.error());
    }
    if (manifest_value->has_value()) {
        payload.asset_manifest = std::move(**manifest_value);
    }

    return payload;
}

bool WantsJsonResponse(httplib::Request const& req) {
    if (req.has_param("format")) {
        return req.get_param_value("format") == "json";
    }
    auto accept = req.get_header_value("Accept");
    return !accept.empty() && accept.find("application/json") != std::string::npos;
}

std::string BuildLiveUpdateScript(std::string const& app, std::string const& view) {
    auto base_route = make_app_route(app, view);
    std::string script;
    script.reserve(2400);
    script.append("<script id=\"pathspace-html-live\">(function(){\n");
    script.append("if(window.__pathspaceHtmlLive){return;}window.__pathspaceHtmlLive=true;\n");
    script.append("var baseRoute=").append(json(base_route).dump()).append(";\n");
    script.append("var eventsUrl=baseRoute+'/events';\n");
    script.append("var payloadUrl=baseRoute+'?format=json';\n");
    script.append("var scriptEl=document.currentScript||document.getElementById('pathspace-html-live');\n");
    script.append(
        "function ensureRoot(){var root=document.getElementById('pathspace-html-live-root');"
        "if(root){return root;}root=document.createElement('div');root.id='pathspace-html-live-root';"
        "while(document.body.firstChild){var child=document.body.firstChild;"
        "if(child===scriptEl){document.body.removeChild(child);continue;}root.appendChild(child);}"
        "document.body.appendChild(root);document.body.appendChild(scriptEl);return root;}\n");
    script.append(
        "function ensureStyle(){var style=document.getElementById('pathspace-html-live-style');"
        "if(!style){style=document.createElement('style');style.id='pathspace-html-live-style';"
        "document.head.appendChild(style);}return style;}\n");
    script.append(
        "function ensureBanner(){var banner=document.getElementById('pathspace-html-live-status');"
        "if(!banner){banner=document.createElement('div');banner.id='pathspace-html-live-status';"
        "banner.style.position='fixed';banner.style.bottom='16px';banner.style.right='16px';"
        "banner.style.padding='12px 16px';banner.style.background='rgba(8,24,48,0.85)';"
        "banner.style.color='#fff';banner.style.fontFamily='system-ui,sans-serif';"
        "banner.style.fontSize='14px';banner.style.borderRadius='999px';"
        "banner.style.zIndex='2147483647';banner.style.boxShadow='0 8px 24px rgba(0,0,0,0.25)';"
        "banner.style.display='none';document.body.appendChild(banner);}return banner;}\n");
    script.append(
        "function updateCommands(value){var cmds=document.getElementById('pathspace-commands');"
        "if(!cmds){cmds=document.createElement('script');cmds.type='application/json';"
        "cmds.id='pathspace-commands';document.body.appendChild(cmds);}cmds.textContent=value||'';}\n");
    script.append(
        "function executeJs(source){if(!source){return;}try{var exec=document.createElement('script');"
        "exec.type='text/javascript';exec.setAttribute('data-pathspace-html-live','1');"
        "exec.text=source;document.body.appendChild(exec);document.body.removeChild(exec);}catch(err){console.warn(err);}}\n");
    script.append("var liveRoot=ensureRoot();\n");
    script.append("var statusBanner=ensureBanner();statusBanner.style.display='none';\n");
    script.append("var lastRevision=0;\n");
    script.append(
        "function applyPayload(payload){if(!payload){return;}if(typeof payload.revision==='number'){lastRevision=payload.revision;}"
        "if(typeof payload.dom==='string'){liveRoot.innerHTML=payload.dom;}"
        "if('css' in payload){ensureStyle().textContent=payload.css||'';}"
        "if('commands' in payload){updateCommands(payload.commands||'');}"
        "if(payload.js){executeJs(payload.js);}}\n");
    script.append(
        "function fetchLatest(){return fetch(payloadUrl,{credentials:'include'})"
        ".then(function(resp){return resp.json();}).then(function(data){applyPayload(data);"
        "statusBanner.style.display='none';})"
        ".catch(function(err){console.warn('pathspace-html-live fetch failed',err);window.location.reload();});}\n");
    script.append(
        "function connect(){if(!window.EventSource){statusBanner.textContent='Live updates unavailable - refresh manually.';"
        "statusBanner.style.display='block';return;}var source=new EventSource(eventsUrl);"
        "source.addEventListener('frame',function(){fetchLatest();});"
        "source.addEventListener('reload',function(){window.location.reload();});"
        "source.addEventListener('diagnostic',function(evt){if(!evt||!evt.data){return;}statusBanner.textContent=evt.data;"
        "statusBanner.style.display='block';});"
        "source.onerror=function(){source.close();statusBanner.textContent='Live updates reconnecting...';"
        "statusBanner.style.display='block';setTimeout(connect,2000);};}\n");
    script.append("fetchLatest();\nconnect();\n");
    script.append("})();</script>\n");
    return script;
}

} // namespace

auto HtmlController::Create(HttpRequestContext& ctx) -> std::unique_ptr<HtmlController> {
    return std::unique_ptr<HtmlController>(new HtmlController(ctx));
}

HtmlController::HtmlController(HttpRequestContext& ctx)
    : ctx_(ctx) {}

HtmlController::~HtmlController() = default;

void HtmlController::register_routes(httplib::Server& server) {
    server.Get(R"(/apps/([A-Za-z0-9_\-\.]+)/([A-Za-z0-9_\-\.]+))",
               [this](httplib::Request const& req, httplib::Response& res) {
                   handle_apps_request(req, res);
               });

    server.Get(R"(/assets/([A-Za-z0-9_\-\.]+)/(.+))",
               [this](httplib::Request const& req, httplib::Response& res) {
                   handle_assets_request(req, res);
               });
}

void HtmlController::handle_apps_request(httplib::Request const& req, httplib::Response& res) {
    [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Apps, res};

    if (req.matches.size() < 3) {
        res.status = 400;
        res.set_content("invalid route", "text/plain; charset=utf-8");
        return;
    }

    std::string app  = req.matches[1];
    std::string view = req.matches[2];
    if (!is_identifier(app) || !is_identifier(view)) {
        res.status = 400;
        res.set_content("invalid app or view", "text/plain; charset=utf-8");
        return;
    }

    auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
    auto app_root       = make_app_root_path(ctx_.options, app);
    if (!apply_rate_limits(ctx_, "apps", req, res, session_cookie, &app_root)) {
        return;
    }
    if (!ensure_session(ctx_, req, res, session_cookie)) {
        return;
    }

    auto html_base = make_html_base(ctx_.options, app, view);
    auto payload   = LoadHtmlPayload(ctx_.space, html_base);
    if (!payload) {
        auto const& error = payload.error();
        if (error.code == SP::Error::Code::NoObjectFound || error.code == SP::Error::Code::NoSuchPath) {
            res.status = 404;
            res.set_content("no HTML output at " + html_base, "text/plain; charset=utf-8");
        } else {
            res.status = 500;
            res.set_content("failed to read HTML output: " + SP::describeError(error),
                            "text/plain; charset=utf-8");
        }
        return;
    }

    RecordAssetManifest(app, view, *payload);

    res.set_header("X-PathSpace-App", app);
    res.set_header("X-PathSpace-View", view);
    if (payload->revision) {
        res.set_header("ETag", "\"" + std::to_string(*payload->revision) + "\"");
    }

    if (WantsJsonResponse(req)) {
        json payload_json{{"dom", payload->dom},
                          {"css", payload->css.value_or(std::string{})},
                          {"js", payload->js.value_or(std::string{})},
                          {"commands", payload->commands.value_or(std::string{})},
                          {"revision", payload->revision.value_or(0)}};
        write_json_response(res, payload_json, 200, true);
        return;
    }

    auto body = BuildHtmlResponseBody(*payload, app, view);
    body.append(BuildLiveUpdateScript(app, view));
    res.set_content(body, "text/html; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}

void HtmlController::handle_assets_request(httplib::Request const& req, httplib::Response& res) {
    [[maybe_unused]] RequestMetricsScope request_scope{ctx_.metrics, RouteMetric::Assets, res};

    if (req.matches.size() < 3) {
        res.status = 400;
        res.set_content("invalid route", "text/plain; charset=utf-8");
        return;
    }

    std::string app       = req.matches[1];
    std::string asset_rel = req.matches[2];
    if (!is_identifier(app) || !IsAssetPath(asset_rel)) {
        res.status = 400;
        res.set_content("invalid app or asset path", "text/plain; charset=utf-8");
        return;
    }

    auto session_cookie = read_cookie_value(req, ctx_.session_store.cookie_name());
    auto app_root       = make_app_root_path(ctx_.options, app);
    if (!apply_rate_limits(ctx_, "assets", req, res, session_cookie, &app_root)) {
        return;
    }
    if (!ensure_session(ctx_, req, res, session_cookie)) {
        return;
    }

    auto locator = LookupAssetLocator(app, asset_rel);
    if (!locator) {
        res.status = 404;
        res.set_content("asset not indexed", "text/plain; charset=utf-8");
        return;
    }

    auto html_base = make_html_base(ctx_.options, app, locator->view);
    auto data_path = html_base + "/assets/data/" + asset_rel;
    auto bytes     = ctx_.space.read<std::vector<std::uint8_t>>(data_path);
    if (!bytes) {
        auto const error = bytes.error();
        if (error.code == SP::Error::Code::NoObjectFound || error.code == SP::Error::Code::NoSuchPath) {
            res.status = 404;
            res.set_content("asset not found", "text/plain; charset=utf-8");
        } else {
            res.status = 500;
            res.set_content("failed to read asset: " + SP::describeError(error),
                            "text/plain; charset=utf-8");
        }
        return;
    }

    auto mime_path = html_base + "/assets/meta/" + asset_rel;
    auto mime      = read_optional_value<std::string>(ctx_.space, mime_path);
    if (!mime) {
        res.status = 500;
        res.set_content("failed to read asset metadata: " + SP::describeError(mime.error()),
                        "text/plain; charset=utf-8");
        return;
    }
    auto content_type = mime->value_or("application/octet-stream");

    std::string etag;
    if (locator->revision) {
        etag = "\"r" + std::to_string(*locator->revision) + ":" + asset_rel + "\"";
    } else {
        etag = "\"asset:" + asset_rel + "\"";
    }

    auto if_none_match = req.get_header_value("If-None-Match");
    res.set_header("Cache-Control", "public, max-age=31536000, immutable");
    res.set_header("X-PathSpace-App", app);
    res.set_header("X-PathSpace-View", locator->view);
    res.set_header("X-PathSpace-Asset", asset_rel);
    if (!etag.empty()) {
        res.set_header("ETag", etag);
    }
    if (!etag.empty() && !if_none_match.empty() && if_none_match == etag) {
        res.status = 304;
        ctx_.metrics.record_asset_cache_hit();
        return;
    }

    std::string body(bytes->begin(), bytes->end());
    res.set_content(std::move(body), content_type.c_str());
    ctx_.metrics.record_asset_cache_miss();
}

} // namespace SP::ServeHtml
