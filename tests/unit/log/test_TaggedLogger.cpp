#include "third_party/doctest.h"
#include "log/TaggedLogger.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

class EnvGuard {
public:
    EnvGuard(std::string key, const char* value) : key(std::move(key)) {
        if (const char* existing = std::getenv(this->key.c_str())) {
            original = std::string(existing);
        }
        apply(value);
    }

    EnvGuard(const EnvGuard&)            = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

    EnvGuard(EnvGuard&& other) noexcept { *this = std::move(other); }

    EnvGuard& operator=(EnvGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        restore();
        key      = std::move(other.key);
        original = std::move(other.original);
        active   = other.active;
        other.active = false;
        return *this;
    }

    ~EnvGuard() {
        restore();
    }

private:
    void apply(const char* value) {
        if (value) {
            setenv(this->key.c_str(), value, 1);
        } else {
            unsetenv(this->key.c_str());
        }
    }

    void restore() {
        if (!active) {
            return;
        }
        if (original) {
            setenv(key.c_str(), original->c_str(), 1);
        } else {
            unsetenv(key.c_str());
        }
        active = false;
    }

    std::string              key;
    std::optional<std::string> original;
    bool                     active{true};
};

class EnvBlock {
public:
    EnvBlock(std::initializer_list<std::pair<std::string, const char*>> vars) {
        guards.reserve(vars.size());
        for (auto const& [name, value] : vars) {
            guards.emplace_back(name, value);
        }
    }

private:
    std::vector<EnvGuard> guards;
};

auto captureStderr(std::function<void()> fn) -> std::string {
    std::ostringstream buffer;
    auto*               original = std::cerr.rdbuf(buffer.rdbuf());
    fn();
    std::cerr.rdbuf(original);
    return buffer.str();
}

void waitForFlush() {
    std::this_thread::sleep_for(20ms);
}

auto makeBaselineEnvBlock() -> EnvBlock {
    return EnvBlock{
        {"PATHSPACE_LOG_ENABLED", nullptr},
        {"PATHSPACE_LOG", nullptr},
        {"PATHSPACE_LOG_CLEAR_DEFAULT_SKIPS", nullptr},
        {"PATHSPACE_LOG_ENABLE_TAGS", nullptr},
        {"PATHSPACE_LOG_SKIP_TAGS", nullptr},
    };
}

} // namespace

TEST_SUITE("log.tagged_logger") {

TEST_CASE("logging_disabled_by_default_drops_messages") {
    auto env = makeBaselineEnvBlock();

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("should not appear", std::source_location::current(), "TestTag");
        waitForFlush();
    });

    CHECK(output.empty());
}

TEST_CASE("environment_flag_enables_logging") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("hello log", std::source_location::current(), "TestTag");
        waitForFlush();
    });

    CHECK_FALSE(output.empty());
    CHECK(output.find("[TestTag]") != std::string::npos);
    CHECK(output.find("hello log") != std::string::npos);
    CHECK(output.find("Thread 0") != std::string::npos);
}

TEST_CASE("PATHSPACE_LOG_env_enables_logging") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG", "on");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("env enabled", std::source_location::current(), "EnvTag");
        waitForFlush();
    });

    CHECK(output.find("env enabled") != std::string::npos);
    CHECK(output.find("EnvTag") != std::string::npos);
}

TEST_CASE("default_skip_list_filters_info_tag") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto skipped = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("filtered", std::source_location::current(), "INFO");
        waitForFlush();
    });

    CHECK(skipped.empty());
}

TEST_CASE("clear_default_skips_allows_info") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");
    EnvGuard clearSkips("PATHSPACE_LOG_CLEAR_DEFAULT_SKIPS", "1");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("info allowed", std::source_location::current(), "INFO");
        waitForFlush();
    });

    CHECK(output.find("info allowed") != std::string::npos);
}

