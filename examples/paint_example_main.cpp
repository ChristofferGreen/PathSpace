#include <pathspace/examples/paint/PaintExampleApp.hpp>

int main(int argc, char** argv) {
    auto options = PathSpaceExamples::ParsePaintExampleCommandLine(argc, argv);
    return PathSpaceExamples::RunPaintExample(std::move(options));
}
