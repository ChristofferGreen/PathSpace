#define DOCTEST_CONFIG_IMPLEMENT
#include "third_party/doctest.h"
#include "log/TaggedLogger.hpp"

#include <sys/resource.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> split_suite_filters(const char* raw) {
    std::vector<std::string> filters;
    if (!raw || *raw == '\0') {
        return filters;
    }
    std::string current;
    auto flush_current = [&]() {
        if (current.empty()) {
            return;
        }
        size_t start = 0;
        size_t end = current.size();
        while (start < end && std::isspace(static_cast<unsigned char>(current[start]))) {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(current[end - 1]))) {
            --end;
        }
        if (start < end) {
            filters.emplace_back(current.substr(start, end - start));
        }
        current.clear();
    };
    for (const char* p = raw; *p != '\0'; ++p) {
        if (*p == ',' || *p == ';') {
            flush_current();
        } else {
            current.push_back(*p);
        }
    }
    flush_current();
    return filters;
}

// Hang detection that does not depend on how busy the machine is.
//
// Wall-clock limits fail healthy tests on a loaded machine. The watchdog instead looks at
// counters of work actually done by this process:
//   * CPU budget: CPU seconds (user+system, all threads) one test case may consume.
//   * Stall: CPU seconds consumed in a sampling window. A deadlocked process consumes none;
//     a merely starved process still gets scheduled and consumes some.
// Both limits can be raised with PATHSPACE_TEST_CPU_BUDGET_S / PATHSPACE_TEST_STALL_CPU_MS
// (sanitizer builds multiply CPU cost). Setting PATHSPACE_TEST_CPU_BUDGET_S=0 disables them.
double processCpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto toSeconds = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6; };
    return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
}

double envSeconds(const char* name, double fallback) {
    if (const char* raw = std::getenv(name); raw && *raw != '\0') {
        return std::strtod(raw, nullptr);
    }
    return fallback;
}

class CpuWatchdog {
public:
    static CpuWatchdog& instance() {
        // Never destroyed: the detached sampler thread keeps locking `mutex` until the process
        // exits, so a function-local static would be torn down underneath it (and aborts the
        // run with "mutex lock failed" under ThreadSanitizer).
        static auto* const watchdog = new CpuWatchdog;
        return *watchdog;
    }

    void testCaseStarted(const char* name) {
        std::lock_guard<std::mutex> lock(mutex);
        current        = name;
        caseStartCpu   = processCpuSeconds();
        active         = true;
    }
    void testCaseEnded() {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
    }

private:
    CpuWatchdog() {
        budgetSeconds = envSeconds("PATHSPACE_TEST_CPU_BUDGET_S", 120.0);
        stallCpuMs    = envSeconds("PATHSPACE_TEST_STALL_CPU_MS", 5.0);
        stallWindow   = std::chrono::milliseconds(static_cast<long long>(envSeconds("PATHSPACE_TEST_STALL_WINDOW_S", 60.0) * 1000.0));
        if (budgetSeconds <= 0.0) {
            return;
        }
        std::thread([this] { run(); }).detach();
    }

    [[noreturn]] void trip(const std::string& current, const char* reason, double value) {
        std::fprintf(stderr, "\nWATCHDOG: test case \"%s\" %s (%.3f)\n", current.c_str(), reason, value);
        std::fflush(stderr);
        std::_Exit(124);
    }

    void run() {
        auto   windowStart    = std::chrono::steady_clock::now();
        double windowStartCpu = processCpuSeconds();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const double cpu = processCpuSeconds();
            std::string  name;
            bool         isActive;
            double       startCpu;
            {
                std::lock_guard<std::mutex> lock(mutex);
                name     = current;
                isActive = active;
                startCpu = caseStartCpu;
            }
            if (!isActive) {
                windowStart    = std::chrono::steady_clock::now();
                windowStartCpu = cpu;
                continue;
            }
            if (cpu - startCpu > budgetSeconds) {
                trip(name, "exceeded its CPU budget (CPU seconds)", cpu - startCpu);
            }
            if (std::chrono::steady_clock::now() - windowStart >= stallWindow) {
                const double usedMs = (cpu - windowStartCpu) * 1000.0;
                if (usedMs < stallCpuMs) {
                    trip(name, "made no progress: deadlock suspected (CPU ms in window)", usedMs);
                }
                windowStart    = std::chrono::steady_clock::now();
                windowStartCpu = cpu;
            }
        }
    }

    std::mutex                mutex;
    std::string               current;
    double                    caseStartCpu  = 0.0;
    bool                      active        = false;
    double                    budgetSeconds = 120.0;
    double                    stallCpuMs    = 5.0;
    std::chrono::milliseconds stallWindow{60000};
};

}  // namespace

