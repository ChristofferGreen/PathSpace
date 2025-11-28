#include "PathRenderer2DDetail.hpp"
#include "PathRenderer2DInternal.hpp"

#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/ImageCache.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace SP::UI::PathRenderer2DDetail {
namespace {

namespace Scene = SP::UI::Scene;

void blend_pixel(float* dest, LinearPremulColor const& src) {
    auto const inv_alpha = 1.0f - src.a;
    dest[0] = clamp_unit(src.r + dest[0] * inv_alpha);
    dest[1] = clamp_unit(src.g + dest[1] * inv_alpha);
    dest[2] = clamp_unit(src.b + dest[2] * inv_alpha);
    dest[3] = clamp_unit(src.a + dest[3] * inv_alpha);
}

auto draw_rect_area(float min_x,
                    float min_y,
                    float max_x,
                    float max_y,
                    LinearPremulColor const& color,
                    std::vector<float>& buffer,
                    int width,
                    int height) -> bool {
    auto const clipped_min_x = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto const clipped_min_y = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto const clipped_max_x = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto const clipped_max_y = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (clipped_min_x >= clipped_max_x || clipped_min_y >= clipped_max_y) {
        return false;
    }

    auto const row_stride = static_cast<std::size_t>(width) * 4u;
    for (int y = clipped_min_y; y < clipped_max_y; ++y) {
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (int x = clipped_min_x; x < clipped_max_x; ++x) {
            auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
            blend_pixel(dest, color);
        }
    }
    return true;
}

auto multiply_straight(LinearStraightColor lhs, LinearStraightColor rhs) -> LinearStraightColor {
    return LinearStraightColor{
        .r = clamp_unit(lhs.r * rhs.r),
        .g = clamp_unit(lhs.g * rhs.g),
        .b = clamp_unit(lhs.b * rhs.b),
        .a = clamp_unit(lhs.a * rhs.a),
    };
}

auto sample_font_atlas_alpha(FontAtlasData const& atlas,
                             float u,
                             float v) -> float {
    if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.empty()) {
        return 0.0f;
    }
    u = clamp_unit(u);
    v = clamp_unit(v);
    auto max_x = static_cast<float>(atlas.width - 1);
    auto max_y = static_cast<float>(atlas.height - 1);
    auto x = std::clamp(static_cast<int>(std::round(u * max_x)), 0, static_cast<int>(atlas.width - 1));
    auto y = std::clamp(static_cast<int>(std::round(v * max_y)), 0, static_cast<int>(atlas.height - 1));
    auto index = static_cast<std::size_t>(y) * atlas.width + static_cast<std::size_t>(x);
    if (index >= atlas.pixels.size()) {
        return 0.0f;
    }
    return clamp_unit(static_cast<float>(atlas.pixels[index]) / 255.0f);
}

auto sample_font_atlas_rgba(FontAtlasData const& atlas,
                            float u,
                            float v) -> LinearPremulColor {
    LinearPremulColor color{};
    if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.empty() || atlas.bytes_per_pixel < 4) {
        return color;
    }
    u = clamp_unit(u);
    v = clamp_unit(v);
    auto max_x = static_cast<float>(atlas.width - 1);
    auto max_y = static_cast<float>(atlas.height - 1);
    auto x = std::clamp(static_cast<int>(std::round(u * max_x)), 0, static_cast<int>(atlas.width - 1));
    auto y = std::clamp(static_cast<int>(std::round(v * max_y)), 0, static_cast<int>(atlas.height - 1));
    auto index = (static_cast<std::size_t>(y) * atlas.width + static_cast<std::size_t>(x))
                 * static_cast<std::size_t>(atlas.bytes_per_pixel);
    if (index + 3 >= atlas.pixels.size()) {
        return color;
    }
    float r = static_cast<float>(atlas.pixels[index + 0]) / 255.0f;
    float g = static_cast<float>(atlas.pixels[index + 1]) / 255.0f;
    float b = static_cast<float>(atlas.pixels[index + 2]) / 255.0f;
    float a = static_cast<float>(atlas.pixels[index + 3]) / 255.0f;
    a = clamp_unit(a);
    color.a = a;
    color.r = clamp_unit(r * a);
    color.g = clamp_unit(g * a);
    color.b = clamp_unit(b * a);
    return color;
}

