#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pathspace/utils/PopFrontVector.hpp>
#include <pathspace/utils/PopFrontVectorStreamBuffer.hpp>

using namespace SP;

TEST_CASE("PopFrontVectorStreamBuffer", "[PopFrontVector][PopFrontVectorStreamBuffer]") {
    PopFrontVector<std::byte> buffer;
    PopFrontVectorStreamBuffer bufferStream(buffer);

    SECTION("Writing to the buffer and reading back") {
        std::iostream stream(&bufferStream);
        std::string testStr = "Hello, World!";

        stream << testStr;
        stream.flush(); // Ensure buffer is flushed

        REQUIRE(reinterpret_cast<char*>(buffer.data())==testStr);

        // Reset stream state and reposition read pointer
        stream.clear();  // Clear any error flags
        stream.seekg(0); // Reposition to the beginning for reading

        // Read from the stream
        std::string output;
        std::getline(stream, output);  // Use getline to read the whole line

        REQUIRE(output == testStr);
    }

    SECTION("Reading and writing multiple times") {
        std::iostream stream(&bufferStream);

        // Write and read multiple times
        for (int i = 0; i < 5; ++i) {
            std::string testStr = "Test " + std::to_string(i);
            stream << testStr;
            stream.flush();

            stream.clear();  // Clear any error flags
            stream.seekg(0); // Reposition to the beginning for reading

            std::string output;
            std::getline(stream, output);  // Use getline to read the whole line

            REQUIRE(output == testStr);

            stream.clear();  // Clear any error flags
            stream.seekg(0); // Reposition to the beginning for reading
        }
    }

    SECTION("Buffer correctly handles different data types") {
        std::iostream stream(&bufferStream);

        // Write different data types
        int number = 123;
        double floating = 45.67;
        std::string text = "Sample_text";

        stream << number << ' ' << floating << ' ' << text;
        stream.flush();

        stream.clear();  // Clear any error flags
        stream.seekg(0); // Reposition to the beginning for reading

        // Read and verify
        int readNumber;
        double readFloating;
        std::string readText;

        stream >> readNumber >> readFloating >> readText;

        REQUIRE(readNumber == number);
        REQUIRE(readFloating == Catch::Approx(floating));
        REQUIRE(readText == text);
    }
}