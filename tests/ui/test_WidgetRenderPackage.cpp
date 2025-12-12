#include "third_party/doctest.h"

#include <cstddef>

#include <pathspace/PathSpace.hpp>
#include <pathspace/task/TaskPool.hpp>
#include <pathspace/type/SlidingBuffer.hpp>
#include <pathspace/ui/declarative/WidgetRenderPackage.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>

using SP::UI::Declarative::WidgetRenderPackage;
using SP::UI::Declarative::WidgetSurface;
using SP::UI::Declarative::WidgetSurfaceFlags;
using SP::UI::Declarative::WidgetSurfaceKind;

namespace {

auto make_sample_package() -> WidgetRenderPackage {
    WidgetRenderPackage package{};
    package.capsule_revision = 42;
    package.render_sequence = 7;
    package.content_hash = 0xDEADBEEFu;
    package.dirty_rect = SP::UI::Runtime::MakeDirtyRectHint(1.0f, 2.0f, 10.0f, 20.0f);
    package.command_kinds = {1u, 3u, 5u};
    package.command_payload = {0xAA, 0xBB, 0xCC, 0xDD};
    package.texture_fingerprints = {100u, 200u, 300u};

    WidgetSurface surface{};
    surface.kind = WidgetSurfaceKind::Software;
    surface.flags = WidgetSurfaceFlags::Opaque | WidgetSurfaceFlags::StretchToFit;
    surface.width = 640;
    surface.height = 480;
    surface.fingerprint = 999;
    surface.logical_bounds = {0.0f, 0.0f, 640.0f, 480.0f};
    package.surfaces.push_back(surface);

    WidgetSurface overlay{};
    overlay.kind = WidgetSurfaceKind::External;
    overlay.flags = WidgetSurfaceFlags::AlphaPremultiplied;
    overlay.width = 320;
    overlay.height = 200;
    overlay.fingerprint = 1234;
    overlay.logical_bounds = {5.0f, 6.0f, 50.0f, 60.0f};
    package.surfaces.push_back(overlay);

    return package;
}

} // namespace

TEST_CASE("WidgetRenderPackage serialize/deserialize round trip") {
    auto original = make_sample_package();

    SP::SlidingBuffer buffer;
    auto status = SP::serialize(original, buffer);
    REQUIRE_FALSE(status.has_value());

    auto decoded = SP::deserialize<WidgetRenderPackage>(buffer);
    REQUIRE(decoded);

    CHECK(decoded->capsule_revision == original.capsule_revision);
    CHECK(decoded->render_sequence == original.render_sequence);
    CHECK(decoded->content_hash == original.content_hash);
    CHECK(decoded->command_kinds == original.command_kinds);
    CHECK(decoded->command_payload == original.command_payload);
    CHECK(decoded->texture_fingerprints == original.texture_fingerprints);
    REQUIRE(decoded->surfaces.size() == original.surfaces.size());
    for (std::size_t i = 0; i < original.surfaces.size(); ++i) {
        CHECK(decoded->surfaces[i].kind == original.surfaces[i].kind);
        CHECK(decoded->surfaces[i].flags == original.surfaces[i].flags);
        CHECK(decoded->surfaces[i].width == original.surfaces[i].width);
        CHECK(decoded->surfaces[i].height == original.surfaces[i].height);
        CHECK(decoded->surfaces[i].fingerprint == original.surfaces[i].fingerprint);
        CHECK(decoded->surfaces[i].logical_bounds == original.surfaces[i].logical_bounds);
    }
    CHECK(decoded->dirty_rect.min_x == doctest::Approx(original.dirty_rect.min_x));
    CHECK(decoded->dirty_rect.max_y == doctest::Approx(original.dirty_rect.max_y));
}

TEST_CASE("WidgetRenderPackage round trips through PathSpace") {
    SP::TaskPool pool;
    SP::PathSpace space{&pool};

    auto package = make_sample_package();

    auto insert_result = space.insert("/widgets/example/render/package", package);
    REQUIRE(insert_result.errors.empty());

    auto read_back = space.read<WidgetRenderPackage>("/widgets/example/render/package");
    REQUIRE(read_back);

    CHECK(read_back->command_payload == package.command_payload);
    CHECK(read_back->surfaces.size() == package.surfaces.size());
    CHECK(read_back->texture_fingerprints.back() == package.texture_fingerprints.back());
}
