#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/task/TaskPool.hpp>
#include <pathspace/ui/HtmlAdapter.hpp>

#include <vector>

using SP::PathSpace;
using SP::TaskPool;
using SP::UI::Html::Asset;

TEST_CASE("Html assets round-trip without HtmlSerialization include") {
    TaskPool pool;
    PathSpace space{&pool};

    std::vector<Asset> assets;
    Asset image{};
    image.logical_path = "images/example.png";
    image.mime_type = "image/png";
    image.bytes = {0x01, 0x02, 0x03, 0xFF};
    assets.emplace_back(image);

    auto insert_result = space.insert("/output/v1/html/assets", assets);
    REQUIRE(insert_result.errors.empty());

    auto read_result = space.read<std::vector<Asset>>("/output/v1/html/assets");
    REQUIRE(read_result);
    REQUIRE(read_result->size() == assets.size());
    CHECK((*read_result)[0].bytes == assets[0].bytes);
    CHECK((*read_result)[0].mime_type == assets[0].mime_type);
    CHECK((*read_result)[0].logical_path == assets[0].logical_path);
}
