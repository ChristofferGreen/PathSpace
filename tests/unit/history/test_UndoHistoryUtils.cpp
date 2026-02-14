#include "third_party/doctest.h"

#include <pathspace/history/UndoHistoryUtils.hpp>

#include <chrono>
#include <cctype>
#include <filesystem>
#include <algorithm>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace UndoUtils = SP::History::UndoUtils;

namespace {

struct TempDir {
    explicit TempDir(std::string_view name) {
        auto uuid = UndoUtils::generateSpaceUuid();
        path      = std::filesystem::temp_directory_path() / name / uuid;
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

auto makeBinary(std::initializer_list<uint8_t> values) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (auto v : values) {
        bytes.push_back(static_cast<std::byte>(v));
    }
    return bytes;
}

auto isHexString(std::string const& text) -> bool {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

} // namespace

TEST_SUITE_BEGIN("history.undo.utils");

TEST_CASE("timepoint round-trip through millis helpers") {
    auto const tp      = std::chrono::system_clock::time_point{std::chrono::milliseconds{123456789}};
    auto const millis  = UndoUtils::toMillis(tp);
    auto const rebuilt = UndoUtils::fromMillis(millis);

    CHECK(millis == 123456789ULL);
    CHECK(rebuilt == tp);
}

TEST_CASE("generateSpaceUuid yields 32 hex characters") {
    auto first  = UndoUtils::generateSpaceUuid();
    auto second = UndoUtils::generateSpaceUuid();

    CHECK(first.size() == 32);
    CHECK(second.size() == 32);
    CHECK(isHexString(first));
    CHECK(isHexString(second));
    CHECK(first != second); // extremely low collision odds
}

TEST_CASE("atomic text write/read with fsync") {
    TempDir tmp("pathspace_undo_utils_text");
    auto const file = tmp.path / "note.txt";

    auto write = UndoUtils::writeTextFileAtomic(file, "hello world", true);
    REQUIRE(write.has_value());

    auto contents = UndoUtils::readTextFile(file);
    REQUIRE(contents.has_value());
    CHECK(*contents == "hello world");
    CHECK(UndoUtils::fileSizeOrZero(file) == std::string("hello world").size());

    UndoUtils::removePathIfExists(file);
    CHECK(UndoUtils::fileSizeOrZero(file) == 0);
}

TEST_CASE("atomic binary write/read without fsync") {
    TempDir tmp("pathspace_undo_utils_bin");
    auto const file = tmp.path / "data.bin";

    auto bytes = makeBinary({0x01, 0x02, 0x03, 0x04, 0xFF});
    auto write = UndoUtils::writeFileAtomic(file, std::span<const std::byte>(bytes.data(), bytes.size()), false, true);
    REQUIRE(write.has_value());

    auto loaded = UndoUtils::readBinaryFile(file);
    REQUIRE(loaded.has_value());
    CHECK(loaded->size() == bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        CHECK(static_cast<uint8_t>((*loaded)[i]) == static_cast<uint8_t>(bytes[i]));
    }
}

TEST_CASE("file helpers handle missing paths gracefully") {
    TempDir tmp("pathspace_undo_utils_missing");
    auto const missingFile = tmp.path / "absent.bin";

    auto binary = UndoUtils::readBinaryFile(missingFile);
    REQUIRE_FALSE(binary.has_value());
    CHECK(binary.error().code == SP::Error::Code::NotFound);

    auto text = UndoUtils::readTextFile(missingFile);
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().code == SP::Error::Code::NotFound);

    // Removing a non-existent path should be a no-op.
    REQUIRE_NOTHROW(UndoUtils::removePathIfExists(missingFile));
    CHECK(UndoUtils::fileSizeOrZero(missingFile) == 0);
}

TEST_CASE("fileSizeOrZero returns zero for directories") {
    TempDir tmp("pathspace_undo_utils_dirsize");
    auto const dir = tmp.path / "dir";
    std::filesystem::create_directories(dir);
    CHECK(UndoUtils::fileSizeOrZero(dir) == 0);
}

TEST_CASE("fsync helpers propagate success and failure") {
    auto invalid = UndoUtils::fsyncFileDescriptor(-1);
    CHECK_FALSE(invalid.has_value());
    CHECK(invalid.error().code == SP::Error::Code::UnknownError);

    TempDir tmp("pathspace_undo_utils_fsync");
    auto    file = tmp.path / "sync.bin";

#ifdef _WIN32
    int fd = _open(file.string().c_str(), _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = ::open(file.c_str(), O_CREAT | O_WRONLY, 0644);
#endif
    REQUIRE(fd >= 0);

    auto payload = makeBinary({0xAA, 0xBB, 0xCC});
#ifdef _WIN32
    auto written = _write(fd, reinterpret_cast<void const*>(payload.data()), static_cast<unsigned int>(payload.size()));
    REQUIRE(written == static_cast<int>(payload.size()));
#else
    auto written = ::write(fd, reinterpret_cast<void const*>(payload.data()), payload.size());
    REQUIRE(written == static_cast<ssize_t>(payload.size()));
#endif

    auto syncFd = UndoUtils::fsyncFileDescriptor(fd);
    CHECK(syncFd.has_value());

#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif

    auto syncDir = UndoUtils::fsyncDirectory(tmp.path);
    CHECK(syncDir.has_value());
}

TEST_CASE("fsyncDirectory reports error for non-directory path") {
    TempDir tmp("pathspace_undo_utils_notdir");
    auto    file = tmp.path / "file.txt";

    auto write = UndoUtils::writeTextFileAtomic(file, "data", false);
    REQUIRE(write.has_value());

    auto badDir = UndoUtils::fsyncDirectory(file);
    CHECK_FALSE(badDir.has_value());
    CHECK(badDir.error().code == SP::Error::Code::UnknownError);
}

TEST_CASE("writeFileAtomic reports error when parent path is not a directory") {
    TempDir tmp("pathspace_undo_utils_bad_parent");
    auto    fileParent = tmp.path / "parent_file";

    auto write = UndoUtils::writeTextFileAtomic(fileParent, "seed", false);
    REQUIRE(write.has_value());

    auto child = fileParent / "child.txt";
    auto result = UndoUtils::writeTextFileAtomic(child, "payload", false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == SP::Error::Code::UnknownError);
}

#ifndef _WIN32
TEST_CASE("writeFileAtomic reports error when directory is not writable") {
    TempDir tmp("pathspace_undo_utils_ro");
    auto    roDir = tmp.path / "readonly";

    std::filesystem::create_directories(roDir);
    auto originalPerms = std::filesystem::status(roDir).permissions();
    auto readonlyPerms = originalPerms
                         & ~std::filesystem::perms::owner_write
                         & ~std::filesystem::perms::group_write
                         & ~std::filesystem::perms::others_write;
    std::error_code ec;
    std::filesystem::permissions(roDir, readonlyPerms, ec);
    REQUIRE_FALSE(ec);

    auto target = roDir / "note.txt";
    auto result = UndoUtils::writeTextFileAtomic(target, "data", false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == SP::Error::Code::UnknownError);

    std::filesystem::permissions(roDir, originalPerms, ec);
    CHECK_FALSE(ec);
}
#endif

TEST_SUITE_END();
