#define DOCTEST_CONFIG_IMPLEMENT
#include "ext/doctest.h"
#include "utils/TaggedLogger.hpp"

int main(int argc, char** argv) {
    // Start with logging disabled
    SP::set_logging_enabled(false);

    doctest::Context context;

    // Apply command line arguments
    context.applyCommandLine(argc, argv);

    // Initialize thread name for logging
    SP::set_thread_name("TestMain");

    // Check if we're in test discovery mode or if we should exit early
    if (context.shouldExit()) {
        // Run doctest without enabling logging
        return context.run();
    }

    // Enable logging for normal test execution
    SP::set_logging_enabled(true);
    SP::log("Starting test execution", "TEST", "INFO");

    // Run the tests
    int res = context.run();

    // Log the test results
    if (res == 0) {
        SP::log("All tests passed successfully", "TEST", "SUCCESS");
    } else {
        SP::log("Some tests failed", "TEST", "FAILURE");
    }

    return res;
}