auto sample_image_linear(ImageCache::ImageData const& image,
                         float u,
                         float v) -> LinearStraightColor {
    if (image.width == 0 || image.height == 0) {
        return LinearStraightColor{};
    }

    auto clamp01 = [](float value) -> float {
        return std::clamp(value, 0.0f, 1.0f);
    };

    u = clamp01(u);
    v = clamp01(v);

    auto max_x = static_cast<float>(image.width - 1);
    auto max_y = static_cast<float>(image.height - 1);

    auto x = u * max_x;
    auto y = v * max_y;

    auto x0 = static_cast<int>(std::floor(x));
    auto y0 = static_cast<int>(std::floor(y));
    auto x1 = std::min(x0 + 1, static_cast<int>(image.width - 1));
    auto y1 = std::min(y0 + 1, static_cast<int>(image.height - 1));

    auto tx = x - static_cast<float>(x0);
    auto ty = y - static_cast<float>(y0);

    auto fetch = [&](int ix, int iy) -> LinearStraightColor {
        auto index = (static_cast<std::size_t>(iy) * image.width + static_cast<std::size_t>(ix)) * 4u;
        return LinearStraightColor{
            .r = image.pixels[index + 0],
            .g = image.pixels[index + 1],
            .b = image.pixels[index + 2],
            .a = image.pixels[index + 3],
        };
    };

    auto c00 = fetch(x0, y0);
    auto c10 = fetch(x1, y0);
    auto c01 = fetch(x0, y1);
    auto c11 = fetch(x1, y1);

    auto lerp = [](float a, float b, float t) -> float {
        return a + (b - a) * t;
    };

    auto interp_row = [&](LinearStraightColor const& a,
                          LinearStraightColor const& b) -> LinearStraightColor {
        return LinearStraightColor{
            .r = lerp(a.r, b.r, tx),
            .g = lerp(a.g, b.g, tx),
            .b = lerp(a.b, b.b, tx),
            .a = lerp(a.a, b.a, tx),
        };
    };

    auto top = interp_row(c00, c10);
    auto bottom = interp_row(c01, c11);

    return LinearStraightColor{
        .r = lerp(top.r, bottom.r, ty),
        .g = lerp(top.g, bottom.g, ty),
        .b = lerp(top.b, bottom.b, ty),
        .a = lerp(top.a, bottom.a, ty),
    };
}

auto draw_disc(float center_x,
               float center_y,
               float radius,
               LinearPremulColor const& color,
               std::vector<float>& buffer,
               int width,
               int height) -> bool {
    if (radius <= 0.0f) {
        return draw_rect_area(center_x, center_y, center_x + 1.0f, center_y + 1.0f, color, buffer, width, height);
    }
    auto min_x = std::clamp(static_cast<int>(std::floor(center_x - radius)), 0, width);
    auto max_x = std::clamp(static_cast<int>(std::ceil(center_x + radius)), 0, width);
    auto min_y = std::clamp(static_cast<int>(std::floor(center_y - radius)), 0, height);
    auto max_y = std::clamp(static_cast<int>(std::ceil(center_y + radius)), 0, height);
    if (min_x >= max_x || min_y >= max_y) {
        return false;
    }
    auto radius_sq = radius * radius;
    bool drawn = false;
    auto row_stride = static_cast<std::size_t>(width) * 4u;
    for (int y = min_y; y < max_y; ++y) {
        auto py = static_cast<float>(y) + 0.5f;
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (int x = min_x; x < max_x; ++x) {
            auto px = static_cast<float>(x) + 0.5f;
            auto dx = px - center_x;
            auto dy = py - center_y;
            auto dist_sq = dx * dx + dy * dy;
            if (dist_sq <= radius_sq) {
                auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
                blend_pixel(dest, color);
                drawn = true;
            }
        }
    }
    return drawn;
}

