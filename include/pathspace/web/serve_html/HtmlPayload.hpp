#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP::ServeHtml {

struct HtmlPayload {
    std::string                  dom;
    std::optional<std::string>   css;
    std::optional<std::string>   js;
    std::optional<std::string>   commands;
    std::optional<std::uint64_t> revision;
    std::vector<std::string>     asset_manifest;
};

[[nodiscard]] std::string BuildHtmlResponseBody(HtmlPayload const& payload,
                                                std::string_view   app,
                                                std::string_view   view);

} // namespace SP::ServeHtml

