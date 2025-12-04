#include <pathspace/web/serve_html/HtmlPayload.hpp>

#include <string>

namespace SP::ServeHtml {

namespace {

std::string EscapeScriptPayload(std::string const& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i + 8 < text.size() && text.compare(i, 9, "</script>") == 0) {
            escaped.append("<\\/script>");
            i += 8;
        } else {
            escaped.push_back(text[i]);
        }
    }
    return escaped;
}

} // namespace

std::string BuildHtmlResponseBody(HtmlPayload const& payload,
                                  std::string_view   app,
                                  std::string_view   view) {
    std::string body;
    body.reserve(payload.dom.size() + 1024);
    body.append("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">");
    body.append("<meta http-equiv=\"Cache-Control\" content=\"no-store\">");
    body.append("<title>");
    body.append(app);
    body.append(" — ");
    body.append(view);
    body.append("</title>");

    if (payload.css && !payload.css->empty()) {
        body.append("<style>");
        body.append(*payload.css);
        body.append("</style>");
    }
    if (payload.js && !payload.js->empty()) {
        body.append("<script>\n");
        body.append(EscapeScriptPayload(*payload.js));
        body.append("\n</script>");
    }
    if (payload.commands && !payload.commands->empty()) {
        body.append("<script type=\"application/json\" id=\"pathspace-commands\">");
        body.append(EscapeScriptPayload(*payload.commands));
        body.append("</script>");
    }

    body.append("</head><body>");
    body.append(payload.dom);
    body.append("</body></html>");
    return body;
}

} // namespace SP::ServeHtml

