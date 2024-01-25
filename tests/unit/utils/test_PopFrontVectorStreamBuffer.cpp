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

    SECTION("Writing and reading large data") {
        std::iostream stream(&bufferStream);
        std::string largeStr(1000, 'x'); // Large string of 1000 'x's

        stream << largeStr;
        stream.flush();

        stream.clear();
        stream.seekg(0);

        std::string output;
        std::getline(stream, output);

        REQUIRE(output == largeStr);
    }

    SECTION("Check buffer size constraints") {
        std::iostream stream(&bufferStream);
        std::string longStr(2000, 'y'); // Longer than internal buffer size

        stream << longStr;
        stream.flush();

        stream.clear();
        stream.seekg(0);

        std::string output;
        std::getline(stream, output);

        REQUIRE(output == longStr);
    }

    SECTION("Check underflow handling") {
        std::iostream stream(&bufferStream);
        std::string testStr = "Short test";

        stream << testStr;
        stream.flush();

        stream.clear();
        stream.seekg(0);

        char ch;
        std::string output;
        while (stream.get(ch)) {
            output.push_back(ch);
        }

        REQUIRE(output == testStr);
    }

    SECTION("Check for write after read") {
        std::iostream stream(&bufferStream);
        std::string initialStr = "Initial";
        std::string newStr = "New";

        stream << initialStr;
        stream.flush();
        stream.clear();
        stream.seekg(0);

        std::string output;
        std::getline(stream, output);
        REQUIRE(output == initialStr);

        stream.clear();
        stream.seekp(0);
        stream << newStr;
        stream.flush();
        stream.clear();
        stream.seekg(0);

        std::getline(stream, output);
        REQUIRE(output == newStr);
    }

    SECTION("Check handling of empty buffer") {
        std::iostream stream(&bufferStream);

        std::string output = "non-empty";
        std::getline(stream, output);

        REQUIRE(output.empty());
    }

    SECTION("Check stream state after operations") {
        std::iostream stream(&bufferStream);

        stream << "Test";
        REQUIRE(stream.good());

        stream.setstate(std::ios::badbit);
        REQUIRE(stream.bad());

        stream.clear();
        REQUIRE(stream.good());
    }    
}