auto distance_to_segment_sq(float px,
                            float py,
                            float ax,
                            float ay,
                            float bx,
                            float by) -> float {
    auto vx = bx - ax;
    auto vy = by - ay;
    auto ux = px - ax;
    auto uy = py - ay;
    auto len_sq = vx * vx + vy * vy;
    float t = 0.0f;
    if (len_sq > 0.0f) {
        t = (ux * vx + uy * vy) / len_sq;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    auto proj_x = ax + vx * t;
    auto proj_y = ay + vy * t;
    auto dx = proj_x - px;
    auto dy = proj_y - py;
    return dx * dx + dy * dy;
}

auto draw_stroke_segment(Scene::StrokePoint const& a,
                         Scene::StrokePoint const& b,
                         float half_width,
                         LinearPremulColor const& color,
                         std::vector<float>& buffer,
                         int width,
                         int height) -> bool {
    auto min_x = std::clamp(static_cast<int>(std::floor(std::min(a.x, b.x) - half_width)), 0, width);
    auto max_x = std::clamp(static_cast<int>(std::ceil(std::max(a.x, b.x) + half_width)), 0, width);
    auto min_y = std::clamp(static_cast<int>(std::floor(std::min(a.y, b.y) - half_width)), 0, height);
    auto max_y = std::clamp(static_cast<int>(std::ceil(std::max(a.y, b.y) + half_width)), 0, height);
    if (min_x >= max_x || min_y >= max_y) {
        return false;
    }
    auto radius_sq = half_width * half_width;
    bool drawn = false;
    auto row_stride = static_cast<std::size_t>(width) * 4u;
    for (int y = min_y; y < max_y; ++y) {
        auto py = static_cast<float>(y) + 0.5f;
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (int x = min_x; x < max_x; ++x) {
            auto px = static_cast<float>(x) + 0.5f;
            auto dist_sq = distance_to_segment_sq(px, py, a.x, a.y, b.x, b.y);
            if (dist_sq <= radius_sq) {
                auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
                blend_pixel(dest, color);
                drawn = true;
            }
        }
    }
    return drawn;
}

} // namespace

auto color_from_drawable(std::uint64_t drawableId) -> std::array<float, 4> {
    auto r = static_cast<float>(drawableId & 0xFFu) / 255.0f;
    auto g = static_cast<float>((drawableId >> 8) & 0xFFu) / 255.0f;
    auto b = static_cast<float>((drawableId >> 16) & 0xFFu) / 255.0f;
    if (r == 0.0f && g == 0.0f && b == 0.0f) {
        r = 0.9f;
        g = 0.9f;
        b = 0.9f;
    }
    return {r, g, b, 1.0f};
}

auto draw_rect_command(Scene::RectCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height,
                       std::span<PathRenderer2DInternal::DamageRect const> clip_rects) -> bool {
    auto color = make_linear_color(command.color);
    if (clip_rects.empty()) {
        return draw_rect_area(command.min_x,
                              command.min_y,
                              command.max_x,
                              command.max_y,
                              color,
                              buffer,
                              width,
                              height);
    }
    bool drawn = false;
    for (auto const& clip : clip_rects) {
        auto min_x = std::max(command.min_x, static_cast<float>(clip.min_x));
        auto min_y = std::max(command.min_y, static_cast<float>(clip.min_y));
        auto max_x = std::min(command.max_x, static_cast<float>(clip.max_x));
        auto max_y = std::min(command.max_y, static_cast<float>(clip.max_y));
        if (min_x >= max_x || min_y >= max_y) {
            continue;
        }
        drawn |= draw_rect_area(min_x,
                                min_y,
                                max_x,
                                max_y,
                                color,
                                buffer,
                                width,
                                height);
    }
    return drawn;
}

