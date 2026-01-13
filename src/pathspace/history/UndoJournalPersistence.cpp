#include "history/UndoJournalPersistence.hpp"

#include "history/UndoHistoryUtils.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using SP::Error;
using SP::Expected;
using SP::History::UndoJournal::JournalEntry;
using SP::History::UndoJournal::JournalFileWriter;
using SP::History::UndoJournal::JournalFileMagic;
using SP::History::UndoJournal::JournalFileVersion;
namespace UndoUtils = SP::History::UndoUtils;

inline auto errnoMessage(std::string_view prefix) -> Error {
    return Error{Error::Code::UnknownError,
                 std::string(prefix) + ": " + std::strerror(errno)};
}

inline auto makeError(Error::Code code, std::string message) -> Error {
    return Error{code, std::move(message)};
}

inline auto readScalar(std::FILE* file, void* out, std::size_t size) -> Expected<void> {
    if (std::fread(out, size, 1, file) != 1) {
        if (std::feof(file))
            return std::unexpected(makeError(Error::Code::MalformedInput, "Unexpected end of journal file"));
        return std::unexpected(errnoMessage("Failed to read journal file"));
    }
    return {};
}

inline auto writeScalar(std::FILE* file, void const* data, std::size_t size) -> Expected<void> {
    if (std::fwrite(data, size, 1, file) != 1)
        return std::unexpected(errnoMessage("Failed to write journal file"));
    return {};
}

inline auto fsyncFile(std::FILE* file) -> Expected<void> {
    if (!file)
        return {};
    if (std::fflush(file) != 0)
        return std::unexpected(errnoMessage("Failed to flush journal file"));
    auto fd = ::fileno(file);
    if (fd == -1)
        return std::unexpected(errnoMessage("Failed to acquire journal file descriptor"));
    return UndoUtils::fsyncFileDescriptor(fd);
}

} // namespace

namespace SP::History::UndoJournal {

JournalFileWriter::JournalFileWriter(std::filesystem::path path)
    : filePath(std::move(path)) {}

JournalFileWriter::~JournalFileWriter() {
    if (handle) {
        std::fflush(handle);
        std::fclose(handle);
    }
}

auto JournalFileWriter::open(bool fsyncHeader) -> Expected<void> {
    return ensureOpened(fsyncHeader);
}

auto JournalFileWriter::append(JournalEntry const& entry, bool fsync) -> Expected<void> {
    if (auto ensure = ensureOpened(fsync); !ensure)
        return ensure;

    auto serialized = serializeEntry(entry);
    if (!serialized)
        return std::unexpected(serialized.error());

    auto size = serialized->size();
    if (size > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(makeError(Error::Code::UnknownError, "Journal entry exceeds maximum encodable size"));

    std::uint32_t length = static_cast<std::uint32_t>(size);
    if (auto writeLength = writeScalar(handle, &length, sizeof(length)); !writeLength)
        return writeLength;
    if (!serialized->empty()) {
        if (std::fwrite(serialized->data(), 1, serialized->size(), handle) != serialized->size())
            return std::unexpected(errnoMessage("Failed to write journal entry payload"));
    }

    if (fsync) {
        if (auto sync = fsyncFile(handle); !sync)
            return sync;
    }
    return {};
}

auto JournalFileWriter::flush() -> Expected<void> {
    if (!handle)
        return {};
    if (std::fflush(handle) != 0)
        return std::unexpected(errnoMessage("Failed to flush journal writer"));
    return {};
}

auto JournalFileWriter::ensureOpened(bool fsyncHeader) -> Expected<void> {
    if (handle)
        return {};

    auto parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(
                makeError(Error::Code::UnknownError, "Failed to create journal directory: " + ec.message()));
        }
    }

    bool needHeader = !std::filesystem::exists(filePath)
                      || std::filesystem::file_size(filePath) == 0;
    if (needHeader) {
        auto write = writeHeader(fsyncHeader);
        if (!write)
            return write;
    }

    handle = std::fopen(filePath.string().c_str(), "rb+");
    if (!handle)
        return std::unexpected(errnoMessage("Failed to open journal file"));

    if (!needHeader) {
        if (auto validate = validateHeader(); !validate) {
            std::fclose(handle);
            handle = nullptr;
            return validate;
        }
    }

    if (std::fseek(handle, 0, SEEK_END) != 0) {
        auto err = errnoMessage("Failed to seek journal file");
        std::fclose(handle);
        handle = nullptr;
        return std::unexpected(err);
    }

    return {};
}

auto JournalFileWriter::writeHeader(bool fsync) -> Expected<void> {
    auto parent = filePath.parent_path();
    std::FILE* headerFile = std::fopen(filePath.string().c_str(), "wb");
    if (!headerFile)
        return std::unexpected(errnoMessage("Failed to create journal file"));

    auto guard = std::unique_ptr<std::FILE, decltype(&std::fclose)>{headerFile, &std::fclose};

    if (auto writeMagic = writeScalar(headerFile, &JournalFileMagic, sizeof(JournalFileMagic)); !writeMagic)
        return writeMagic;
    if (auto writeVersion = writeScalar(headerFile, &JournalFileVersion, sizeof(JournalFileVersion)); !writeVersion)
        return writeVersion;

    std::uint32_t reserved = 0;
    if (auto writeReserved = writeScalar(headerFile, &reserved, sizeof(reserved)); !writeReserved)
        return writeReserved;

    if (fsync) {
        if (auto sync = fsyncFile(headerFile); !sync)
            return sync;
        if (!parent.empty()) {
            if (auto dirSync = UndoUtils::fsyncDirectory(parent); !dirSync)
                return dirSync;
        }
    }

    return {};
}

