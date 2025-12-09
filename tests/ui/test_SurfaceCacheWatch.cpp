#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/RuntimeDetail.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace Runtime = SP::UI::Runtime;

namespace {

auto make_surface_desc() -> Runtime::SurfaceDesc {
    Runtime::SurfaceDesc desc;
    desc.size_px.width        = 64;
    desc.size_px.height       = 64;
    desc.pixel_format         = Runtime::PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space          = Runtime::ColorSpace::sRGB;
    desc.premultiplied_alpha  = true;
    desc.progressive_tile_px  = 8;
    desc.progressive_enabled  = true;
    desc.buffered_frame_count = 1;
    return desc;
}

auto wait_for_surface_cache_drop(std::string const& key,
                                 std::chrono::milliseconds timeout = std::chrono::milliseconds{200})
    -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(Runtime::Detail::surfaces_cache_mutex());
            if (Runtime::Detail::surfaces_cache().find(key) == Runtime::Detail::surfaces_cache().end()) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

struct WatchCleanupGuard {
    ~WatchCleanupGuard() {
        Runtime::Detail::shutdown_surface_cache_watches();
    }
};

} // namespace

TEST_SUITE("SurfaceCacheWatch") {

TEST_CASE("evicts cached surfaces when diagnostics path is removed") {
    WatchCleanupGuard guard;
    SP::PathSpace     space;
    std::string const target_key = "/system/tests/renderers/cacheWatch/targets/main";
    std::string const watch_path = target_key + "/diagnostics/cacheWatch";

    auto status = Runtime::Detail::ensure_surface_cache_watch(space, target_key);
    REQUIRE(status);

    auto& surface = Runtime::Detail::acquire_surface(target_key, make_surface_desc());
    (void)surface;

    {
        std::lock_guard<std::mutex> lock(Runtime::Detail::surfaces_cache_mutex());
        CHECK(Runtime::Detail::surfaces_cache().count(target_key) == 1);
    }

    auto removed = space.take<bool>(watch_path);
    REQUIRE(removed);
    CHECK(*removed);

    CHECK(wait_for_surface_cache_drop(target_key));

    Runtime::Detail::evict_surface_cache_entry(target_key);
}

TEST_CASE("watches can restart after shutdown") {
    WatchCleanupGuard guard;
    SP::PathSpace     space;
    std::string const target_key = "/system/tests/renderers/cacheWatch/targets/restart";
    std::string const watch_path = target_key + "/diagnostics/cacheWatch";

    auto status = Runtime::Detail::ensure_surface_cache_watch(space, target_key);
    REQUIRE(status);

    auto& surface = Runtime::Detail::acquire_surface(target_key, make_surface_desc());
    (void)surface;

    Runtime::Detail::shutdown_surface_cache_watches();

    auto removed = space.take<bool>(watch_path);
    REQUIRE(removed);
    CHECK(*removed);

    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    {
        std::lock_guard<std::mutex> lock(Runtime::Detail::surfaces_cache_mutex());
        CHECK(Runtime::Detail::surfaces_cache().count(target_key) == 1);
    }

    status = Runtime::Detail::ensure_surface_cache_watch(space, target_key);
    REQUIRE(status);

    auto removed_again = space.take<bool>(watch_path);
    REQUIRE(removed_again);
    CHECK(*removed_again);

    CHECK(wait_for_surface_cache_drop(target_key));

    Runtime::Detail::evict_surface_cache_entry(target_key);
}

} // TEST_SUITE
