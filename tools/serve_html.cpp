#include <csignal>

#include <pathspace/web/ServeHtmlServer.hpp>

namespace {
void handle_signal(int) {
    SP::ServeHtml::RequestServeHtmlStop();
}
} // namespace

int main(int argc, char** argv) {
    auto options_opt = SP::ServeHtml::ParseServeHtmlArguments(argc, argv);
    if (!options_opt) {
        return EXIT_FAILURE;
    }

    auto options = *options_opt;
    if (options.show_help) {
        SP::ServeHtml::PrintServeHtmlUsage();
        return EXIT_SUCCESS;
    }

    SP::ServeHtml::ServeHtmlSpace space;
    SP::ServeHtml::ResetServeHtmlStopFlag();

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    return SP::ServeHtml::RunServeHtmlServer(space, options);
}
