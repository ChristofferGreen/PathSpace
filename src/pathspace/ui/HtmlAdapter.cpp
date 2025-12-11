#include <pathspace/ui/HtmlAdapter.hpp>
#include <pathspace/core/Error.hpp>
#include <pathspace/ui/DrawCommands.hpp>

#include <algorithm>
#include <cctype>
#include <array>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace SP::UI::Html {
namespace {

template <typename T>
auto read_struct(std::vector<std::uint8_t> const& payload,
                 std::size_t offset) -> T {
    T value{};
    std::memcpy(&value,
                payload.data() + static_cast<std::ptrdiff_t>(offset),
                sizeof(T));
    return value;
}

auto fingerprint_to_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto make_placeholder_asset(std::string logical_path,
                            Html::AssetKind kind) -> Asset {
    Asset asset{};
    asset.logical_path = std::move(logical_path);
    switch (kind) {
    case Html::AssetKind::Image:
        asset.mime_type = std::string(kImageAssetReferenceMime);
        break;
    case Html::AssetKind::Font:
        asset.mime_type = std::string(kFontAssetReferenceMime);
        break;
    }
    auto bytes_view = std::string_view(asset.logical_path);
    asset.bytes.assign(bytes_view.begin(), bytes_view.end());
    return asset;
}

auto resolve_asset(Html::EmitOptions const& options,
                   std::string const& logical_path,
                   std::uint64_t fingerprint,
                   Html::AssetKind kind) -> SP::Expected<Asset> {
    if (options.resolve_asset) {
        auto resolved = options.resolve_asset(logical_path, fingerprint, kind);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        if (resolved->logical_path.empty()) {
            resolved->logical_path = logical_path;
        }
        return resolved;
    }
    return make_placeholder_asset(logical_path, kind);
}

auto css_escape_single_quotes(std::string const& value) -> std::string {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\'') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

auto infer_font_family(std::string const& logical_path) -> std::string {
    auto slash = logical_path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? logical_path : logical_path.substr(slash + 1);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    if (name.empty()) {
        return "PathSpaceFont";
    }
    std::string family;
    family.reserve(name.size());
    for (char ch : name) {
        switch (ch) {
        case '_':
        case '-':
            family.push_back(' ');
            break;
        default:
            family.push_back(ch);
            break;
        }
    }
    return family;
}

auto infer_font_format(std::string const& mime_type, std::string const& logical_path) -> std::string {
    auto lower = [](std::string const& text) {
        std::string out;
        out.reserve(text.size());
        for (char ch : text) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return out;
    };

    auto mime = lower(mime_type);
    if (mime == "font/woff2" || mime == "application/font-woff2") {
        return "woff2";
    }
    if (mime == "font/woff" || mime == "application/font-woff") {
        return "woff";
    }
    if (mime == "font/ttf" || mime == "application/x-font-ttf") {
        return "truetype";
    }
    if (mime == "font/otf" || mime == "application/x-font-opentype") {
        return "opentype";
    }

    auto slash = logical_path.find_last_of('/');
    std::string file = (slash == std::string::npos) ? logical_path : logical_path.substr(slash + 1);
    auto dot = file.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? std::string{} : file.substr(dot + 1);
    ext = lower(ext);
    if (ext == "woff2") {
        return "woff2";
    }
    if (ext == "woff") {
        return "woff";
    }
    if (ext == "ttf") {
        return "truetype";
    }
    if (ext == "otf") {
        return "opentype";
    }
    return "truetype";
}

struct FontFaceDesc {
    std::string logical_path;
    std::string family;
    std::string css_style;
    std::string css_weight;
    std::uint64_t fingerprint = 0;
};

auto to_lower_ascii(std::string_view text) -> std::string {
    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

auto contains_token_ci(std::string_view haystack, std::string_view needle) -> bool {
    auto lowered_haystack = to_lower_ascii(haystack);
    auto lowered_needle = to_lower_ascii(needle);
    return lowered_haystack.find(lowered_needle) != std::string::npos;
}

auto parse_font_family_and_style(std::string const& resource_root) -> std::pair<std::string, std::string> {
    std::pair<std::string, std::string> result;
    if (resource_root.empty()) {
        return result;
    }
    constexpr std::string_view marker = "/fonts/";
    auto pos = resource_root.find(marker);
    if (pos == std::string::npos) {
        return result;
    }
    auto family_start = pos + marker.size();
    auto family_end = resource_root.find('/', family_start);
    if (family_end == std::string::npos) {
        return result;
    }
    result.first = resource_root.substr(family_start, family_end - family_start);
    auto style_start = family_end + 1;
    auto style_end = resource_root.find('/', style_start);
    if (style_start < resource_root.size()) {
        if (style_end == std::string::npos) {
            result.second = resource_root.substr(style_start);
        } else {
            result.second = resource_root.substr(style_start, style_end - style_start);
        }
    }
    return result;
}

auto infer_font_weight_from_style(std::string const& style_name) -> std::string {
    if (style_name.empty()) {
        return "400";
    }
    if (contains_token_ci(style_name, "thin")) {
        return "100";
    }
    if (contains_token_ci(style_name, "extralight") || contains_token_ci(style_name, "ultralight")) {
        return "200";
    }
    if (contains_token_ci(style_name, "light")) {
        return "300";
    }
    if (contains_token_ci(style_name, "medium")) {
        return "500";
    }
    if (contains_token_ci(style_name, "semibold") || contains_token_ci(style_name, "demibold")) {
        return "600";
    }
    if (contains_token_ci(style_name, "bold")) {
        return "700";
    }
    if (contains_token_ci(style_name, "extrabold") || contains_token_ci(style_name, "ultrabold")) {
        return "800";
    }
    if (contains_token_ci(style_name, "black")) {
        return "900";
    }
    return "400";
}

auto infer_font_css_style(std::string const& style_name) -> std::string {
    if (contains_token_ci(style_name, "italic")) {
        return "italic";
    }
    if (contains_token_ci(style_name, "oblique")) {
        return "oblique";
    }
    return "normal";
}

auto make_font_logical_path(std::uint64_t fingerprint) -> std::string {
    auto hex = fingerprint_to_hex(fingerprint);
    std::string logical = "fonts/";
    logical += hex;
    logical += ".woff2";
    return logical;
}

auto color_to_css(std::array<float, 4> const& rgba, bool premultiplied = false) -> std::string {
    auto clamp = [](float v) {
        return std::clamp(v, 0.0f, 1.0f);
    };
    auto a = clamp(rgba[3]);
    float r = rgba[0];
    float g = rgba[1];
    float b = rgba[2];
    if (premultiplied && a > 0.0f) {
        r /= a;
        g /= a;
        b /= a;
    }
    auto to_channel = [&](float c) -> int {
        return static_cast<int>(std::round(clamp(c) * 255.0f));
    };
    std::ostringstream oss;
    oss << "rgba(" << to_channel(r) << "," << to_channel(g) << "," << to_channel(b) << "," << std::setprecision(3)
        << clamp(a) << ")";
    return oss.str();
}

struct HtmlNode {
    enum class Kind { Rect, RoundedRect, Image, Text, Path, Mesh, Stroke };

    Kind kind = Kind::Rect;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> corner_radius{0.0f, 0.0f, 0.0f, 0.0f};
    std::uint64_t fingerprint = 0;
    std::uint32_t glyph_count = 0;
    std::uint32_t vertex_count = 0;
    bool has_fingerprint = false;
    float stroke_width = 0.0f;
    std::uint32_t stroke_point_offset = 0;
    std::uint32_t stroke_point_count = 0;
};

auto nodes_to_canvas_commands(std::vector<HtmlNode> const& nodes) -> std::vector<CanvasCommand> {
    std::vector<CanvasCommand> commands;
    commands.reserve(nodes.size());
    for (auto const& node : nodes) {
        CanvasCommand command{};
        command.x = node.min_x;
        command.y = node.min_y;
        command.width = std::max(0.0f, node.max_x - node.min_x);
        command.height = std::max(0.0f, node.max_y - node.min_y);
        switch (node.kind) {
        case HtmlNode::Kind::Rect:
            command.type = CanvasCommandType::Rect;
            command.color = node.color;
            command.opacity = node.color[3];
            break;
        case HtmlNode::Kind::RoundedRect:
            command.type = CanvasCommandType::RoundedRect;
            command.color = node.color;
            command.corner_radii = node.corner_radius;
            command.opacity = node.color[3];
            break;
        case HtmlNode::Kind::Image:
            command.type = CanvasCommandType::Image;
            command.fingerprint = node.fingerprint;
            command.has_fingerprint = node.has_fingerprint;
            command.color = node.tint;
            command.opacity = node.tint[3];
            break;
        case HtmlNode::Kind::Text:
            command.type = CanvasCommandType::Text;
            command.color = node.color;
            command.opacity = node.color[3];
            command.glyph_count = node.glyph_count;
            break;
        case HtmlNode::Kind::Path:
            command.type = CanvasCommandType::Path;
            command.color = node.color;
            command.opacity = node.color[3];
            break;
        case HtmlNode::Kind::Mesh:
            command.type = CanvasCommandType::Mesh;
            command.color = node.color;
            command.opacity = node.color[3];
            command.vertex_count = node.vertex_count;
            break;
        case HtmlNode::Kind::Stroke:
            command.type = CanvasCommandType::Stroke;
            command.color = node.color;
            command.opacity = node.color[3];
            command.stroke_width = node.stroke_width;
            command.stroke_point_offset = node.stroke_point_offset;
            command.stroke_point_count = node.stroke_point_count;
            break;
        }
        commands.push_back(command);
    }
    return commands;
}

auto build_dom(std::vector<HtmlNode> const& nodes,
               std::unordered_map<std::uint64_t, Asset> const& asset_map,
               std::vector<CanvasCommand> const& commands) -> EmitResult {
    EmitResult result{};
    std::ostringstream dom;
    dom << "<div class=\"ps-scene\" data-node-count=\"" << nodes.size() << "\">\n";

    auto append_style_rect = [](std::ostringstream& out,
                                HtmlNode const& node,
                                std::string const& color_css,
                                std::optional<std::string> tint_css = std::nullopt) {
        auto width = std::max(0.0f, node.max_x - node.min_x);
        auto height = std::max(0.0f, node.max_y - node.min_y);
        out << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
        out << "width:" << width << "px;height:" << height << "px;";
        out << "background-color:" << color_css << ";";
        if (tint_css) {
            out << "opacity:" << tint_css.value() << ";";
        }
        out << "\"";
    };

    for (auto const& node : nodes) {
        auto color_css = color_to_css(node.color);
        switch (node.kind) {
        case HtmlNode::Kind::Rect: {
            dom << "  <div class=\"ps-node ps-rect\"";
            append_style_rect(dom, node, color_css);
            dom << "></div>\n";
            break;
        }
        case HtmlNode::Kind::RoundedRect: {
            dom << "  <div class=\"ps-node ps-rounded-rect\"";
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "background-color:" << color_css << ";";
            dom << "border-radius:" << node.corner_radius[0] << "px "
                << node.corner_radius[1] << "px "
                << node.corner_radius[2] << "px "
                << node.corner_radius[3] << "px;\"";
            dom << "></div>\n";
            break;
        }
        case HtmlNode::Kind::Image: {
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << "  <img class=\"ps-node ps-image\" src=\"\" data-asset=\"images/"
                << fingerprint_to_hex(node.fingerprint) << ".png\"";
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "opacity:" << std::clamp(node.tint[3], 0.0f, 1.0f) << ";\"";
            dom << " alt=\"\" />\n";
            break;
        }
        case HtmlNode::Kind::Text: {
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << "  <div class=\"ps-node ps-text\"";
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "color:" << color_css << ";\"";
            dom << " data-glyphs=\"" << node.glyph_count << "\"></div>\n";
            break;
        }
        case HtmlNode::Kind::Path: {
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << "  <div class=\"ps-node ps-path\"";
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "border:1px solid " << color_css << ";\"";
            dom << "></div>\n";
            break;
        }
        case HtmlNode::Kind::Mesh: {
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << "  <div class=\"ps-node ps-mesh\"";
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "border:1px dashed " << color_css << ";\"";
            dom << " data-vertices=\"" << node.vertex_count << "\"></div>\n";
            break;
        }
        case HtmlNode::Kind::Stroke: {
            auto width = std::max(0.0f, node.max_x - node.min_x);
            auto height = std::max(0.0f, node.max_y - node.min_y);
            dom << "  <div class=\"ps-node ps-stroke\"";
            dom << " style=\"left:" << node.min_x << "px;top:" << node.min_y << "px;";
            dom << "width:" << width << "px;height:" << height << "px;";
            dom << "border:1px solid " << color_css << ";\"";
            dom << " data-stroke-width=\"" << node.stroke_width << "\"";
            dom << "></div>\n";
            break;
        }
        }
    }
    dom << "</div>\n";

    result.dom = dom.str();
    result.css =
        ".ps-scene{position:relative;display:block;font-family:sans-serif;}\n"
        ".ps-node{position:absolute;box-sizing:border-box;}\n"
        ".ps-image{object-fit:contain;}\n";
    result.canvas_commands = "[]";
    result.used_canvas_fallback = false;
    result.assets.reserve(asset_map.size());
    for (auto const& entry : asset_map) {
        result.assets.push_back(entry.second);
    }
    result.canvas_replay_commands = commands;
    return result;
}

auto build_canvas(std::vector<HtmlNode> const& nodes,
                  std::unordered_map<std::uint64_t, Asset> const& asset_map,
                  std::vector<CanvasCommand> const& commands,
                  std::span<Scene::StrokePoint const> stroke_points) -> EmitResult {
    EmitResult result{};
    std::ostringstream canvas;
    canvas << "[";
    bool first = true;
    auto emit_separator = [&]() {
        if (!first) {
            canvas << ",";
        } else {
            first = false;
        }
    };
    auto width = [](HtmlNode const& node) {
        return std::max(0.0f, node.max_x - node.min_x);
    };
    auto height = [](HtmlNode const& node) {
        return std::max(0.0f, node.max_y - node.min_y);
    };
    auto append_rect_fields = [&](HtmlNode const& node) {
        canvas << "\"x\":" << node.min_x << ",\"y\":" << node.min_y
               << ",\"width\":" << width(node) << ",\"height\":" << height(node);
    };

    for (auto const& node : nodes) {
        emit_separator();
        canvas << "{";
        switch (node.kind) {
        case HtmlNode::Kind::Rect:
            canvas << "\"type\":\"rect\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            break;
        case HtmlNode::Kind::RoundedRect:
            canvas << "\"type\":\"rounded_rect\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            canvas << ",\"radii\":[" << node.corner_radius[0] << "," << node.corner_radius[1]
                   << "," << node.corner_radius[2] << "," << node.corner_radius[3] << "]";
            break;
        case HtmlNode::Kind::Image:
            canvas << "\"type\":\"image\",";
            append_rect_fields(node);
            canvas << ",\"asset\":\"images/" << fingerprint_to_hex(node.fingerprint) << ".png\"";
            canvas << ",\"opacity\":" << std::clamp(node.tint[3], 0.0f, 1.0f);
            break;
        case HtmlNode::Kind::Text:
            canvas << "\"type\":\"text\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            canvas << ",\"glyphs\":" << node.glyph_count;
            break;
        case HtmlNode::Kind::Path:
            canvas << "\"type\":\"path\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            break;
        case HtmlNode::Kind::Mesh:
            canvas << "\"type\":\"mesh\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            canvas << ",\"vertices\":" << node.vertex_count;
            break;
        case HtmlNode::Kind::Stroke: {
            canvas << "\"type\":\"stroke\",";
            append_rect_fields(node);
            canvas << ",\"color\":\"" << color_to_css(node.color) << "\"";
            canvas << ",\"strokeWidth\":" << node.stroke_width;
            canvas << ",\"points\":[";
            bool first_point = true;
            auto offset = static_cast<std::size_t>(node.stroke_point_offset);
            auto count = static_cast<std::size_t>(node.stroke_point_count);
            if (offset + count > stroke_points.size()) {
                count = 0;
            }
            for (std::size_t idx = 0; idx < count; ++idx) {
                auto const& pt = stroke_points[offset + idx];
                if (!first_point) {
                    canvas << ",";
                } else {
                    first_point = false;
                }
                canvas << "[" << pt.x << "," << pt.y << "]";
            }
            canvas << "]";
            break;
        }
        }
        canvas << "}";
    }
    canvas << "]";

    result.canvas_commands = canvas.str();
    result.dom.clear();
    result.css.clear();
    result.used_canvas_fallback = true;
    result.assets.reserve(asset_map.size());
    for (auto const& entry : asset_map) {
        result.assets.push_back(entry.second);
    }
    result.canvas_replay_commands = commands;
    result.stroke_points.assign(stroke_points.begin(), stroke_points.end());
    return result;
}

} // namespace

auto Adapter::emit(Scene::DrawableBucketSnapshot const& snapshot,
                   EmitOptions const& options) -> SP::Expected<EmitResult> {
    std::size_t command_total = snapshot.command_kinds.size();
    std::vector<std::size_t> payload_offsets;
    payload_offsets.reserve(command_total);

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < command_total; ++i) {
        payload_offsets.push_back(cursor);
        auto kind = static_cast<Scene::DrawCommandKind>(snapshot.command_kinds[i]);
        cursor += Scene::payload_size_bytes(kind);
    }
    if (cursor > snapshot.command_payload.size()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                         "command payload buffer too small"});
    }

    std::vector<HtmlNode> nodes;
    nodes.reserve(snapshot.drawable_ids.size());
    std::unordered_map<std::uint64_t, Asset> image_assets;
    std::unordered_map<std::string, Asset> font_assets;
    std::unordered_set<std::string> font_paths;
    std::vector<FontFaceDesc> font_face_descs;
    font_face_descs.reserve(snapshot.font_assets.size() + options.font_logical_paths.size());
    bool requires_canvas_only = false;

    auto ensure_payload = [&](std::size_t command_index,
                              Scene::DrawCommandKind kind) -> SP::Expected<std::size_t> {
        if (command_index >= payload_offsets.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                             "command index out of range"});
        }
        auto offset = payload_offsets[command_index];
        auto size = Scene::payload_size_bytes(kind);
        if (offset + size > snapshot.command_payload.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                             "command payload exceeds buffer"});
        }
        return offset;
    };

    for (std::size_t drawable = 0; drawable < snapshot.drawable_ids.size(); ++drawable) {
        if (!snapshot.visibility.empty()
            && drawable < snapshot.visibility.size()
            && snapshot.visibility[drawable] == 0) {
            continue;
        }
        if (drawable >= snapshot.command_offsets.size()
            || drawable >= snapshot.command_counts.size()) {
            return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                             "command metadata missing"});
        }
        auto command_offset = snapshot.command_offsets[drawable];
        auto command_count = snapshot.command_counts[drawable];
        for (std::uint32_t local_index = 0; local_index < command_count; ++local_index) {
            auto command_index = static_cast<std::size_t>(command_offset) + local_index;
            if (command_index >= snapshot.command_kinds.size()) {
                return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                                 "command index exceeds buffer"});
            }
            auto kind = static_cast<Scene::DrawCommandKind>(snapshot.command_kinds[command_index]);
            auto payload_offset = ensure_payload(command_index, kind);
            if (!payload_offset) {
                return std::unexpected(payload_offset.error());
            }

            HtmlNode node{};
            switch (kind) {
            case Scene::DrawCommandKind::Rect: {
                auto rect = read_struct<Scene::RectCommand>(snapshot.command_payload, *payload_offset);
                node.kind = HtmlNode::Kind::Rect;
                node.min_x = rect.min_x;
                node.min_y = rect.min_y;
                node.max_x = rect.max_x;
                node.max_y = rect.max_y;
                node.color = rect.color;
                nodes.push_back(node);
                break;
            }
            case Scene::DrawCommandKind::RoundedRect: {
                auto rounded = read_struct<Scene::RoundedRectCommand>(snapshot.command_payload, *payload_offset);
                node.kind = HtmlNode::Kind::RoundedRect;
                node.min_x = rounded.min_x;
                node.min_y = rounded.min_y;
                node.max_x = rounded.max_x;
                node.max_y = rounded.max_y;
                node.color = rounded.color;
                node.corner_radius = {rounded.radius_top_left,
                                      rounded.radius_top_right,
                                      rounded.radius_bottom_right,
                                      rounded.radius_bottom_left};
                nodes.push_back(node);
                break;
            }
            case Scene::DrawCommandKind::Image: {
                auto image = read_struct<Scene::ImageCommand>(snapshot.command_payload, *payload_offset);
                node.kind = HtmlNode::Kind::Image;
                node.min_x = image.min_x;
                node.min_y = image.min_y;
                node.max_x = image.max_x;
                node.max_y = image.max_y;
                node.tint = image.tint;
                node.has_fingerprint = true;
                node.fingerprint = image.image_fingerprint;
                if (image_assets.find(image.image_fingerprint) == image_assets.end()) {
                    auto logical_path = std::string("images/")
                                        + fingerprint_to_hex(image.image_fingerprint) + ".png";
                    auto asset = resolve_asset(options,
                                               logical_path,
                                               image.image_fingerprint,
                                               Html::AssetKind::Image);
                    if (!asset) {
                        return std::unexpected(asset.error());
                    }
                    image_assets.emplace(image.image_fingerprint, std::move(*asset));
                }
                nodes.push_back(node);
                break;
            }
            case Scene::DrawCommandKind::TextGlyphs: {
                auto glyphs = read_struct<Scene::TextGlyphsCommand>(snapshot.command_payload, *payload_offset);
                node.kind = HtmlNode::Kind::Text;
                node.min_x = glyphs.min_x;
                node.min_y = glyphs.min_y;
                node.max_x = glyphs.max_x;
                node.max_y = glyphs.max_y;
                node.color = glyphs.color;
                node.glyph_count = glyphs.glyph_count;
                nodes.push_back(node);
                break;
            }
        case Scene::DrawCommandKind::Path: {
            auto path = read_struct<Scene::PathCommand>(snapshot.command_payload, *payload_offset);
            node.kind = HtmlNode::Kind::Path;
            node.min_x = path.min_x;
            node.min_y = path.min_y;
            node.max_x = path.max_x;
            node.max_y = path.max_y;
            node.color = path.fill_color;
            nodes.push_back(node);
            break;
        }
        case Scene::DrawCommandKind::Stroke: {
            auto stroke = read_struct<Scene::StrokeCommand>(snapshot.command_payload, *payload_offset);
            node.kind = HtmlNode::Kind::Stroke;
            node.min_x = stroke.min_x;
            node.min_y = stroke.min_y;
            node.max_x = stroke.max_x;
            node.max_y = stroke.max_y;
            node.color = stroke.color;
            node.stroke_width = std::max(stroke.thickness, 0.0f);
            node.stroke_point_offset = stroke.point_offset;
            node.stroke_point_count = stroke.point_count;
            auto offset = static_cast<std::size_t>(stroke.point_offset);
            auto count = static_cast<std::size_t>(stroke.point_count);
            if (offset + count > snapshot.stroke_points.size()) {
                return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                                 "stroke command references out-of-range point buffer"});
            }
            nodes.push_back(node);
            requires_canvas_only = true;
            break;
        }
        case Scene::DrawCommandKind::Mesh: {
            auto mesh = read_struct<Scene::MeshCommand>(snapshot.command_payload, *payload_offset);
            node.kind = HtmlNode::Kind::Mesh;
            node.min_x = 0.0f;
            node.min_y = 0.0f;
                node.max_x = 0.0f;
                node.max_y = 0.0f;
                node.color = mesh.color;
                node.vertex_count = mesh.vertex_count;
                nodes.push_back(node);
                break;
            }
            }
        }
    }

    auto add_font_face = [&](FontFaceDesc desc) {
        if (desc.logical_path.empty()) {
            return;
        }
        if (!font_paths.insert(desc.logical_path).second) {
            return;
        }
        if (desc.family.empty()) {
            desc.family = infer_font_family(desc.logical_path);
        }
        if (desc.css_style.empty()) {
            desc.css_style = "normal";
        }
        if (desc.css_weight.empty()) {
            desc.css_weight = "400";
        }
        font_face_descs.push_back(std::move(desc));
    };

    for (auto const& font_ref : snapshot.font_assets) {
        if (font_ref.fingerprint == 0) {
            continue;
        }
        FontFaceDesc desc{};
        desc.logical_path = make_font_logical_path(font_ref.fingerprint);
        desc.fingerprint = font_ref.fingerprint;
        auto [family, style_name] = parse_font_family_and_style(font_ref.resource_root);
        desc.family = std::move(family);
        desc.css_style = infer_font_css_style(style_name);
        desc.css_weight = infer_font_weight_from_style(style_name);
        add_font_face(std::move(desc));
    }

    for (auto const& logical_path : options.font_logical_paths) {
        if (logical_path.empty()) {
            continue;
        }
        FontFaceDesc desc{};
        desc.logical_path = logical_path;
        desc.family = infer_font_family(logical_path);
        desc.css_style = "normal";
        desc.css_weight = "400";
        desc.fingerprint = 0;
        add_font_face(std::move(desc));
    }

    for (auto const& face : font_face_descs) {
        auto asset = resolve_asset(options, face.logical_path, face.fingerprint, Html::AssetKind::Font);
        if (!asset) {
            font_assets.emplace(face.logical_path,
                                make_placeholder_asset(face.logical_path, Html::AssetKind::Font));
            continue;
        }
        font_assets.emplace(face.logical_path, std::move(*asset));
    }

    auto dom_node_count = nodes.size();
    bool dom_allowed = options.prefer_dom;
    bool dom_within_budget = options.max_dom_nodes == 0
                             || dom_node_count <= options.max_dom_nodes;
    if (requires_canvas_only) {
        dom_allowed = false;
    }
    if (requires_canvas_only && !options.allow_canvas_fallback) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                         "stroke commands require canvas fallback but it is disabled"});
    }

    auto replay_commands = nodes_to_canvas_commands(nodes);
    EmitResult result{};

    if (dom_allowed && dom_within_budget) {
        result = build_dom(nodes, image_assets, replay_commands);
    } else {
        if (!options.allow_canvas_fallback) {
            return std::unexpected(SP::Error{SP::Error::Code::InvalidType,
                                             "DOM node budget exceeded and canvas fallback is disabled"});
        }
        result = build_canvas(nodes, image_assets, replay_commands, snapshot.stroke_points);
    }

    for (auto const& entry : font_assets) {
        result.assets.push_back(entry.second);
    }

    if (!font_face_descs.empty()) {
        if (!result.css.empty() && result.css.back() != '\n') {
            result.css.push_back('\n');
        }
        for (auto const& face : font_face_descs) {
            auto asset_it = font_assets.find(face.logical_path);
            if (asset_it == font_assets.end()) {
                continue;
            }
            auto const& asset = asset_it->second;
            auto family_name = face.family.empty() ? infer_font_family(face.logical_path) : face.family;
            auto family = css_escape_single_quotes(family_name);
            auto format = infer_font_format(asset.mime_type, asset.logical_path);
            auto style_value = face.css_style.empty() ? std::string{"normal"} : face.css_style;
            auto weight_value = face.css_weight.empty() ? std::string{"400"} : face.css_weight;
            result.css += "@font-face{font-family:'";
            result.css += family;
            result.css += "';font-style:";
            result.css += style_value;
            result.css += ";font-weight:";
            result.css += weight_value;
            result.css += ";src:url(\"assets/";
            result.css += asset.logical_path;
            result.css += "\") format('";
            result.css += format;
            result.css += "');font-display:swap;}\n";
        }
    }

    if (result.stroke_points.empty() && !snapshot.stroke_points.empty()) {
        result.stroke_points.assign(snapshot.stroke_points.begin(), snapshot.stroke_points.end());
    }

    return result;
}

} // namespace SP::UI::Html
