#include "third_party/doctest.h"

#include <pathspace/ui/scenegraph/SoftwareTileRenderer.hpp>

#include <array>
#include <vector>

using namespace SP::UI::SceneGraph;
namespace Scene = SP::UI::Scene;
using SP::UI::PathSurfaceSoftware;
using SP::UI::Runtime::SurfaceDesc;
using SP::UI::Runtime::PixelFormat;
using SP::UI::Runtime::ColorSpace;

namespace {

auto read_pixel(PathSurfaceSoftware& surface, int x, int y) -> std::array<std::uint8_t, 4> {
    auto span = surface.staging_span();
    auto stride = surface.row_stride_bytes();
    auto offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    return {span[offset + 0], span[offset + 1], span[offset + 2], span[offset + 3]};
}

auto make_surface(int width, int height) -> PathSurfaceSoftware {
    SurfaceDesc desc{};
    desc.size_px.width = width;
    desc.size_px.height = height;
    desc.pixel_format = PixelFormat::RGBA8Unorm;
    desc.color_space = ColorSpace::sRGB;
    return PathSurfaceSoftware(desc);
}

} // namespace

TEST_SUITE("ui.scenegraph.render.tile.renderer.software") {
    TEST_CASE("renders_rect_commands_into_tiles") {
        auto surface = make_surface(8, 4);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 4, .tile_height = 2});

        std::vector<Scene::RectCommand> rects{
            Scene::RectCommand{.min_x = 0.0f, .min_y = 0.0f, .max_x = 4.0f, .max_y = 2.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
            Scene::RectCommand{.min_x = 4.0f, .min_y = 0.0f, .max_x = 8.0f, .max_y = 2.0f, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
        };

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 4, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 1,
        });
        store.upsert(CommandDescriptor{
            .bbox = IntRect{4, 0, 8, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 1,
            .entity_id = 2,
        });

        SpanPayloadProvider payloads;
        payloads.rects = rects;

        auto stats = renderer.render(store, payloads);

        CHECK(stats.tiles_total == 4);
        CHECK(stats.tiles_dirty == 2);
        CHECK(stats.commands_rendered == 2);
        CHECK(stats.tiles_rendered == 2);
        CHECK(stats.tile_jobs == 2);
        CHECK(stats.workers_used >= 1);

        auto red = read_pixel(surface, 0, 0);
        auto green = read_pixel(surface, 4, 0);
        auto clear = read_pixel(surface, 0, 3);

        CHECK(red[0] == 255);
        CHECK(red[1] == 0);
        CHECK(red[2] == 0);
        CHECK(red[3] == 255);

        CHECK(green[0] == 0);
        CHECK(green[1] == 255);
        CHECK(green[2] == 0);
        CHECK(green[3] == 255);

        CHECK(clear[0] == 0);
        CHECK(clear[1] == 0);
        CHECK(clear[2] == 0);
        CHECK(clear[3] == 0);
    }

    TEST_CASE("respects_z_order_in_overlapping_tiles") {
        auto surface = make_surface(4, 4);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 2, .tile_height = 2});

        std::vector<Scene::RectCommand> rects{
            Scene::RectCommand{.min_x = 0.0f, .min_y = 0.0f, .max_x = 4.0f, .max_y = 4.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
            Scene::RectCommand{.min_x = 1.0f, .min_y = 1.0f, .max_x = 3.0f, .max_y = 3.0f, .color = {0.0f, 0.0f, 1.0f, 1.0f}},
        };

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 4, 4},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 10,
        });
        store.upsert(CommandDescriptor{
            .bbox = IntRect{1, 1, 3, 3},
            .z = 1,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 1,
            .entity_id = 11,
        });

        SpanPayloadProvider payloads;
        payloads.rects = rects;

        renderer.render(store, payloads);

        auto center = read_pixel(surface, 2, 2);
        CHECK(center[0] == 0);
        CHECK(center[1] == 0);
        CHECK(center[2] == 255);
        CHECK(center[3] == 255);
    }

    TEST_CASE("honors_max_workers_cap") {
        auto surface = make_surface(4, 4);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{
                                                          .tile_width = 2,
                                                          .tile_height = 2,
                                                          .max_bucket_size = 8,
                                                          .max_workers = 1});

        std::vector<Scene::RectCommand> rects{
            Scene::RectCommand{.min_x = 0.0f, .min_y = 0.0f, .max_x = 2.0f, .max_y = 2.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
            Scene::RectCommand{.min_x = 2.0f, .min_y = 0.0f, .max_x = 4.0f, .max_y = 2.0f, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
        };

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 30,
        });
        store.upsert(CommandDescriptor{
            .bbox = IntRect{2, 0, 4, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 1,
            .entity_id = 31,
        });

        SpanPayloadProvider payloads;
        payloads.rects = rects;

        auto stats = renderer.render(store, payloads);

        CHECK(stats.tile_jobs == 2);
        CHECK(stats.workers_used == 1);
        CHECK(stats.tiles_rendered == 2);
    }

    TEST_CASE("draws_text_command_via_bbox_fill") {
        auto surface = make_surface(4, 2);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 2, .tile_height = 2});

        std::vector<Scene::TextGlyphsCommand> texts{
            Scene::TextGlyphsCommand{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = 2.0f,
                .max_y = 2.0f,
                .glyph_offset = 0,
                .glyph_count = 1,
                .atlas_fingerprint = 42,
                .font_size = 12.0f,
                .em_size = 12.0f,
                .px_range = 1.0f,
                .flags = 0,
                .color = {0.0f, 0.0f, 1.0f, 1.0f},
            },
        };

        std::vector<Scene::TextGlyphVertex> glyphs{
            Scene::TextGlyphVertex{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = 2.0f,
                .max_y = 2.0f,
                .u0 = 0.0f,
                .v0 = 0.0f,
                .u1 = 1.0f,
                .v1 = 1.0f,
            },
        };

        auto atlas = std::make_shared<SP::UI::FontAtlasData>();
        atlas->width = 1;
        atlas->height = 1;
        atlas->format = SP::UI::FontAtlasFormat::Alpha8;
        atlas->bytes_per_pixel = 1;
        atlas->pixels = {255};

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::TextGlyphs,
            .payload_handle = 0,
            .entity_id = 20,
        });

        SpanPayloadProvider payloads;
        payloads.texts = texts;
        payloads.glyphs = glyphs;
        payloads.atlases.emplace(42, atlas);

        renderer.render(store, payloads);

        auto pixel = read_pixel(surface, 1, 1);
        CHECK(pixel[0] == 0);
        CHECK(pixel[1] == 0);
        CHECK(pixel[2] == 255);
        CHECK(pixel[3] == 255);
    }

    TEST_CASE("text_tiles_do_not_double_blend_across_tiles") {
        auto surface = make_surface(4, 2);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 2, .tile_height = 2});

        std::vector<Scene::TextGlyphsCommand> texts{
            Scene::TextGlyphsCommand{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = 4.0f,
                .max_y = 2.0f,
                .glyph_offset = 0,
                .glyph_count = 1,
                .atlas_fingerprint = 43,
                .font_size = 12.0f,
                .em_size = 12.0f,
                .px_range = 1.0f,
                .flags = 0,
                .color = {1.0f, 0.0f, 0.0f, 0.5f},
            },
        };

        std::vector<Scene::TextGlyphVertex> glyphs{
            Scene::TextGlyphVertex{
                .min_x = 0.0f,
                .min_y = 0.0f,
                .max_x = 4.0f,
                .max_y = 2.0f,
                .u0 = 0.0f,
                .v0 = 0.0f,
                .u1 = 1.0f,
                .v1 = 1.0f,
            },
        };

        auto atlas = std::make_shared<SP::UI::FontAtlasData>();
        atlas->width = 1;
        atlas->height = 1;
        atlas->format = SP::UI::FontAtlasFormat::Alpha8;
        atlas->bytes_per_pixel = 1;
        atlas->pixels = {255};

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 4, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::TextGlyphs,
            .payload_handle = 0,
            .entity_id = 21,
        });

        SpanPayloadProvider payloads;
        payloads.texts = texts;
        payloads.glyphs = glyphs;
        payloads.atlases.emplace(43, atlas);

        renderer.render(store, payloads);

        // Pixel lies in the left tile; with correct per-tile clipping alpha should be ~0.5.
        auto pixel = read_pixel(surface, 1, 1);
        CHECK(pixel[0] >= 127);
        CHECK(pixel[0] <= 128);
        CHECK(pixel[1] == 0);
        CHECK(pixel[2] == 0);
        CHECK(pixel[3] >= 127);
        CHECK(pixel[3] <= 128);
    }

    TEST_CASE("dirty_overrides_reuse_previous_tiles") {
        auto surface = make_surface(4, 2);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 2, .tile_height = 2});

        std::vector<Scene::RectCommand> rects{
            Scene::RectCommand{.min_x = 0.0f, .min_y = 0.0f, .max_x = 2.0f, .max_y = 2.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
            Scene::RectCommand{.min_x = 2.0f, .min_y = 0.0f, .max_x = 4.0f, .max_y = 2.0f, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
        };

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 101,
        });
        store.upsert(CommandDescriptor{
            .bbox = IntRect{2, 0, 4, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 1,
            .entity_id = 102,
        });

        SpanPayloadProvider payloads;
        payloads.rects = rects;

        renderer.render(store, payloads);

        // Update only the left rect and render with a dirty override for that tile.
        rects[0].color = {0.0f, 0.0f, 1.0f, 1.0f};
        payloads.rects = rects;

        std::vector<IntRect> dirty{{0, 0, 2, 2}};
        renderer.render(store, payloads, dirty);

        auto left = read_pixel(surface, 0, 0);
        auto right = read_pixel(surface, 3, 0);

        CHECK(left[2] == 255);  // blue
        CHECK(left[0] == 0);
        CHECK(right[1] == 255); // green should persist without redraw
        CHECK(right[0] == 0);
    }

    TEST_CASE("invokes_tile_encoder_hooks_with_command_views") {
        struct RecordingHooks final : TileEncoderHooks {
            bool begin_called = false;
            bool end_called = false;
            TileRenderFrameInfo frame{};
            std::vector<IntRect> tiles;
            std::vector<std::vector<TileRenderCommandView>> commands;
            SoftwareTileRenderStats end_stats{};

            auto begin_frame(TileRenderFrameInfo const& info,
                             SoftwareTileRendererPayloads const&) -> void override {
                begin_called = true;
                frame = info;
            }

            auto encode_tile(TileRenderSubmission const& submission,
                             SoftwareTileRendererPayloads const&) -> void override {
                tiles.push_back(submission.tile_rect);
                commands.emplace_back(submission.commands.begin(), submission.commands.end());
            }

            auto end_frame(SoftwareTileRenderStats const& stats,
                           SoftwareTileRendererPayloads const&) -> void override {
                end_called = true;
                end_stats = stats;
            }
        };

        auto surface = make_surface(2, 2);
        SoftwareTileRenderer renderer(surface, SoftwareTileRendererConfig{.tile_width = 2, .tile_height = 2});

        std::vector<Scene::RectCommand> rects{
            Scene::RectCommand{.min_x = 0.0f, .min_y = 0.0f, .max_x = 2.0f, .max_y = 2.0f, .color = {0.5f, 0.25f, 0.75f, 1.0f}},
        };

        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 50,
        });

        SpanPayloadProvider payloads;
        payloads.rects = rects;

        RecordingHooks hooks;
        PathSurfaceSoftware::FrameInfo frame_info{
            .frame_index = 5,
            .revision = 9,
        };

        auto stats = renderer.render(store, payloads, {}, frame_info, &hooks);

        CHECK(hooks.begin_called);
        CHECK(hooks.end_called);
        CHECK(hooks.frame.surface_width == 2);
        CHECK(hooks.frame.surface_height == 2);
        CHECK(hooks.frame.tile_width == 2);
        CHECK(hooks.frame.tile_height == 2);
        CHECK(hooks.frame.frame_index == 5);
        CHECK(hooks.frame.revision == 9);
        REQUIRE(hooks.tiles.size() == 1);
        CHECK(hooks.tiles.front().min_x == 0);
        CHECK(hooks.tiles.front().max_x == 2);
        REQUIRE(hooks.commands.size() == 1);
        REQUIRE(hooks.commands.front().size() == 1);
        auto const& recorded = hooks.commands.front().front();
        CHECK(recorded.kind == Scene::DrawCommandKind::Rect);
        CHECK(recorded.entity_id == 50);
        CHECK(recorded.payload_handle == 0);
        CHECK(recorded.z == 0);
        CHECK(recorded.opacity == doctest::Approx(1.0f));
        CHECK(recorded.bbox.min_x == 0);
        CHECK(recorded.bbox.max_x == 2);
        CHECK(stats.commands_rendered == hooks.end_stats.commands_rendered);
    }
}
