#include "history/UndoHistoryUtils.hpp"

#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SP::History::UndoUtils {

auto toMillis(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
    return static_cast<std::uint64_t>(duration.count());
}

auto generateSpaceUuid() -> std::string {
    std::random_device                           rd;
    std::uniform_int_distribution<std::uint64_t> dist;
    auto                                         high = dist(rd);
    auto                                         low  = dist(rd);
    std::ostringstream                           oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << high << std::setw(16) << low;
    return oss.str();
}

auto fsyncFileDescriptor(int fd) -> Expected<void> {
#ifdef _WIN32
    if (_commit(fd) != 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "_commit failed"});
    }
#else
    if (::fsync(fd) != 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "fsync failed"});
    }
#endif
    return {};
}

auto fsyncDirectory(std::filesystem::path const& dir) -> Expected<void> {
#ifdef _WIN32
    (void)dir;
    return {};
#else
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "open directory failed"});
    }
    auto result = fsyncFileDescriptor(fd);
    ::close(fd);
    return result;
#endif
}

auto writeFileAtomic(std::filesystem::path const& path,
                     std::span<const std::byte> data,
                     bool fsyncData,
                     bool binary) -> Expected<void> {
    std::error_code ec;
    auto            parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(Error{Error::Code::UnknownError, "Failed to create directories"});
        }
    }

    auto tmpPath = path;
    tmpPath += ".tmp";

#ifdef _WIN32
    int flags = _O_CREAT | _O_TRUNC | _O_WRONLY | (binary ? _O_BINARY : 0);
    int fd    = _open(tmpPath.string().c_str(), flags, _S_IREAD | _S_IWRITE);
#else
    int flags = O_CREAT | O_TRUNC | O_WRONLY | (binary ? 0 : 0);
    int fd    = ::open(tmpPath.c_str(), flags, 0644);
#endif
    if (fd < 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to open temp file"});
    }

    std::size_t totalWritten = 0;
    while (totalWritten < data.size()) {
        auto const* ptr = data.data() + static_cast<std::ptrdiff_t>(totalWritten);
        auto const  remaining = data.size() - totalWritten;
#ifdef _WIN32
        auto written = _write(fd, reinterpret_cast<void const*>(ptr), static_cast<unsigned int>(remaining));
#else
        auto written = ::write(fd, reinterpret_cast<void const*>(ptr), remaining);
#endif
        if (written <= 0) {
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            return std::unexpected(Error{Error::Code::UnknownError, "Failed to write temp file"});
        }
        totalWritten += static_cast<std::size_t>(written);
    }

    if (fsyncData) {
        if (auto sync = fsyncFileDescriptor(fd); !sync) {
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
            return sync;
        }
    }

#ifdef _WIN32
    if (_close(fd) != 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to close temp file"});
    }
#else
    if (::close(fd) != 0) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to close temp file"});
    }
#endif

    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to rename temp file"});
    }

    if (fsyncData && !parent.empty()) {
        auto syncDir = fsyncDirectory(parent);
        if (!syncDir) {
            return syncDir;
        }
    }

    return {};
}

auto writeTextFileAtomic(std::filesystem::path const& path,
                         std::string const& text,
                         bool fsyncData) -> Expected<void> {
    auto span = std::as_bytes(std::span(text.data(), text.size()));
    return writeFileAtomic(path, span, fsyncData, false);
}

auto readBinaryFile(std::filesystem::path const& path) -> Expected<std::vector<std::byte>> {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{Error::Code::NotFound, "File not found"});
    }
    stream.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> buffer(size);
    if (!stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size))) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to read file"});
    }
    return buffer;
}

auto readTextFile(std::filesystem::path const& path) -> Expected<std::string> {
    std::ifstream stream(path);
    if (!stream) {
        return std::unexpected(Error{Error::Code::NotFound, "File not found"});
    }
    std::ostringstream oss;
    oss << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to read file"});
    }
    return oss.str();
}

void removePathIfExists(std::filesystem::path const& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

auto fileSizeOrZero(std::filesystem::path const& path) -> std::uintmax_t {
    std::error_code ec;
    auto            size = std::filesystem::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return size;
}

} // namespace SP::History::UndoUtils
