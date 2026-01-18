#include "third_party/doctest.h"

#include "pathspace/web/serve_html/AssetPath.hpp"
#include "pathspace/web/serve_html/HtmlPayload.hpp"

#include <string>

TEST_SUITE("web.html.helpers") {
TEST_CASE("BuildHtmlResponseBody escapes embedded script payloads") {
    SP::ServeHtml::HtmlPayload payload{};
    payload.dom = "<div id=\"root\"></div>";
    payload.css = std::string{"body{color:red;}"};
    payload.js = std::string{"console.log('</script> marker');"};
    payload.commands = std::string{"{\"ops\":[\"</script>\"]}"};

    auto html = SP::ServeHtml::BuildHtmlResponseBody(payload, "demo_app", "main_view");

    CHECK(html.find("<title>demo_app — main_view</title>") != std::string::npos);
    CHECK(html.find("<div id=\"root\"></div>") != std::string::npos);
    CHECK(html.find("</script> marker") == std::string::npos);
    CHECK(html.find("<\\/script> marker") != std::string::npos);
    CHECK(html.find("id=\"pathspace-commands\">") != std::string::npos);

    auto first_escape = html.find("<\\/script>");
    CHECK(first_escape != std::string::npos);
    auto second_escape = html.find("<\\/script>", first_escape + 1);
    CHECK(second_escape != std::string::npos);
}

TEST_CASE("IsAssetPath validates relative asset identifiers") {
    using SP::ServeHtml::IsAssetPath;
    CHECK(IsAssetPath("css/app.css"));
    CHECK(IsAssetPath("images/icons/logo.png"));
    CHECK(IsAssetPath("/absolute/path"));
    CHECK_FALSE(IsAssetPath(""));
    CHECK_FALSE(IsAssetPath("../secret.txt"));
    CHECK_FALSE(IsAssetPath("bad//asset"));
}
}