auto JournalFileWriter::validateHeader() -> Expected<void> {
    std::uint32_t magic = 0;
    if (auto readMagic = readScalar(handle, &magic, sizeof(magic)); !readMagic)
        return readMagic;

    if (magic != JournalFileMagic) {
        return std::unexpected(
            makeError(Error::Code::MalformedInput, "Journal file header magic mismatch"));
    }

    std::uint16_t version = 0;
    if (auto readVersion = readScalar(handle, &version, sizeof(version)); !readVersion)
        return readVersion;

    if (version != JournalFileVersion) {
        return std::unexpected(
            makeError(Error::Code::MalformedInput, "Unsupported journal file version"));
    }

    std::uint32_t reserved = 0;
    if (auto readReserved = readScalar(handle, &reserved, sizeof(reserved)); !readReserved)
        return readReserved;
    (void)reserved;

    return {};
}

auto replayJournal(
    std::filesystem::path const& path,
    std::function<Expected<void>(JournalEntry&&)> const& onEntry) -> Expected<void> {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) {
        if (errno == ENOENT)
            return std::unexpected(makeError(Error::Code::NotFound, "Journal file not found"));
        return std::unexpected(errnoMessage("Failed to open journal file for replay"));
    }

    auto guard = std::unique_ptr<std::FILE, decltype(&std::fclose)>{file, &std::fclose};

    std::uint32_t magic = 0;
    if (auto readMagic = readScalar(file, &magic, sizeof(magic)); !readMagic)
        return readMagic;
    if (magic != JournalFileMagic)
        return std::unexpected(makeError(Error::Code::MalformedInput, "Journal file magic mismatch"));

    std::uint16_t version = 0;
    if (auto readVersion = readScalar(file, &version, sizeof(version)); !readVersion)
        return readVersion;
    if (version != JournalFileVersion)
        return std::unexpected(makeError(Error::Code::MalformedInput, "Unsupported journal file version"));

    std::uint32_t reserved = 0;
    if (auto readReserved = readScalar(file, &reserved, sizeof(reserved)); !readReserved)
        return readReserved;
    (void)reserved;

    while (true) {
        std::uint32_t length = 0;
        auto readLength = std::fread(&length, sizeof(length), 1, file);
        if (readLength != 1) {
            if (std::feof(file))
                break;
            return std::unexpected(errnoMessage("Failed to read journal entry length"));
        }

        std::vector<std::byte> buffer;
        buffer.resize(length);
        if (length > 0) {
            if (std::fread(buffer.data(), 1, length, file) != length) {
                if (std::feof(file)) {
                    return std::unexpected(makeError(Error::Code::MalformedInput, "Truncated journal entry payload"));
                }
                return std::unexpected(errnoMessage("Failed to read journal entry payload"));
            }
        }

        auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
        if (!decoded)
            return std::unexpected(decoded.error());

        if (auto callbackResult = onEntry(std::move(decoded.value())); !callbackResult)
            return callbackResult;
    }

    return {};
}

auto compactJournal(
    std::filesystem::path const& path,
    std::span<JournalEntry const> entries,
    bool fsyncTarget) -> Expected<void> {
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(
                makeError(Error::Code::UnknownError, "Failed to create journal directory: " + ec.message()));
        }
    }

    auto tempPath = path;
    tempPath += ".tmp";

    std::FILE* file = std::fopen(tempPath.string().c_str(), "wb");
    if (!file)
        return std::unexpected(errnoMessage("Failed to open journal temp file"));

    auto guard = std::unique_ptr<std::FILE, decltype(&std::fclose)>{file, &std::fclose};

    if (auto writeMagic = writeScalar(file, &JournalFileMagic, sizeof(JournalFileMagic)); !writeMagic)
        return writeMagic;
    if (auto writeVersion = writeScalar(file, &JournalFileVersion, sizeof(JournalFileVersion)); !writeVersion)
        return writeVersion;
    std::uint32_t reserved = 0;
    if (auto writeReserved = writeScalar(file, &reserved, sizeof(reserved)); !writeReserved)
        return writeReserved;

    for (auto const& entry : entries) {
        auto serialized = serializeEntry(entry);
        if (!serialized)
            return std::unexpected(serialized.error());
        if (serialized->size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(
                makeError(Error::Code::UnknownError, "Journal entry exceeds maximum encodable size"));
        }
        std::uint32_t length = static_cast<std::uint32_t>(serialized->size());
        if (auto writeLength = writeScalar(file, &length, sizeof(length)); !writeLength)
            return writeLength;
        if (!serialized->empty()) {
            if (std::fwrite(serialized->data(), 1, serialized->size(), file) != serialized->size())
                return std::unexpected(errnoMessage("Failed to write compacted journal entry"));
        }
    }

    if (fsyncTarget) {
        if (auto sync = fsyncFile(file); !sync)
            return sync;
    } else if (std::fflush(file) != 0) {
        return std::unexpected(errnoMessage("Failed to flush journal temp file"));
    }

    guard.reset(); // close file

    std::error_code renameEc;
    std::filesystem::rename(tempPath, path, renameEc);
    if (renameEc) {
        std::filesystem::remove(tempPath);
        return std::unexpected(
            makeError(Error::Code::UnknownError, "Failed to replace journal file: " + renameEc.message()));
    }

    if (fsyncTarget && !parent.empty()) {
        if (auto dirSync = UndoUtils::fsyncDirectory(parent); !dirSync)
            return dirSync;
    }

    return {};
}

} // namespace SP::History::UndoJournal