TEST_CASE("enabled_tags_gate_output") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");
    EnvGuard enableTags("PATHSPACE_LOG_ENABLE_TAGS", "Focus");

    auto accepted = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("keep me", std::source_location::current(), "Focus");
        waitForFlush();
    });
    CHECK(accepted.find("keep me") != std::string::npos);

    auto rejected = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("drop me", std::source_location::current(), "Focus", "Other");
        waitForFlush();
    });
    CHECK(rejected.empty());
}

TEST_CASE("custom_skip_tags_extend_filter") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");
    EnvGuard extraSkip("PATHSPACE_LOG_SKIP_TAGS", "Noisy");

    auto skipped = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("not expected", std::source_location::current(), "Noisy");
        waitForFlush();
    });
    CHECK(skipped.empty());

    auto passed = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("expected", std::source_location::current(), "Quiet");
        waitForFlush();
    });
    CHECK(passed.find("expected") != std::string::npos);
}

TEST_CASE("skip_tag_parsing_trims_tokens") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");
    EnvGuard clearSkips("PATHSPACE_LOG_CLEAR_DEFAULT_SKIPS", "1");
    EnvGuard extraSkip("PATHSPACE_LOG_SKIP_TAGS", " noisy , extra ");

    auto skipped = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("first", std::source_location::current(), "extra");
        waitForFlush();
    });
    CHECK(skipped.empty());

    auto kept = captureStderr([] {
        SP::TaggedLogger logger;
        logger.log_impl("second", std::source_location::current(), "clean");
        waitForFlush();
    });
    CHECK(kept.find("second") != std::string::npos);
}

TEST_CASE("thread_name_is_used_in_output") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
        logger.setThreadName("Worker-7");
        logger.log_impl("with name", std::source_location::current(), "Test");
        waitForFlush();
    });

    CHECK(output.find("[Worker-7]") != std::string::npos);
}

TEST_CASE("set_logging_enabled_overrides_env") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto suppressed = captureStderr([] {
        SP::TaggedLogger logger;
        logger.setLoggingEnabled(false);
        logger.log_impl("disabled", std::source_location::current(), "Test");
        waitForFlush();
    });
    CHECK(suppressed.empty());

    auto enabled = captureStderr([] {
        SP::TaggedLogger logger;
        logger.setLoggingEnabled(true);
        logger.log_impl("enabled", std::source_location::current(), "Test");
        waitForFlush();
    });
    CHECK(enabled.find("enabled") != std::string::npos);
}

TEST_CASE("global_wrappers_and_macro_emit_joined_tags") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto output = captureStderr([] {
        SP::set_thread_name("WrapperThread");
        SP::set_logging_enabled(true);
        sp_log("via macro", "Alpha", "Beta");
        waitForFlush();
    });

    CHECK(output.find("Alpha][Beta") != std::string::npos);
    CHECK(output.find("[WrapperThread]") != std::string::npos);
}

TEST_CASE("short_path_handles_file_without_parent_directory") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
#line 500 "TaggedLoggerNoParent.cpp"
        logger.log_impl("no parent path", std::source_location::current(), "Solo");
#line 1 "tests/unit/log/test_TaggedLogger.cpp"
        waitForFlush();
    });

    CHECK(output.find("TaggedLoggerNoParent.cpp:500") != std::string::npos);
}

TEST_CASE("short_path_includes_parent_directory") {
    auto env = makeBaselineEnvBlock();
    EnvGuard enableLog("PATHSPACE_LOG_ENABLED", "1");

    auto output = captureStderr([] {
        SP::TaggedLogger logger;
#line 42 "dir/subdir/TaggedLoggerChild.cpp"
        logger.log_impl("has parent", std::source_location::current(), "Solo");
#line 1 "tests/unit/log/test_TaggedLogger.cpp"
        waitForFlush();
    });

    CHECK(output.find("subdir/TaggedLoggerChild.cpp:42") != std::string::npos);
}

} // TEST_SUITE