auto draw_rounded_rect_command(Scene::RoundedRectCommand const& command,
                               std::vector<float>& buffer,
                               int width,
                               int height) -> bool {
    auto color = make_linear_color(command.color);

    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);

    auto width_f = std::max(0.0f, max_x - min_x);
    auto height_f = std::max(0.0f, max_y - min_y);
    if (width_f <= 0.0f || height_f <= 0.0f) {
        return false;
    }

    auto clamp_positive = [](float value) -> float {
        return std::max(0.0f, value);
    };

    auto radius_tl = clamp_positive(command.radius_top_left);
    auto radius_tr = clamp_positive(command.radius_top_right);
    auto radius_br = clamp_positive(command.radius_bottom_right);
    auto radius_bl = clamp_positive(command.radius_bottom_left);

    auto adjust_pair = [](float& a, float& b, float limit) {
        if (limit <= 0.0f) {
            a = 0.0f;
            b = 0.0f;
            return;
        }
        auto sum = a + b;
        if (sum > limit && sum > 0.0f) {
            auto scale = limit / sum;
            a *= scale;
            b *= scale;
        }
    };

    adjust_pair(radius_tl, radius_tr, width_f);
    adjust_pair(radius_bl, radius_br, width_f);
    adjust_pair(radius_tl, radius_bl, height_f);
    adjust_pair(radius_tr, radius_br, height_f);

    auto min_x_i = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto max_x_i = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto min_y_i = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto max_y_i = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
        return false;
    }

    auto radius_squared = [](float radius) -> float {
        return radius * radius;
    };

    auto blend_if_inside = [&](int x, int y) -> bool {
        auto px = static_cast<float>(x) + 0.5f;
        auto py = static_cast<float>(y) + 0.5f;

        if (px < min_x || px > max_x || py < min_y || py > max_y) {
            return false;
        }

        bool inside = true;

        if (radius_tl > 0.0f && px < (min_x + radius_tl) && py < (min_y + radius_tl)) {
            auto dx = px - (min_x + radius_tl);
            auto dy = py - (min_y + radius_tl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tl);
        } else if (radius_tr > 0.0f && px > (max_x - radius_tr) && py < (min_y + radius_tr)) {
            auto dx = px - (max_x - radius_tr);
            auto dy = py - (min_y + radius_tr);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tr);
        } else if (radius_br > 0.0f && px > (max_x - radius_br) && py > (max_y - radius_br)) {
            auto dx = px - (max_x - radius_br);
            auto dy = py - (max_y - radius_br);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_br);
        } else if (radius_bl > 0.0f && px < (min_x + radius_bl) && py > (max_y - radius_bl)) {
            auto dx = px - (min_x + radius_bl);
            auto dy = py - (max_y - radius_bl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_bl);
        }

        if (!inside) {
            return false;
        }

        auto row_stride = static_cast<std::size_t>(width) * 4u;
        auto base = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 4u;
        auto* dest = buffer.data() + base;
        blend_pixel(dest, color);
        return true;
    };

    bool drawn = false;
    for (int y = min_y_i; y < max_y_i; ++y) {
        for (int x = min_x_i; x < max_x_i; ++x) {
            if (blend_if_inside(x, y)) {
                drawn = true;
            }
        }
    }
    return drawn;
}

