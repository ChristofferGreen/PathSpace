#include "third_party/doctest.h"

#include <pathspace/examples/cli/ExampleCli.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
auto make_argv(std::initializer_list<const char*> list) {
    std::vector<char*> argv;
    argv.reserve(list.size());
    for (auto* value : list) {
        argv.push_back(const_cast<char*>(value));
    }
    return argv;
}
}

TEST_CASE("ExampleCli parses flags and integer options") {
    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("example_cli_test");

    bool headless = false;
    int width = 640;
    cli.add_flag("--headless", {.on_set = [&] { headless = true; }});
    cli.add_int("--width", {.on_value = [&](int value) { width = value; }});

    auto argv = make_argv({"prog", "--headless", "--width=1440"});
    CHECK(cli.parse(static_cast<int>(argv.size()), argv.data()));
    CHECK(headless);
    CHECK(width == 1440);
}

TEST_CASE("ExampleCli optional value skips next flag when missing") {
    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("example_cli_test_optional");

    bool gpu_smoke = false;
    std::optional<std::string> capture_path;

    ExampleCli::ValueOption gpu_option{};
    gpu_option.value_optional = true;
    gpu_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        gpu_smoke = true;
        if (value && !value->empty()) {
            capture_path = std::string(value->begin(), value->end());
        }
        return std::nullopt;
    };
    cli.add_value("--gpu-smoke", gpu_option);
    cli.add_int("--width", {.on_value = [&](int v) { capture_path.reset(); (void)v; }});

    auto argv = make_argv({"prog", "--gpu-smoke", "--width=1400"});
    CHECK(cli.parse(static_cast<int>(argv.size()), argv.data()));
    CHECK(gpu_smoke);
    CHECK(!capture_path.has_value());
}

TEST_CASE("ExampleCli optional value consumes explicit token") {
    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;

    bool gpu_smoke = false;
    std::optional<std::string> capture_path;

    ExampleCli::ValueOption gpu_option{};
    gpu_option.value_optional = true;
    gpu_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        gpu_smoke = true;
        if (value && !value->empty()) {
            capture_path = std::string(value->begin(), value->end());
        } else {
            capture_path.reset();
        }
        return std::nullopt;
    };
    cli.add_value("--gpu-smoke", gpu_option);

    auto argv = make_argv({"prog", "--gpu-smoke", "capture.png"});
    CHECK(cli.parse(static_cast<int>(argv.size()), argv.data()));
    CHECK(gpu_smoke);
    REQUIRE(capture_path.has_value());
    CHECK(*capture_path == "capture.png");
}

TEST_CASE("ExampleCli missing required value fails parse") {
    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("example_cli_fail");

    cli.add_int("--width", {.on_value = [](int) {}});

    auto argv = make_argv({"prog", "--width"});
    CHECK_FALSE(cli.parse(static_cast<int>(argv.size()), argv.data()));
}

TEST_CASE("ExampleCli unknown handler can mark failures") {
    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    bool handler_called = false;
    cli.set_unknown_argument_handler([&](std::string_view) {
        handler_called = true;
        return false;
    });

    auto argv = make_argv({"prog", "--mystery"});
    CHECK_FALSE(cli.parse(static_cast<int>(argv.size()), argv.data()));
    CHECK(handler_called);
}
