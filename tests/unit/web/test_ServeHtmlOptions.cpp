#include "third_party/doctest.h"
#include "pathspace/web/ServeHtmlOptions.hpp"

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* key, const char* value)
        : key_(key) {
        if (const char* existing = std::getenv(key)) {
            original_ = std::string{existing};
        }
        if (value != nullptr) {
            setenv(key, value, 1);
        } else {
            unsetenv(key);
        }
    }

    ~EnvGuard() {
        if (original_.has_value()) {
            setenv(key_.c_str(), original_->c_str(), 1);
        } else {
            unsetenv(key_.c_str());
        }
    }

    std::string                key_;
    std::optional<std::string> original_;
};

struct ArgvBuilder {
    explicit ArgvBuilder(std::initializer_list<const char*> args) {
        storage.reserve(args.size());
        for (auto value : args) {
            storage.emplace_back(value);
        }
        pointers.reserve(storage.size());
        for (auto& entry : storage) {
            pointers.push_back(entry.data());
        }
    }

    auto argc() const -> int { return static_cast<int>(pointers.size()); }
    auto argv() -> char** { return pointers.data(); }

    std::vector<std::string> storage;
    std::vector<char*>       pointers;
};

} // namespace

TEST_CASE("ServeHtmlOptions validation helpers guard ranges") {
    CHECK(SP::ServeHtml::IsValidServeHtmlPort(80));
    CHECK_FALSE(SP::ServeHtml::IsValidServeHtmlPort(0));
    CHECK(SP::ServeHtml::IsValidServeHtmlRenderer("html"));
    CHECK_FALSE(SP::ServeHtml::IsValidServeHtmlRenderer("bad/name"));
}

TEST_CASE("ServeHtmlOptions Validate detects invalid combinations") {
    SP::ServeHtml::ServeHtmlOptions options{};
    options.port = 70000;
    auto error = SP::ServeHtml::ValidateServeHtmlOptions(options);
    REQUIRE(error.has_value());
    CHECK(error->find("--port") != std::string::npos);

    options.port = 8080;
    options.renderer = "bad/name";
    error = SP::ServeHtml::ValidateServeHtmlOptions(options);
    REQUIRE(error.has_value());
    CHECK(error->find("--renderer") != std::string::npos);
}

TEST_CASE("Environment overrides apply to CLI defaults") {
    EnvGuard host{"PATHSPACE_SERVE_HTML_HOST", "0.0.0.0"};
    EnvGuard port{"PATHSPACE_SERVE_HTML_PORT", "9090"};

    ArgvBuilder argv{"pathspace_serve_html"};
    auto        parsed = SP::ServeHtml::ParseServeHtmlArguments(argv.argc(), argv.argv());

    REQUIRE(parsed.has_value());
    CHECK(parsed->host == "0.0.0.0");
    CHECK(parsed->port == 9090);
}

TEST_CASE("Invalid environment override fails early") {
    EnvGuard port{"PATHSPACE_SERVE_HTML_PORT", "70000"};

    ArgvBuilder argv{"pathspace_serve_html"};
    auto        parsed = SP::ServeHtml::ParseServeHtmlArguments(argv.argc(), argv.argv());

    CHECK_FALSE(parsed.has_value());
}