auto draw_shaped_text_command(Scene::TextGlyphsCommand const& command,
                              Scene::DrawableBucketSnapshot const& bucket,
                              std::shared_ptr<FontAtlasData const> const& atlas_ptr,
                              LinearPremulColor const& base_color,
                              LinearStraightColor const& tint_straight,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    if (!atlas_ptr || width <= 0 || height <= 0) {
        return false;
    }
    auto const& atlas = *atlas_ptr;
    if (atlas.width == 0 || atlas.height == 0 || atlas.pixels.empty()) {
        return false;
    }

    auto const glyph_offset = static_cast<std::size_t>(command.glyph_offset);
    auto const glyph_count = static_cast<std::size_t>(command.glyph_count);
    if (glyph_offset > bucket.glyph_vertices.size()
        || glyph_offset + glyph_count > bucket.glyph_vertices.size()) {
        return false;
    }

    auto const* glyphs = bucket.glyph_vertices.data() + glyph_offset;
    auto const row_stride = static_cast<std::size_t>(width) * 4u;
    auto const uses_color_atlas =
        (command.flags & Scene::kTextGlyphsFlagUsesColorAtlas) != 0u;

    bool drawn = false;
    for (std::size_t index = 0; index < glyph_count; ++index) {
        auto const& glyph = glyphs[index];
        auto glyph_min_x = std::min(glyph.min_x, glyph.max_x);
        auto glyph_max_x = std::max(glyph.min_x, glyph.max_x);
        auto glyph_min_y = std::min(glyph.min_y, glyph.max_y);
        auto glyph_max_y = std::max(glyph.min_y, glyph.max_y);

        auto width_f = glyph_max_x - glyph_min_x;
        auto height_f = glyph_max_y - glyph_min_y;
        if (width_f <= 0.0f || height_f <= 0.0f) {
            continue;
        }

        auto min_x_i = std::clamp(static_cast<int>(std::floor(glyph_min_x)), 0, width);
        auto max_x_i = std::clamp(static_cast<int>(std::ceil(glyph_max_x)), 0, width);
        auto min_y_i = std::clamp(static_cast<int>(std::floor(glyph_min_y)), 0, height);
        auto max_y_i = std::clamp(static_cast<int>(std::ceil(glyph_max_y)), 0, height);
        if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
            continue;
        }

        auto u_range = glyph.u1 - glyph.u0;
        auto v_range = glyph.v1 - glyph.v0;
        if (std::fabs(u_range) <= std::numeric_limits<float>::epsilon()
            || std::fabs(v_range) <= std::numeric_limits<float>::epsilon()) {
            continue;
        }

        for (int y = min_y_i; y < max_y_i; ++y) {
            auto base_index = static_cast<std::size_t>(y) * row_stride;
            for (int x = min_x_i; x < max_x_i; ++x) {
                float local_x = (static_cast<float>(x) + 0.5f - glyph_min_x) / width_f;
                float local_y = (static_cast<float>(y) + 0.5f - glyph_min_y) / height_f;
                auto atlas_u = glyph.u0 + u_range * clamp_unit(local_x);
                auto atlas_v = glyph.v0 + v_range * clamp_unit(local_y);

                LinearPremulColor src{};
                if (uses_color_atlas && atlas.format == FontAtlasFormat::Rgba8) {
                    src = sample_font_atlas_rgba(atlas, atlas_u, atlas_v);
                    if (src.a == 0.0f) {
                        continue;
                    }
                    src.r = clamp_unit(src.r * tint_straight.r);
                    src.g = clamp_unit(src.g * tint_straight.g);
                    src.b = clamp_unit(src.b * tint_straight.b);
                    src.a = clamp_unit(src.a * tint_straight.a);
                } else {
                    auto alpha = sample_font_atlas_alpha(atlas, atlas_u, atlas_v);
                    if (alpha <= 0.0f) {
                        continue;
                    }
                    src.r = clamp_unit(base_color.r * alpha);
                    src.g = clamp_unit(base_color.g * alpha);
                    src.b = clamp_unit(base_color.b * alpha);
                    src.a = clamp_unit(base_color.a * alpha);
                }

                auto* dest = buffer.data() + base_index + static_cast<std::size_t>(x) * 4u;
                blend_pixel(dest, src);
                drawn = true;
            }
        }
    }
    return drawn;
}

auto draw_text_glyphs_command(Scene::TextGlyphsCommand const& command,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    auto color = premultiply(make_linear_straight(command.color));
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);
    return draw_rect_area(min_x, min_y, max_x, max_y, color, buffer, width, height);
}

auto draw_path_command(Scene::PathCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    auto color = premultiply(make_linear_straight(command.fill_color));
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);
    return draw_rect_area(min_x, min_y, max_x, max_y, color, buffer, width, height);
}

auto draw_mesh_command(Scene::MeshCommand const& command,
                       Scene::DrawableBucketSnapshot const& bucket,
                       std::size_t drawable_index,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return false;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    auto color = premultiply(make_linear_straight(command.color));
    return draw_rect_area(box.min[0], box.min[1], box.max[0], box.max[1], color, buffer, width, height);
}