TEST_CASE("split_suite_filters trims whitespace and treats commas/semicolons as separators") {
    auto filters = split_suite_filters("  alpha , beta;gamma ;  delta  ");
    std::vector<std::string> expected{"alpha", "beta", "gamma", "delta"};
    CHECK(filters == expected);

    auto empty = split_suite_filters(" , ;  ; ");
    CHECK(empty.empty());

    auto single = split_suite_filters(" solo ");
    CHECK(single.size() == 1);
    CHECK(single.front() == "solo");
}

struct ShowTestStart : public doctest::IReporter {
    ShowTestStart(const doctest::ContextOptions& /* in */) {
    }
    void test_case_start(const doctest::TestCaseData& in) override {
#ifdef SP_LOG_DEBUG
        std::lock_guard<std::mutex> lock(SP::logger().coutMutex);
#endif
        std::cout << "Test: " << in.m_name << std::endl;
        CpuWatchdog::instance().testCaseStarted(in.m_name);
    }
    void report_query(const doctest::QueryData&) override {
    }
    void test_run_start() override {
    }
    void test_run_end(const doctest::TestRunStats&) override {
    }
    void test_case_reenter(const doctest::TestCaseData&) override {
    }
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        CpuWatchdog::instance().testCaseEnded();
    }
    void test_case_exception(const doctest::TestCaseException&) override {
    }
    void subcase_start(const doctest::SubcaseSignature& in) override {
#ifdef SP_LOG_DEBUG
        std::lock_guard<std::mutex> lock(SP::logger().coutMutex);
#endif
        std::cout << "\tSubcase: " << in.m_name << std::endl;
    }
    void subcase_end() override {
    }
    void log_assert(const doctest::AssertData&) override {
    }
    void log_message(const doctest::MessageData&) override {
    }
    void test_case_skipped(const doctest::TestCaseData&) override {
    }
};

REGISTER_LISTENER("test_start", 1, ShowTestStart);

int main(int argc, char** argv) {
#ifdef SP_LOG_DEBUG
    // Start with logging disabled
    SP::set_logging_enabled(false);
#endif

    doctest::Context context;

    // Apply command line arguments
    context.applyCommandLine(argc, argv);

    if (const char* include_case = std::getenv("PATHSPACE_TEST_CASE")) {
        if (*include_case != '\0') {
            context.addFilter("test-case", include_case);
        }
    }
    if (const char* exclude_case = std::getenv("PATHSPACE_TEST_CASE_EXCLUDE")) {
        if (*exclude_case != '\0') {
            context.addFilter("test-case-exclude", exclude_case);
        }
    }
    if (const char* include_suite = std::getenv("PATHSPACE_TEST_SUITE")) {
        if (*include_suite != '\0') {
            std::vector<std::string> suite_filters = split_suite_filters(include_suite);
            if (suite_filters.empty()) {
                context.addFilter("test-suite", include_suite);
            } else {
                for (const std::string& filter : suite_filters) {
                    context.addFilter("test-suite", filter.c_str());
                }
            }
        }
    }
    if (const char* exclude_suite = std::getenv("PATHSPACE_TEST_SUITE_EXCLUDE")) {
        if (*exclude_suite != '\0') {
            context.addFilter("test-suite-exclude", exclude_suite);
        }
    }

#ifdef SP_LOG_DEBUG
    // Initialize thread name for logging
    SP::set_thread_name("TestMain");
    // Determine whether to enable logging based on PATHSPACE_LOG
    bool enableLog = false;
    if (const char* _env_log = std::getenv("PATHSPACE_LOG")) {
        if (std::strcmp(_env_log, "0") != 0) enableLog = true;
    }
#endif

    // Check if we're in test discovery mode or if we should exit early
    if (context.shouldExit()) {
        // Run doctest without enabling logging
        return context.run();
    }

// No default exclusions: run all test cases unless the user filters via CLI.

#ifdef SP_LOG_DEBUG
    // Enable logging for normal test execution if PATHSPACE_LOG is set (and not "0")
    if (enableLog) {
        SP::set_logging_enabled(true);
        sp_log("Starting test execution", "TEST", "INFO");
    }
#endif

    // Run the tests
    int res = context.run();

    // Log the test results
#ifdef SP_LOG_DEBUG
    if (enableLog) {
        if (res == 0) {
            sp_log("All tests passed successfully", "TEST", "SUCCESS");
        } else {
            sp_log("Some tests failed", "TEST", "FAILURE");
        }
    }
#endif

    return res;
}
