#define DOCTEST_CONFIG_IMPLEMENT
#include "ext/doctest.h"
#include "log/TaggedLogger.hpp"

struct ShowTestStart : public doctest::IReporter {
    ShowTestStart(const doctest::ContextOptions& /* in */) {
    }
    void test_case_start(const doctest::TestCaseData& in) override {
#ifdef SP_LOG_DEBUG
        std::lock_guard<std::mutex> lock(SP::logger().coutMutex);
#endif
        std::cout << "Test: " << in.m_name << std::endl;
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

#ifdef SP_LOG_DEBUG
    // Initialize thread name for logging
    SP::set_thread_name("TestMain");
#endif

    // Check if we're in test discovery mode or if we should exit early
    if (context.shouldExit()) {
        // Run doctest without enabling logging
        return context.run();
    }

#ifdef SP_LOG_DEBUG
    // Enable logging for normal test execution
    SP::set_logging_enabled(true);
    sp_log("Starting test execution", "TEST", "INFO");
#endif

    // Run the tests
    int res = context.run();

    // Log the test results
#ifdef SP_LOG_DEBUG
    if (res == 0) {
        sp_log("All tests passed successfully", "TEST", "SUCCESS");
    } else {
        sp_log("Some tests failed", "TEST", "FAILURE");
    }
#endif

    return res;
}