auto draw_stroke_command(Scene::StrokeCommand const& command,
                         Scene::DrawableBucketSnapshot const& bucket,
                         std::vector<float>& buffer,
                         int width,
                         int height) -> bool {
    auto offset = static_cast<std::size_t>(command.point_offset);
    auto count = static_cast<std::size_t>(command.point_count);
    if (count == 0 || offset + count > bucket.stroke_points.size()) {
        return false;
    }
    auto half_width = std::max(command.thickness * 0.5f, 0.0f);
    if (half_width <= 0.0f) {
        half_width = 0.5f;
    }
    auto color = make_linear_color(command.color);
    auto const* points = bucket.stroke_points.data() + offset;

    bool drawn = false;
    if (count == 1) {
        drawn |= draw_disc(points[0].x, points[0].y, half_width, color, buffer, width, height);
        return drawn;
    }

    for (std::size_t index = 0; index + 1 < count; ++index) {
        auto const& a = points[index];
        auto const& b = points[index + 1];
        drawn |= draw_stroke_segment(a, b, half_width, color, buffer, width, height);
    }

    drawn |= draw_disc(points[0].x, points[0].y, half_width, color, buffer, width, height);
    drawn |= draw_disc(points[count - 1].x, points[count - 1].y, half_width, color, buffer, width, height);
    return drawn;
}

auto draw_image_command(Scene::ImageCommand const& command,
                        ImageCache::ImageData const& image,
                        LinearStraightColor const& tint,
                        std::vector<float>& buffer,
                        int width,
                        int height) -> bool {
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);

    auto width_f = std::max(0.0f, max_x - min_x);
    auto height_f = std::max(0.0f, max_y - min_y);
    if (width_f <= 0.0f || height_f <= 0.0f) {
        return false;
    }

    auto clamp_pixel = [](float value, int limit, bool ceiling) -> int {
        auto pixel = ceiling ? std::ceil(value) : std::floor(value);
        return std::clamp(static_cast<int>(pixel), 0, limit);
    };

    auto min_x_i = clamp_pixel(min_x, width, false);
    auto max_x_i = clamp_pixel(max_x, width, true);
    auto min_y_i = clamp_pixel(min_y, height, false);
    auto max_y_i = clamp_pixel(max_y, height, true);

    if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
        return false;
    }

    auto uv_width = command.uv_max_x - command.uv_min_x;
    auto uv_height = command.uv_max_y - command.uv_min_y;
    if (uv_width == 0.0f || uv_height == 0.0f) {
        return false;
    }

    bool drawn = false;
    auto const row_stride = static_cast<std::size_t>(width) * 4u;

    for (int y = min_y_i; y < max_y_i; ++y) {
        auto py = static_cast<float>(y) + 0.5f;
        auto local_v = (py - min_y) / height_f;
        auto v = command.uv_min_y + uv_height * local_v;

        for (int x = min_x_i; x < max_x_i; ++x) {
            auto px = static_cast<float>(x) + 0.5f;
            auto local_u = (px - min_x) / width_f;
            auto u = command.uv_min_x + uv_width * local_u;

            auto sampled = sample_image_linear(image, u, v);
            auto tinted = multiply_straight(sampled, tint);
            auto premul = premultiply(tinted);
            if (premul.a <= 0.0f) {
                continue;
            }

            auto base = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 4u;
            auto* dest = buffer.data() + base;
            blend_pixel(dest, premul);
            drawn = true;
        }
    }

    return drawn;
}

auto draw_fallback_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                              std::size_t drawable_index,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return false;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    auto color = make_linear_color(color_from_drawable(bucket.drawable_ids[drawable_index]));
    return draw_rect_area(box.min[0],
                          box.min[1],
                          box.max[0],
                          box.max[1],
                          color,
                          buffer,
                          width,
                          height);
}

} // namespace SP::UI::PathRenderer2DDetail
