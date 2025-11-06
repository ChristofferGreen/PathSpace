#include "history/UndoableSpace.hpp"

#include "PathSpace.hpp"
#include "core/InsertReturn.hpp"
#include "core/Node.hpp"
#include "core/NodeData.hpp"
#include "log/TaggedLogger.hpp"
#include <cstring>
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#endif
#include <iomanip>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <system_error>
#include <iterator>
#include <unistd.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <type_traits>

namespace {

using SP::History::CowSubtreePrototype;
using SP::Error;
using SP::Expected;

auto toMillis(std::chrono::system_clock::time_point tp) -> std::uint64_t {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
    return static_cast<std::uint64_t>(duration.count());
}

auto generateSpaceUuid() -> std::string {
    std::random_device                          rd;
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t                               high = dist(rd);
    std::uint64_t                               low  = dist(rd);
    std::ostringstream                          oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << high << std::setw(16) << low;
    return oss.str();
}

constexpr std::size_t kMaxUnsupportedLogEntries = 16;
constexpr std::string_view kUnsupportedNestedMessage =
    "History does not yet support nested PathSpaces";
constexpr std::string_view kUnsupportedExecutionMessage =
    "History does not yet support nodes containing tasks or futures";
constexpr std::string_view kUnsupportedSerializationMessage =
    "Unable to serialize node payload for history";

auto fsyncFd(int fd) -> Expected<void> {
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
    auto result = fsyncFd(fd);
    ::close(fd);
    return result;
#endif
}

auto writeFileAtomic(std::filesystem::path const& path,
                     std::span<const std::uint8_t> data,
                     bool fsyncData,
                     bool binary) -> Expected<void> {
    std::error_code ec;
    auto parent = path.parent_path();
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
#ifdef _WIN32
        auto written = _write(fd, data.data() + totalWritten, static_cast<unsigned int>(data.size() - totalWritten));
#else
        auto written = ::write(fd, data.data() + totalWritten, data.size() - totalWritten);
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
        if (auto sync = fsyncFd(fd); !sync) {
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
        if (!syncDir)
            return syncDir;
    }

    return {};
}

auto writeTextFileAtomic(std::filesystem::path const& path,
                         std::string const& text,
                         bool fsyncData) -> Expected<void> {
    auto span = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    return writeFileAtomic(path, span, fsyncData, false);
}

auto readBinaryFile(std::filesystem::path const& path) -> Expected<std::vector<std::uint8_t>> {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{Error::Code::NotFound, "File not found"});
    }
    stream.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buffer(size);
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

auto removePathIfExists(std::filesystem::path const& path) -> void {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

constexpr std::uint32_t kSnapshotMagic   = 0x50534853; // 'PSHS'
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::uint32_t kEntryMetaVersion = 1;
constexpr std::uint32_t kStateMetaVersion = 1;

template <typename T>
void appendScalar(std::vector<std::uint8_t>& buffer, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "appendScalar requires integral type");
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT uvalue = static_cast<UnsignedT>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        buffer.push_back(static_cast<std::uint8_t>((uvalue >> (i * 8)) & 0xFFu));
    }
}

template <typename T>
auto readScalar(std::span<const std::uint8_t>& buffer) -> std::optional<T> {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "readScalar requires integral type");
    if (buffer.size() < sizeof(T))
        return std::nullopt;
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<UnsignedT>(buffer[i]) << (i * 8);
    }
    buffer = buffer.subspan(sizeof(T));
    return static_cast<T>(value);
}

struct SnapshotEntryData {
    std::vector<std::string>    components;
    std::vector<std::uint8_t>   payload;
};

void collectSnapshotEntries(std::shared_ptr<const CowSubtreePrototype::Node> const& node,
                            std::vector<std::string>&                               components,
                            std::vector<SnapshotEntryData>&                         out) {
    if (!node)
        return;
    if (node->payload.bytes && !node->payload.bytes->empty()) {
        SnapshotEntryData entry;
        entry.components = components;
        entry.payload.assign(node->payload.bytes->begin(), node->payload.bytes->end());
        out.push_back(std::move(entry));
    }
    for (auto const& [childName, childNode] : node->children) {
        if (!childNode)
            continue;
        components.push_back(childName);
        collectSnapshotEntries(childNode, components, out);
        components.pop_back();
    }
}

auto encodeSnapshot(CowSubtreePrototype::Snapshot const& snapshot)
    -> Expected<std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> buffer;
    appendScalar(buffer, kSnapshotMagic);
    appendScalar(buffer, kSnapshotVersion);
    appendScalar(buffer, static_cast<std::uint64_t>(snapshot.generation));

    std::vector<SnapshotEntryData> entries;
    if (snapshot.root) {
        std::vector<std::string> path;
        collectSnapshotEntries(snapshot.root, path, entries);
    }
    appendScalar(buffer, static_cast<std::uint32_t>(entries.size()));

    for (auto const& entry : entries) {
        appendScalar(buffer, static_cast<std::uint32_t>(entry.components.size()));
        for (auto const& component : entry.components) {
            appendScalar(buffer, static_cast<std::uint32_t>(component.size()));
            buffer.insert(buffer.end(), component.begin(), component.end());
        }
        appendScalar(buffer, static_cast<std::uint32_t>(entry.payload.size()));
        buffer.insert(buffer.end(), entry.payload.begin(), entry.payload.end());
    }

    return buffer;
}

auto decodeSnapshot(CowSubtreePrototype& prototype, std::span<const std::uint8_t> data)
    -> Expected<CowSubtreePrototype::Snapshot> {
    auto buffer = data;

    auto magicOpt = readScalar<std::uint32_t>(buffer);
    if (!magicOpt || *magicOpt != kSnapshotMagic) {
        return std::unexpected(Error{Error::Code::UnknownError, "Invalid snapshot magic"});
    }
    auto versionOpt = readScalar<std::uint32_t>(buffer);
    if (!versionOpt || *versionOpt != kSnapshotVersion) {
        return std::unexpected(Error{Error::Code::UnknownError, "Unsupported snapshot version"});
    }
    auto generationOpt = readScalar<std::uint64_t>(buffer);
    if (!generationOpt) {
        return std::unexpected(Error{Error::Code::UnknownError, "Snapshot missing generation"});
    }
    auto countOpt = readScalar<std::uint32_t>(buffer);
    if (!countOpt) {
        return std::unexpected(Error{Error::Code::UnknownError, "Snapshot missing entry count"});
    }

    std::vector<CowSubtreePrototype::Mutation> mutations;
    mutations.reserve(*countOpt);

    for (std::uint32_t i = 0; i < *countOpt; ++i) {
        auto componentCountOpt = readScalar<std::uint32_t>(buffer);
        if (!componentCountOpt) {
            return std::unexpected(Error{Error::Code::UnknownError, "Snapshot malformed component count"});
        }
        std::vector<std::string> components;
        components.reserve(*componentCountOpt);
        for (std::uint32_t c = 0; c < *componentCountOpt; ++c) {
            auto lenOpt = readScalar<std::uint32_t>(buffer);
            if (!lenOpt || buffer.size() < *lenOpt) {
                return std::unexpected(Error{Error::Code::UnknownError, "Snapshot malformed component"});
            }
            auto comp = std::string(reinterpret_cast<char const*>(buffer.data()), *lenOpt);
            components.push_back(std::move(comp));
            buffer = buffer.subspan(*lenOpt);
        }
        auto payloadSizeOpt = readScalar<std::uint32_t>(buffer);
        if (!payloadSizeOpt || buffer.size() < *payloadSizeOpt) {
            return std::unexpected(Error{Error::Code::UnknownError, "Snapshot malformed payload"});
        }
        std::vector<std::uint8_t> payload(*payloadSizeOpt);
        std::memcpy(payload.data(), buffer.data(), *payloadSizeOpt);
        buffer = buffer.subspan(*payloadSizeOpt);

        CowSubtreePrototype::Mutation mutation;
        mutation.components = std::move(components);
        mutation.payload    = CowSubtreePrototype::Payload(std::move(payload));
        mutations.push_back(std::move(mutation));
    }

    auto snapshot = prototype.emptySnapshot();
    for (auto const& mutation : mutations) {
        snapshot = prototype.apply(snapshot, mutation);
    }
    snapshot.generation = static_cast<std::size_t>(*generationOpt);
    return snapshot;
}

struct EntryMetadata {
    std::size_t  generation = 0;
    std::size_t  bytes      = 0;
    std::uint64_t timestampMs = 0;
};

struct StateMetadata {
    std::size_t              liveGeneration = 0;
    std::vector<std::size_t> undoGenerations;
    std::vector<std::size_t> redoGenerations;
    bool                     manualGc       = false;
    std::size_t              ramCacheEntries = 0;
};

auto snapshotFileStem(std::size_t generation) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << generation;
    return oss.str();
}

auto joinGenerations(std::vector<std::size_t> const& gens) -> std::string {
    std::ostringstream oss;
    for (std::size_t i = 0; i < gens.size(); ++i) {
        if (i > 0)
            oss << ',';
        oss << gens[i];
    }
    return oss.str();
}

auto parseGenerations(std::string_view value) -> Expected<std::vector<std::size_t>> {
    std::vector<std::size_t> result;
    if (value.empty())
        return result;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(',', start);
        if (end == std::string_view::npos)
            end = value.size();
        auto token = value.substr(start, end - start);
        std::size_t number = 0;
        auto fc = std::from_chars(token.data(), token.data() + token.size(), number);
        if (fc.ec != std::errc()) {
            return std::unexpected(Error{Error::Code::UnknownError, "Failed to parse generation list"});
        }
        result.push_back(number);
        start = end + 1;
    }
    return result;
}

auto encodeEntryMeta(EntryMetadata const& meta) -> std::string {
    std::ostringstream oss;
    oss << "version:" << kEntryMetaVersion << '\n';
    oss << "generation:" << meta.generation << '\n';
    oss << "bytes:" << meta.bytes << '\n';
    oss << "timestamp_ms:" << meta.timestampMs << '\n';
    return oss.str();
}

auto parseEntryMeta(std::string const& text) -> Expected<EntryMetadata> {
    EntryMetadata meta;
    std::istringstream iss(text);
    std::string line;
    std::uint32_t version = 0;
    bool haveGen = false;
    bool haveBytes = false;
    bool haveTs = false;
    while (std::getline(iss, line)) {
        if (line.empty())
            continue;
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        auto key   = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        if (key == "version") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), version);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid entry meta version"});
        } else if (key == "generation") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), meta.generation);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid entry meta generation"});
            haveGen = true;
        } else if (key == "bytes") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), meta.bytes);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid entry meta bytes"});
            haveBytes = true;
        } else if (key == "timestamp_ms") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), meta.timestampMs);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid entry meta timestamp"});
            haveTs = true;
        }
    }
    if (version != kEntryMetaVersion || !haveGen || !haveBytes || !haveTs) {
        return std::unexpected(Error{Error::Code::UnknownError, "Incomplete entry metadata"});
    }
    return meta;
}

auto encodeStateMeta(StateMetadata const& meta) -> std::string {
    std::ostringstream oss;
    oss << "version:" << kStateMetaVersion << '\n';
    oss << "live_generation:" << meta.liveGeneration << '\n';
    oss << "undo:" << joinGenerations(meta.undoGenerations) << '\n';
    oss << "redo:" << joinGenerations(meta.redoGenerations) << '\n';
    oss << "manual_gc:" << (meta.manualGc ? 1 : 0) << '\n';
    oss << "ram_cache_entries:" << meta.ramCacheEntries << '\n';
    return oss.str();
}

auto parseStateMeta(std::string const& text) -> Expected<StateMetadata> {
    StateMetadata meta;
    std::istringstream iss(text);
    std::string line;
    std::uint32_t version = 0;
    bool haveLive = false;
    bool haveUndo = false;
    bool haveRedo = false;
    bool haveManual = false;
    bool haveRam = false;
    while (std::getline(iss, line)) {
        if (line.empty())
            continue;
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        auto key   = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        if (key == "version") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), version);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid state meta version"});
        } else if (key == "live_generation") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), meta.liveGeneration);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid live generation"});
            haveLive = true;
        } else if (key == "undo") {
            auto parsed = parseGenerations(value);
            if (!parsed)
                return std::unexpected(parsed.error());
            meta.undoGenerations = std::move(parsed.value());
            haveUndo            = true;
        } else if (key == "redo") {
            auto parsed = parseGenerations(value);
            if (!parsed)
                return std::unexpected(parsed.error());
            meta.redoGenerations = std::move(parsed.value());
            haveRedo             = true;
        } else if (key == "manual_gc") {
            int flag = 0;
            auto fc  = std::from_chars(value.data(), value.data() + value.size(), flag);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid manual_gc flag"});
            meta.manualGc = flag != 0;
            haveManual    = true;
        } else if (key == "ram_cache_entries") {
            auto fc = std::from_chars(value.data(), value.data() + value.size(), meta.ramCacheEntries);
            if (fc.ec != std::errc())
                return std::unexpected(Error{Error::Code::UnknownError, "Invalid ram_cache_entries"});
            haveRam = true;
        }
    }
    if (version != kStateMetaVersion || !haveLive || !haveUndo || !haveRedo || !haveManual || !haveRam) {
        return std::unexpected(Error{Error::Code::UnknownError, "Incomplete state metadata"});
    }
    return meta;
}

} // namespace

namespace SP::History {

using SP::ConcretePathStringView;

struct UndoableSpace::RootState {
    struct Entry {
        CowSubtreePrototype::Snapshot             snapshot;
        std::size_t                               bytes     = 0;
        std::chrono::system_clock::time_point     timestamp = std::chrono::system_clock::now();
        bool                                      persisted = false;
        bool                                      cached    = true;
    };

    struct OperationRecord {
        std::string                               type;
        std::chrono::system_clock::time_point     timestamp;
        std::chrono::milliseconds                 duration{0};
        bool                                      success          = true;
        std::size_t                               undoCountBefore  = 0;
        std::size_t                               undoCountAfter   = 0;
        std::size_t                               redoCountBefore  = 0;
        std::size_t                               redoCountAfter   = 0;
        std::size_t                               bytesBefore      = 0;
        std::size_t                               bytesAfter       = 0;
        std::string                               message;
    };

    struct Telemetry {
        struct UnsupportedRecord {
            std::string                               path;
            std::string                               reason;
            std::chrono::system_clock::time_point     timestamp;
            std::size_t                               occurrences = 0;
        };

        std::size_t                                               undoBytes        = 0;
        std::size_t                                               redoBytes        = 0;
        std::size_t                                               trimOperations   = 0;
        std::size_t                                               trimmedEntries   = 0;
        std::size_t                                               trimmedBytes     = 0;
        std::optional<std::chrono::system_clock::time_point>      lastTrimTimestamp;
        std::optional<OperationRecord>                            lastOperation;
        std::size_t                                               diskBytes        = 0;
        std::size_t                                               diskEntries      = 0;
        std::size_t                                               cachedUndo       = 0;
        std::size_t                                               cachedRedo       = 0;
        bool                                                      persistenceDirty = false;
        std::size_t                                               unsupportedTotal = 0;
        std::vector<UnsupportedRecord>                            unsupportedLog;
    };

    struct TransactionState {
        std::thread::id                    owner;
        std::size_t                        depth          = 0;
        bool                               dirty          = false;
        CowSubtreePrototype::Snapshot      snapshotBefore;
    };

    std::string                               rootPath;
    std::vector<std::string>                  components;
    HistoryOptions                            options;
    CowSubtreePrototype                       prototype;
    CowSubtreePrototype::Snapshot             liveSnapshot;
    std::vector<Entry>                        undoStack;
    std::vector<Entry>                        redoStack;
    std::size_t                               liveBytes = 0;
    Telemetry                                 telemetry;
    std::optional<TransactionState>           activeTransaction;
    mutable std::mutex                        mutex;
    bool                                      persistenceEnabled = false;
    std::filesystem::path                     persistencePath;
    std::filesystem::path                     entriesPath;
    std::string                               encodedRoot;
    bool                                      stateDirty        = false;
    bool                                      hasPersistentState = false;
};

class UndoableSpace::OperationScope {
public:
    OperationScope(UndoableSpace& owner, RootState& state, std::string_view type)
        : owner(owner)
        , state(state)
        , type(type)
        , startSteady(std::chrono::steady_clock::now())
        , undoBefore(state.undoStack.size())
        , redoBefore(state.redoStack.size())
        , bytesBefore(UndoableSpace::computeTotalBytesLocked(state)) {}

    void setResult(bool success, std::string message = {}) {
        succeeded = success;
        messageText = std::move(message);
    }

    ~OperationScope() {
        owner.recordOperation(state, type, std::chrono::steady_clock::now() - startSteady,
                              succeeded, undoBefore, redoBefore, bytesBefore, messageText);
    }

private:
    UndoableSpace&                         owner;
    RootState&                             state;
    std::string                            type;
    std::chrono::steady_clock::time_point  startSteady;
    std::size_t                            undoBefore;
    std::size_t                            redoBefore;
    std::size_t                            bytesBefore;
    bool                                   succeeded = true;
    std::string                            messageText;
};

UndoableSpace::TransactionGuard::TransactionGuard(UndoableSpace& owner,
                                                  std::shared_ptr<RootState> state,
                                                  bool active)
    : owner(&owner)
    , state(std::move(state))
    , active(active) {}

UndoableSpace::TransactionGuard::TransactionGuard(TransactionGuard&& other) noexcept
    : owner(other.owner)
    , state(std::move(other.state))
    , active(other.active) {
    other.owner  = nullptr;
    other.active = false;
}

UndoableSpace::TransactionGuard&
UndoableSpace::TransactionGuard::operator=(TransactionGuard&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    owner        = other.owner;
    state        = std::move(other.state);
    active       = other.active;
    other.owner  = nullptr;
    other.active = false;
    return *this;
}

UndoableSpace::TransactionGuard::~TransactionGuard() {
    release();
}

void UndoableSpace::TransactionGuard::release() {
    if (active && owner && state) {
        auto const result = owner->commitTransaction(*state);
        if (!result) {
            sp_log("UndoableSpace::TransactionGuard commit failed during destruction: "
                       + result.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
    }
    active = false;
}

void UndoableSpace::TransactionGuard::markDirty() {
    if (!active || !owner || !state)
        return;
    owner->markTransactionDirty(*state);
}

auto UndoableSpace::TransactionGuard::commit() -> Expected<void> {
    if (!active || !owner || !state) {
        active = false;
        return {};
    }
    active = false;
    return owner->commitTransaction(*state);
}

void UndoableSpace::TransactionGuard::deactivate() {
    active = false;
}

UndoableSpace::HistoryTransaction::HistoryTransaction(UndoableSpace& owner,
                                                      std::shared_ptr<RootState> state)
    : owner(&owner)
    , rootState(std::move(state)) {}

UndoableSpace::HistoryTransaction::HistoryTransaction(HistoryTransaction&& other) noexcept
    : owner(other.owner)
    , rootState(std::move(other.rootState))
    , active(other.active) {
    other.owner  = nullptr;
    other.active = false;
}

UndoableSpace::HistoryTransaction&
UndoableSpace::HistoryTransaction::operator=(HistoryTransaction&& other) noexcept {
    if (this == &other)
        return *this;
    commit();
    owner        = other.owner;
    rootState    = std::move(other.rootState);
    active       = other.active;
    other.owner  = nullptr;
    other.active = false;
    return *this;
}

UndoableSpace::HistoryTransaction::~HistoryTransaction() {
    if (active && owner && rootState) {
        auto const result = owner->commitTransaction(*rootState);
        if (!result) {
            sp_log("UndoableSpace::HistoryTransaction auto-commit failed: "
                       + result.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
    }
}

auto UndoableSpace::HistoryTransaction::commit() -> Expected<void> {
    if (!active || !owner || !rootState) {
        active = false;
        return {};
    }
    active = false;
    return owner->commitTransaction(*rootState);
}

UndoableSpace::UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions defaults)
    : inner(std::move(inner))
    , defaultOptions(defaults)
    , spaceUuid(generateSpaceUuid()) {}

auto UndoableSpace::resolveRootNode() -> Node* {
    if (!inner)
        return nullptr;
    return inner->getRootNode();
}

auto UndoableSpace::enableHistory(ConcretePathStringView root, HistoryOptions opts) -> Expected<void> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto componentsExpected = canonical->components();
    if (!componentsExpected) {
        return std::unexpected(componentsExpected.error());
    }

    auto normalized = std::string{canonical->getPath()};
    auto components = std::move(componentsExpected.value());

    {
        std::scoped_lock lock(rootsMutex);
        if (roots.find(normalized) != roots.end()) {
            return std::unexpected(Error{Error::Code::UnknownError, "History already enabled for path"});
        }
        if (!defaultOptions.allowNestedUndo || !opts.allowNestedUndo) {
            ConcretePathStringView normalizedView{canonical->getPath()};
            for (auto const& [existing, _] : roots) {
                ConcretePathStringView existingView{existing};
                auto existingIsPrefix = existingView.isPrefixOf(normalizedView);
                if (!existingIsPrefix) {
                    return std::unexpected(existingIsPrefix.error());
                }
                auto normalizedIsPrefix = normalizedView.isPrefixOf(existingView);
                if (!normalizedIsPrefix) {
                    return std::unexpected(normalizedIsPrefix.error());
                }
                if (existingIsPrefix.value() || normalizedIsPrefix.value()) {
                    return std::unexpected(Error{
                        Error::Code::InvalidPermissions,
                        "History roots may not be nested without allowNestedUndo"});
                }
            }
        }
    }

    if (auto* rootNode = resolveRootNode(); !rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "UndoableSpace requires PathSpace backend"});
    }

    auto state                = std::make_shared<RootState>();
    state->rootPath           = normalized;
    state->components         = std::move(components);
    state->options            = defaultOptions;
    state->options.maxEntries = opts.maxEntries ? opts.maxEntries : state->options.maxEntries;
    state->options.maxBytesRetained =
        opts.maxBytesRetained ? opts.maxBytesRetained : state->options.maxBytesRetained;
    state->options.manualGarbageCollect = opts.manualGarbageCollect;
    state->options.allowNestedUndo      = opts.allowNestedUndo;
    state->options.persistHistory       = state->options.persistHistory || opts.persistHistory;
    if (!opts.persistenceRoot.empty()) {
        state->options.persistenceRoot = opts.persistenceRoot;
    }
    if (!opts.persistenceNamespace.empty()) {
        state->options.persistenceNamespace = opts.persistenceNamespace;
    }
    if (opts.ramCacheEntries > 0) {
        state->options.ramCacheEntries = opts.ramCacheEntries;
    }
    if (state->options.ramCacheEntries == 0) {
        state->options.ramCacheEntries = 8;
    }
    state->options.maxDiskBytes =
        opts.maxDiskBytes ? opts.maxDiskBytes : state->options.maxDiskBytes;
    if (opts.keepLatestFor.count() > 0) {
        state->options.keepLatestFor = opts.keepLatestFor;
    }
    state->options.restoreFromPersistence =
        state->options.restoreFromPersistence && opts.restoreFromPersistence;
    state->encodedRoot       = encodeRootForPersistence(state->rootPath);
    state->persistenceEnabled = state->options.persistHistory;
    state->undoStack.clear();
    state->redoStack.clear();
    state->telemetry = {};

    if (state->persistenceEnabled) {
        auto setup = ensurePersistenceSetup(*state);
        if (!setup)
            return std::unexpected(setup.error());
        auto load = loadPersistentState(*state);
        if (!load)
            return std::unexpected(load.error());
        if (state->hasPersistentState) {
            std::scoped_lock rootLock(state->mutex);
            auto restore = restoreRootFromPersistence(*state);
            if (!restore)
                return std::unexpected(restore.error());
            applyRamCachePolicyLocked(*state);
            updateCacheTelemetryLocked(*state);
            std::scoped_lock lock(rootsMutex);
            roots.emplace(state->rootPath, std::move(state));
            return {};
        }
    }

    {
        std::scoped_lock rootLock(state->mutex);
        auto snapshot = captureSnapshotLocked(*state);
        if (!snapshot) {
            return std::unexpected(snapshot.error());
        }
        state->liveSnapshot = snapshot.value();
        auto metrics        = state->prototype.analyze(state->liveSnapshot);
        state->liveBytes    = metrics.payloadBytes;
    }

    state->stateDirty = state->persistenceEnabled;
    updateCacheTelemetryLocked(*state);
    if (state->persistenceEnabled) {
        auto persist = persistStacksLocked(*state, true);
        if (!persist)
            return std::unexpected(persist.error());
    } else {
        updateDiskTelemetryLocked(*state);
    }

    {
        std::scoped_lock lock(rootsMutex);
        roots.emplace(state->rootPath, std::move(state));
    }

    return {};
}

auto UndoableSpace::disableHistory(ConcretePathStringView root) -> Expected<void> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return std::unexpected(canonical.error());
    }
    auto normalized = std::string{canonical->getPath()};
    std::unique_lock lock(rootsMutex);
    auto it = roots.find(normalized);
    if (it == roots.end()) {
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});
    }
    auto state = it->second;
    roots.erase(it);
    lock.unlock();
    if (state && state->persistenceEnabled) {
        std::error_code ec;
        std::filesystem::remove_all(state->persistencePath, ec);
    }
    return {};
}

auto UndoableSpace::findRoot(ConcretePathStringView root) const -> std::shared_ptr<RootState> {
    auto canonical = root.canonicalized();
    if (!canonical) {
        return {};
    }
    auto normalized = std::string{canonical->getPath()};
    std::scoped_lock lock(rootsMutex);
    auto it = roots.find(normalized);
    if (it == roots.end())
        return {};
    return it->second;
}

auto UndoableSpace::findRootByPath(std::string const& path) const -> std::optional<MatchedRoot> {
    ConcretePathStringView pathView{std::string_view{path}};
    auto canonical = pathView.canonicalized();
    if (!canonical) {
        return std::nullopt;
    }

    auto canonicalStr = std::string{canonical->getPath()};
    ConcretePathStringView canonicalView{canonical->getPath()};

    std::string                bestKey;
    std::shared_ptr<RootState> bestState;

    {
        std::scoped_lock lock(rootsMutex);
        for (auto const& [rootPath, state] : roots) {
            ConcretePathStringView rootView{rootPath};
            auto                  isPrefix = rootView.isPrefixOf(canonicalView);
            if (!isPrefix || !isPrefix.value()) {
                continue;
            }
            if (rootPath.size() > bestKey.size()) {
                bestKey   = rootPath;
                bestState = state;
            }
        }
    }

    if (!bestState)
        return std::nullopt;

    std::string relative;
    if (canonicalStr.size() > bestKey.size()) {
        relative = canonicalStr.substr(bestKey.size() + (bestKey == "/" ? 0 : 1));
    }
    return MatchedRoot{std::move(bestState), std::move(bestKey), std::move(relative)};
}

auto UndoableSpace::beginTransactionInternal(std::shared_ptr<RootState> const& state)
    -> Expected<TransactionGuard> {
    if (!state)
        return std::unexpected(Error{Error::Code::UnknownError, "History root missing"});

    std::unique_lock lock(state->mutex);
    auto const currentThread = std::this_thread::get_id();
    if (state->activeTransaction.has_value()) {
        auto& tx = *state->activeTransaction;
        if (tx.owner != currentThread) {
            return std::unexpected(Error{Error::Code::InvalidPermissions,
                                         "History transaction already active on another thread"});
        }
        tx.depth += 1;
    } else {
        state->activeTransaction = RootState::TransactionState{
            .owner          = currentThread,
            .depth          = 1,
            .dirty          = false,
            .snapshotBefore = state->liveSnapshot};
    }
    return TransactionGuard(*this, state, true);
}

void UndoableSpace::markTransactionDirty(RootState& state) {
    std::scoped_lock lock(state.mutex);
    if (state.activeTransaction)
        state.activeTransaction->dirty = true;
}

auto UndoableSpace::commitTransaction(RootState& state) -> Expected<void> {
    std::unique_lock lock(state.mutex);
    if (!state.activeTransaction) {
        return {};
    }
    auto currentThread = std::this_thread::get_id();
    if (state.activeTransaction->owner != currentThread) {
        return std::unexpected(Error{Error::Code::InvalidPermissions,
                                     "History transaction owned by another thread"});
    }

    auto before = state.activeTransaction->snapshotBefore;
    bool dirty  = state.activeTransaction->dirty;
    auto depth  = state.activeTransaction->depth;

    if (depth == 0) {
        state.activeTransaction.reset();
        return {};
    }

    state.activeTransaction->depth -= 1;
    if (state.activeTransaction->depth > 0) {
        return {};
    }

    state.activeTransaction.reset();

    OperationScope scope(*this, state, "commit");

    if (!dirty) {
        scope.setResult(true, "no_changes");
        return {};
    }

    auto snapshotExpected = captureSnapshotLocked(state);
    if (!snapshotExpected) {
        auto revert = applySnapshotLocked(state, before);
        if (!revert) {
            sp_log("UndoableSpace::commitTransaction rollback failed: "
                       + revert.error().message.value_or("unknown"),
                   "UndoableSpace");
        }
        state.liveSnapshot = before;
        auto beforeMetrics  = state.prototype.analyze(state.liveSnapshot);
        state.liveBytes     = beforeMetrics.payloadBytes;
        scope.setResult(false, snapshotExpected.error().message.value_or("capture_failed"));
        return std::unexpected(snapshotExpected.error());
    }

    auto latest     = snapshotExpected.value();
    auto now        = std::chrono::system_clock::now();
    auto undoBytes  = state.liveBytes;
    RootState::Entry undoEntry;
    undoEntry.snapshot  = before;
    undoEntry.bytes     = undoBytes;
    undoEntry.timestamp = now;
    undoEntry.persisted = !state.persistenceEnabled;
    undoEntry.cached    = true;
    state.undoStack.push_back(std::move(undoEntry));
    state.telemetry.undoBytes += undoBytes;

    state.liveSnapshot = latest;
    auto latestMetrics = state.prototype.analyze(latest);
    state.liveBytes    = latestMetrics.payloadBytes;
    for (auto const& redoEntry : state.redoStack) {
        if (redoEntry.persisted)
            removeEntryFiles(state, redoEntry.snapshot.generation);
    }
    state.redoStack.clear();
    state.telemetry.redoBytes = 0;
    state.stateDirty          = true;

    TrimStats trimStats{};
    if (!state.options.manualGarbageCollect) {
        trimStats = applyRetentionLocked(state, "commit");
        if (trimStats.entriesRemoved > 0) {
            scope.setResult(true, "trimmed=" + std::to_string(trimStats.entriesRemoved));
        }
    }

    applyRamCachePolicyLocked(state);
    updateCacheTelemetryLocked(state);
    auto persistResult = persistStacksLocked(state, false);
    if (!persistResult)
        return persistResult;

    return {};
}

auto UndoableSpace::captureSnapshotLocked(RootState& state)
    -> Expected<CowSubtreePrototype::Snapshot> {
    auto* rootNode = resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node* node = rootNode;
    for (auto const& component : state.components) {
        node = node->getChild(component);
        if (!node) {
            return state.prototype.emptySnapshot();
        }
    }

    std::vector<CowSubtreePrototype::Mutation> mutations;
    std::vector<std::string>                   pathComponents;
    std::optional<Error>                       failure;
    std::optional<std::string>                 failurePath;
    std::optional<std::string>                 failureReason;

    auto makeFailurePath = [&](std::vector<std::string> const& components) -> std::string {
        std::filesystem::path path(state.rootPath.empty() ? std::filesystem::path{"/"}
                                                          : std::filesystem::path{state.rootPath});
        for (auto const& component : components) {
            path /= component;
        }
        auto str = path.generic_string();
        if (str.empty()) {
            return "/";
        }
        return str;
    };

    auto gather = [&](auto&& self, Node const& current, std::vector<std::string>& components)
                    -> void {
        std::shared_ptr<const std::vector<std::uint8_t>> payloadBytes;
        {
            std::scoped_lock payloadLock(current.payloadMutex);
            if (current.nested) {
                failure       = Error{Error::Code::UnknownError, std::string(kUnsupportedNestedMessage)};
                failurePath   = makeFailurePath(components);
                failureReason = failure->message;
                return;
            }
            if (current.data) {
                if (current.data->hasExecutionPayload()) {
                    failure       = Error{Error::Code::UnknownError,
                                          std::string(kUnsupportedExecutionMessage)};
                    failurePath   = makeFailurePath(components);
                    failureReason = failure->message;
                    return;
                }
                auto bytesOpt = current.data->serializeSnapshot();
                if (!bytesOpt.has_value()) {
                    failure       = Error{Error::Code::UnknownError,
                                          std::string(kUnsupportedSerializationMessage)};
                    failurePath   = makeFailurePath(components);
                    failureReason = failure->message;
                    return;
                }
                auto rawBytes = std::make_shared<std::vector<std::uint8_t>>(bytesOpt->size());
                std::memcpy(rawBytes->data(), bytesOpt->data(), bytesOpt->size());
                payloadBytes = std::move(rawBytes);
            }
        }

        if (payloadBytes) {
            CowSubtreePrototype::Mutation mutation;
            mutation.components = components;
            mutation.payload    = CowSubtreePrototype::Payload(std::move(*payloadBytes));
            mutations.push_back(std::move(mutation));
        }

        current.children.for_each([&](auto const& kv) {
            components.push_back(kv.first);
            self(self, *kv.second, components);
            components.pop_back();
        });
    };

    gather(gather, *node, pathComponents);
    if (failure) {
        if (failurePath && failureReason) {
            recordUnsupportedPayloadLocked(state, *failurePath, *failureReason);
            std::string message = *failureReason;
            message.append(" at ");
            message.append(*failurePath);
            failure->message = std::move(message);
        }
        return std::unexpected(*failure);
    }

    auto snapshot = state.prototype.emptySnapshot();
    for (auto const& mutation : mutations) {
        snapshot = state.prototype.apply(snapshot, mutation);
    }
    return snapshot;
}

auto UndoableSpace::clearSubtree(Node& node) -> void {
    {
        std::scoped_lock lock(node.payloadMutex);
        node.data.reset();
        node.nested.reset();
    }
    std::vector<std::string> eraseList;
    node.children.for_each([&](auto const& kv) { eraseList.push_back(kv.first); });
    for (auto const& key : eraseList) {
        if (auto* child = node.getChild(key)) {
            clearSubtree(*child);
        }
        node.eraseChild(key);
    }
}

auto UndoableSpace::computeTotalBytesLocked(RootState const& state) -> std::size_t {
    return state.liveBytes + state.telemetry.undoBytes + state.telemetry.redoBytes;
}

void UndoableSpace::recordOperation(RootState& state,
                                    std::string_view type,
                                    std::chrono::steady_clock::duration duration,
                                    bool success,
                                    std::size_t undoBefore,
                                    std::size_t redoBefore,
                                    std::size_t bytesBefore,
                                    std::string const& message) {
    RootState::OperationRecord record;
    record.type            = std::string(type);
    record.timestamp       = std::chrono::system_clock::now();
    record.duration        = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    record.success         = success;
    record.undoCountBefore = undoBefore;
    record.undoCountAfter  = state.undoStack.size();
    record.redoCountBefore = redoBefore;
    record.redoCountAfter  = state.redoStack.size();
    record.bytesBefore     = bytesBefore;
    record.bytesAfter      = computeTotalBytesLocked(state);
    record.message         = message;
    state.telemetry.lastOperation = std::move(record);
}

auto UndoableSpace::applyRetentionLocked(RootState& state, std::string_view origin) -> TrimStats {
    (void)origin;
    TrimStats stats{};
    bool      trimmed    = false;
    std::size_t totalBytes = computeTotalBytesLocked(state);

    auto removeOldestUndo = [&]() -> bool {
        if (state.undoStack.empty())
            return false;
        auto entry = state.undoStack.front();
        state.undoStack.erase(state.undoStack.begin());
        if (state.persistenceEnabled && entry.persisted) {
            removeEntryFiles(state, entry.snapshot.generation);
        }
        if (state.telemetry.undoBytes >= entry.bytes)
            state.telemetry.undoBytes -= entry.bytes;
        else
            state.telemetry.undoBytes = 0;
        totalBytes = totalBytes >= entry.bytes ? totalBytes - entry.bytes : 0;
        stats.entriesRemoved += 1;
        stats.bytesRemoved += entry.bytes;
        trimmed = true;
        return true;
    };

    auto removeOldestRedo = [&]() -> bool {
        if (state.redoStack.empty())
            return false;
        auto entry = state.redoStack.front();
        state.redoStack.erase(state.redoStack.begin());
        if (state.persistenceEnabled && entry.persisted) {
            removeEntryFiles(state, entry.snapshot.generation);
        }
        if (state.telemetry.redoBytes >= entry.bytes)
            state.telemetry.redoBytes -= entry.bytes;
        else
            state.telemetry.redoBytes = 0;
        totalBytes = totalBytes >= entry.bytes ? totalBytes - entry.bytes : 0;
        stats.entriesRemoved += 1;
        stats.bytesRemoved += entry.bytes;
        trimmed = true;
        return true;
    };

    if (state.options.maxEntries > 0) {
        while (state.undoStack.size() > state.options.maxEntries) {
            if (!removeOldestUndo())
                break;
        }
        while (state.redoStack.size() > state.options.maxEntries) {
            if (!removeOldestRedo())
                break;
        }
    }

    if (state.options.maxBytesRetained > 0) {
        while (totalBytes > state.options.maxBytesRetained) {
            if (!state.undoStack.empty()) {
                if (!removeOldestUndo())
                    break;
                continue;
            }
            if (!state.redoStack.empty()) {
                if (!removeOldestRedo())
                    break;
                continue;
            }
            break;
        }
    }

    if (trimmed) {
        state.telemetry.trimOperations += 1;
        state.telemetry.trimmedEntries += stats.entriesRemoved;
        state.telemetry.trimmedBytes   += stats.bytesRemoved;
        state.telemetry.lastTrimTimestamp = std::chrono::system_clock::now();
    }

    return stats;
}

auto UndoableSpace::gatherStatsLocked(RootState const& state) const -> HistoryStats {
    HistoryStats stats;
    stats.counts.undo          = state.undoStack.size();
    stats.counts.redo          = state.redoStack.size();
    stats.bytes.total                 = computeTotalBytesLocked(state);
    stats.bytes.undo                  = state.telemetry.undoBytes;
    stats.bytes.redo                  = state.telemetry.redoBytes;
    stats.bytes.live                  = state.liveBytes;
    stats.bytes.disk                  = state.telemetry.diskBytes;
    stats.counts.manualGarbageCollect = state.options.manualGarbageCollect;
    stats.counts.diskEntries          = state.telemetry.diskEntries;
    stats.counts.cachedUndo           = state.telemetry.cachedUndo;
    stats.counts.cachedRedo           = state.telemetry.cachedRedo;
    stats.trim.operationCount = state.telemetry.trimOperations;
    stats.trim.entries        = state.telemetry.trimmedEntries;
    stats.trim.bytes          = state.telemetry.trimmedBytes;
    if (state.telemetry.lastTrimTimestamp) {
        stats.trim.lastTimestampMs = toMillis(*state.telemetry.lastTrimTimestamp);
    }
    if (state.telemetry.lastOperation) {
        HistoryLastOperation op;
        op.type            = state.telemetry.lastOperation->type;
        op.timestampMs     = toMillis(state.telemetry.lastOperation->timestamp);
        op.durationMs      = static_cast<std::uint64_t>(state.telemetry.lastOperation->duration.count());
        op.success         = state.telemetry.lastOperation->success;
        op.undoCountBefore = state.telemetry.lastOperation->undoCountBefore;
        op.undoCountAfter  = state.telemetry.lastOperation->undoCountAfter;
        op.redoCountBefore = state.telemetry.lastOperation->redoCountBefore;
        op.redoCountAfter  = state.telemetry.lastOperation->redoCountAfter;
        op.bytesBefore     = state.telemetry.lastOperation->bytesBefore;
        op.bytesAfter      = state.telemetry.lastOperation->bytesAfter;
        op.message         = state.telemetry.lastOperation->message;
        stats.lastOperation = std::move(op);
    }
    stats.unsupported.total = state.telemetry.unsupportedTotal;
    stats.unsupported.recent.reserve(state.telemetry.unsupportedLog.size());
    for (auto const& entry : state.telemetry.unsupportedLog) {
        HistoryUnsupportedRecord record;
        record.path            = entry.path;
        record.reason          = entry.reason;
        record.occurrences     = entry.occurrences;
        record.lastTimestampMs = toMillis(entry.timestamp);
        stats.unsupported.recent.push_back(std::move(record));
    }
    return stats;
}

auto UndoableSpace::readHistoryValue(MatchedRoot const& matchedRoot,
                                      std::string const& relativePath,
                                      InputMetadata const& metadata,
                                      void* obj) -> std::optional<Error> {
    auto state = matchedRoot.state;
    if (!state) {
        return Error{Error::Code::UnknownError, "History root missing"};
    }

    std::unique_lock lock(state->mutex);
    auto stats = gatherStatsLocked(*state);

    auto assign = [&](auto value, std::string_view descriptor) -> std::optional<Error> {
        using ValueT = std::decay_t<decltype(value)>;
        if (!metadata.typeInfo || *metadata.typeInfo != typeid(ValueT)) {
            return Error{Error::Code::InvalidType,
                         std::string("History telemetry path ") + std::string(descriptor)
                             + " expects type " + typeid(ValueT).name()};
        }
        if (obj == nullptr) {
            return Error{Error::Code::MalformedInput, "Output pointer is null"};
        }
        *static_cast<ValueT*>(obj) = value;
        return std::nullopt;
    };

    if (relativePath == "_history/stats") {
        return assign(stats, relativePath);
    }
    if (relativePath == "_history/stats/undoCount") {
        return assign(stats.counts.undo, relativePath);
    }
    if (relativePath == "_history/stats/redoCount") {
        return assign(stats.counts.redo, relativePath);
    }
    if (relativePath == "_history/stats/undoBytes") {
        return assign(stats.bytes.undo, relativePath);
    }
    if (relativePath == "_history/stats/redoBytes") {
        return assign(stats.bytes.redo, relativePath);
    }
    if (relativePath == "_history/stats/liveBytes") {
        return assign(stats.bytes.live, relativePath);
    }
    if (relativePath == "_history/stats/bytesRetained") {
        return assign(stats.bytes.total, relativePath);
    }
    if (relativePath == "_history/stats/manualGcEnabled") {
        return assign(stats.counts.manualGarbageCollect, relativePath);
    }
    if (relativePath == "_history/stats/trimOperationCount") {
        return assign(stats.trim.operationCount, relativePath);
    }
    if (relativePath == "_history/stats/trimmedEntries") {
        return assign(stats.trim.entries, relativePath);
    }
    if (relativePath == "_history/stats/trimmedBytes") {
        return assign(stats.trim.bytes, relativePath);
    }
    if (relativePath == "_history/stats/lastTrimTimestampMs") {
        return assign(stats.trim.lastTimestampMs, relativePath);
    }
    if (relativePath == "_history/head/generation") {
        return assign(state->liveSnapshot.generation, relativePath);
    }

    if (relativePath.starts_with("_history/lastOperation")) {
        if (!stats.lastOperation) {
            return Error{Error::Code::NoObjectFound, "No history operation recorded"};
        }
        auto const& op = *stats.lastOperation;
        if (relativePath == "_history/lastOperation/type") {
            return assign(op.type, relativePath);
        }
        if (relativePath == "_history/lastOperation/timestampMs") {
            return assign(op.timestampMs, relativePath);
        }
        if (relativePath == "_history/lastOperation/durationMs") {
            return assign(op.durationMs, relativePath);
        }
        if (relativePath == "_history/lastOperation/success") {
            return assign(op.success, relativePath);
        }
        if (relativePath == "_history/lastOperation/undoCountBefore") {
            return assign(op.undoCountBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/undoCountAfter") {
            return assign(op.undoCountAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/redoCountBefore") {
            return assign(op.redoCountBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/redoCountAfter") {
            return assign(op.redoCountAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/bytesBefore") {
            return assign(op.bytesBefore, relativePath);
        }
        if (relativePath == "_history/lastOperation/bytesAfter") {
            return assign(op.bytesAfter, relativePath);
        }
        if (relativePath == "_history/lastOperation/message") {
            return assign(op.message, relativePath);
        }
    }

    if (relativePath == "_history/unsupported") {
        return assign(stats.unsupported, relativePath);
    }
    if (relativePath == "_history/unsupported/totalCount") {
        return assign(stats.unsupported.total, relativePath);
    }
    if (relativePath == "_history/unsupported/recentCount") {
        return assign(stats.unsupported.recent.size(), relativePath);
    }

    constexpr std::string_view unsupportedRecentPrefix = "_history/unsupported/recent/";
    if (relativePath.starts_with(unsupportedRecentPrefix)) {
        auto parseIndex = [](std::string_view value) -> std::optional<std::size_t> {
            std::size_t index = 0;
            auto        begin = value.data();
            auto        end   = value.data() + value.size();
            auto        res   = std::from_chars(begin, end, index);
            if (res.ec != std::errc{} || res.ptr != end) {
                return std::nullopt;
            }
            return index;
        };

        std::string_view suffix = std::string_view(relativePath).substr(unsupportedRecentPrefix.size());
        auto             slash  = suffix.find('/');
        auto             indexView = suffix.substr(0, slash);
        auto             indexOpt  = parseIndex(indexView);
        if (!indexOpt) {
            return Error{Error::Code::InvalidPath, "Unsupported history record index"};
        }
        auto index = *indexOpt;
        if (index >= stats.unsupported.recent.size()) {
            return Error{Error::Code::NoObjectFound, "Unsupported history record not found"};
        }
        auto const& record = stats.unsupported.recent[index];
        if (slash == std::string_view::npos) {
            return assign(record, relativePath);
        }
        auto field = suffix.substr(slash + 1);
        if (field == "path") {
            return assign(record.path, relativePath);
        }
        if (field == "reason") {
            return assign(record.reason, relativePath);
        }
        if (field == "occurrences") {
            return assign(record.occurrences, relativePath);
        }
        if (field == "timestampMs") {
            return assign(record.lastTimestampMs, relativePath);
        }
    }

    return Error{Error::Code::NotFound, std::string("Unsupported history telemetry path: ") + relativePath};
}

void UndoableSpace::recordUnsupportedPayloadLocked(RootState& state,
                                                   std::string const& path,
                                                   std::string const& reason) {
    auto now = std::chrono::system_clock::now();
    state.telemetry.unsupportedTotal++;

    auto it = std::find_if(state.telemetry.unsupportedLog.begin(),
                           state.telemetry.unsupportedLog.end(),
                           [&](auto const& entry) {
                               return entry.path == path && entry.reason == reason;
                           });
    if (it != state.telemetry.unsupportedLog.end()) {
        it->occurrences += 1;
        it->timestamp = now;
        if (std::next(it) != state.telemetry.unsupportedLog.end()) {
            auto updated = std::move(*it);
            state.telemetry.unsupportedLog.erase(it);
            state.telemetry.unsupportedLog.push_back(std::move(updated));
        }
        return;
    }

    RootState::Telemetry::UnsupportedRecord record;
    record.path        = path;
    record.reason      = reason;
    record.timestamp   = now;
    record.occurrences = 1;
    state.telemetry.unsupportedLog.push_back(std::move(record));
    if (state.telemetry.unsupportedLog.size() > kMaxUnsupportedLogEntries) {
        state.telemetry.unsupportedLog.erase(state.telemetry.unsupportedLog.begin());
    }
}

auto UndoableSpace::ensurePersistenceSetup(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    auto baseRoot = persistenceRootPath(state.options);
    auto nsDir    = state.options.persistenceNamespace.empty() ? std::filesystem::path(spaceUuid)
                                                               : std::filesystem::path(state.options.persistenceNamespace);

    state.persistencePath = baseRoot / nsDir / state.encodedRoot;
    state.entriesPath     = state.persistencePath / "entries";

    std::error_code ec;
    std::filesystem::create_directories(state.entriesPath, ec);
    if (ec) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to create persistence directories"});
    }

    state.stateDirty         = false;
    state.hasPersistentState = std::filesystem::exists(stateMetaPath(state));
    return {};
}

auto UndoableSpace::loadPersistentState(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    auto statePath = stateMetaPath(state);
    auto metaText  = readTextFile(statePath);
    if (!metaText) {
        if (metaText.error().code == Error::Code::NotFound) {
            state.hasPersistentState = false;
            return {};
        }
        return std::unexpected(metaText.error());
    }

    auto stateMetaExpected = parseStateMeta(*metaText);
    if (!stateMetaExpected)
        return std::unexpected(stateMetaExpected.error());
    auto stateMeta = std::move(stateMetaExpected.value());

    state.options.manualGarbageCollect = stateMeta.manualGc;
    if (stateMeta.ramCacheEntries > 0)
        state.options.ramCacheEntries = stateMeta.ramCacheEntries;
    if (state.options.ramCacheEntries == 0)
        state.options.ramCacheEntries = 8;

    state.prototype = CowSubtreePrototype{};
    state.undoStack.clear();
    state.redoStack.clear();
    state.telemetry             = {};
    state.telemetry.persistenceDirty = false;

    std::uintmax_t diskBytes = 0;
    std::size_t    diskEntries = 0;

    auto liveSnapshotPath = entrySnapshotPath(state, stateMeta.liveGeneration);
    auto liveData         = readBinaryFile(liveSnapshotPath);
    if (!liveData)
        return std::unexpected(liveData.error());

    auto liveSnapshotExpected = decodeSnapshot(state.prototype,
                                               std::span<const std::uint8_t>(liveData->data(), liveData->size()));
    if (!liveSnapshotExpected)
        return std::unexpected(liveSnapshotExpected.error());

    state.liveSnapshot = std::move(liveSnapshotExpected.value());
    state.liveBytes    = state.prototype.analyze(state.liveSnapshot).payloadBytes;

    auto liveMeta = readTextFile(entryMetaPath(state, stateMeta.liveGeneration));
    if (liveMeta) {
        auto entryMetaParsed = parseEntryMeta(*liveMeta);
        if (entryMetaParsed) {
            RootState::OperationRecord record;
            record.type        = "restore";
            record.timestamp   = std::chrono::system_clock::time_point{
                std::chrono::milliseconds(entryMetaParsed->timestampMs)};
            record.duration    = std::chrono::milliseconds{0};
            record.success     = true;
            record.undoCountBefore = 0;
            record.undoCountAfter  = 0;
            record.redoCountBefore = 0;
            record.redoCountAfter  = 0;
            record.bytesBefore     = 0;
            record.bytesAfter      = state.liveBytes;
            record.message         = "persistence_restore";
            state.telemetry.lastOperation = std::move(record);
        }
    }

    auto accumulateFileSize = [&](std::filesystem::path const& path) {
        std::error_code ec;
        auto            size = std::filesystem::file_size(path, ec);
        if (!ec)
            diskBytes += size;
    };

    accumulateFileSize(liveSnapshotPath);
    accumulateFileSize(entryMetaPath(state, stateMeta.liveGeneration));
    diskEntries += 1;

    auto loadEntryList = [&](std::vector<std::size_t> const& generations,
                             std::vector<RootState::Entry>& stack,
                             std::size_t&                   byteCounter) -> Expected<void> {
        for (auto generation : generations) {
            auto metaPath = entryMetaPath(state, generation);
            auto metaText = readTextFile(metaPath);
            if (!metaText)
                return std::unexpected(metaText.error());
            auto metaParsed = parseEntryMeta(*metaText);
            if (!metaParsed)
                return std::unexpected(metaParsed.error());

            RootState::Entry entry;
            entry.snapshot.generation = generation;
            entry.bytes               = metaParsed->bytes;
            entry.timestamp           = std::chrono::system_clock::time_point{
                std::chrono::milliseconds(metaParsed->timestampMs)};
            entry.persisted = true;
            entry.cached    = false;

            byteCounter += entry.bytes;
            stack.push_back(std::move(entry));

            accumulateFileSize(entrySnapshotPath(state, generation));
            accumulateFileSize(metaPath);
            diskEntries += 1;
        }
        return Expected<void>{};
    };

    std::size_t undoBytes = 0;
    std::size_t redoBytes = 0;

    if (auto loadUndo = loadEntryList(stateMeta.undoGenerations, state.undoStack, undoBytes); !loadUndo)
        return loadUndo;
    if (auto loadRedo = loadEntryList(stateMeta.redoGenerations, state.redoStack, redoBytes); !loadRedo)
        return loadRedo;

    state.telemetry.undoBytes = undoBytes;
    state.telemetry.redoBytes = redoBytes;

    std::size_t maxGeneration = stateMeta.liveGeneration;
    for (auto g : stateMeta.undoGenerations)
        maxGeneration = std::max(maxGeneration, g);
    for (auto g : stateMeta.redoGenerations)
        maxGeneration = std::max(maxGeneration, g);

    state.prototype.setNextGeneration(maxGeneration + 1);

    state.telemetry.diskBytes   = static_cast<std::size_t>(diskBytes);
    state.telemetry.diskEntries = diskEntries;
    state.hasPersistentState    = true;
    state.stateDirty            = false;

    return {};
}

auto UndoableSpace::restoreRootFromPersistence(RootState& state) -> Expected<void> {
    if (!state.persistenceEnabled || !state.hasPersistentState || !state.options.restoreFromPersistence)
        return {};
    return applySnapshotLocked(state, state.liveSnapshot);
}

auto UndoableSpace::persistStacksLocked(RootState& state, bool forceFsync) -> Expected<void> {
    if (!state.persistenceEnabled)
        return {};

    auto flushNow = forceFsync || !state.options.manualGarbageCollect;
    std::error_code ec;
    std::filesystem::create_directories(state.entriesPath, ec);
    if (ec) {
        return std::unexpected(Error{Error::Code::UnknownError, "Failed to create persistence directory"});
    }

    auto persistSnapshot = [&](CowSubtreePrototype::Snapshot const& snapshot,
                               std::chrono::system_clock::time_point timestamp,
                               std::size_t                            bytesEstimate) -> Expected<void> {
        auto encoded = encodeSnapshot(snapshot);
        if (!encoded)
            return std::unexpected(encoded.error());
        auto snapshotPath = entrySnapshotPath(state, snapshot.generation);
        auto metaPath     = entryMetaPath(state, snapshot.generation);
        auto span         = std::span<const std::uint8_t>(encoded->data(), encoded->size());
        if (auto write = writeFileAtomic(snapshotPath, span, flushNow, true); !write)
            return write;

        EntryMetadata meta;
        meta.generation  = snapshot.generation;
        meta.bytes       = bytesEstimate;
        meta.timestampMs = toMillis(timestamp);

        auto metaText = encodeEntryMeta(meta);
        if (auto writeMeta = writeTextFileAtomic(metaPath, metaText, flushNow); !writeMeta)
            return writeMeta;
        return Expected<void>{};
    };

    auto persistEntry = [&](RootState::Entry& entry) -> Expected<void> {
        if (entry.persisted)
            return Expected<void>{};
        if (!entry.cached) {
            return std::unexpected(Error{Error::Code::UnknownError,
                                         "Attempted to persist history entry without cache"});
        }
        auto result = persistSnapshot(entry.snapshot, entry.timestamp, entry.bytes);
        if (!result)
            return result;
        entry.persisted = true;
        return result;
    };

    for (auto& entry : state.undoStack) {
        auto result = persistEntry(entry);
        if (!result)
            return result;
    }

    for (auto& entry : state.redoStack) {
        auto result = persistEntry(entry);
        if (!result)
            return result;
    }

    if (state.stateDirty || forceFsync) {
        auto livePersist = persistSnapshot(state.liveSnapshot,
                                           std::chrono::system_clock::now(),
                                           state.liveBytes);
        if (!livePersist)
            return livePersist;

        StateMetadata stateMeta;
        stateMeta.liveGeneration  = state.liveSnapshot.generation;
        stateMeta.manualGc        = state.options.manualGarbageCollect;
        stateMeta.ramCacheEntries = state.options.ramCacheEntries;
        stateMeta.undoGenerations.reserve(state.undoStack.size());
        stateMeta.redoGenerations.reserve(state.redoStack.size());
        for (auto const& entry : state.undoStack)
            stateMeta.undoGenerations.push_back(entry.snapshot.generation);
        for (auto const& entry : state.redoStack)
            stateMeta.redoGenerations.push_back(entry.snapshot.generation);

        auto stateText = encodeStateMeta(stateMeta);
        if (auto writeState = writeTextFileAtomic(stateMetaPath(state), stateText, flushNow); !writeState)
            return writeState;

        state.stateDirty = false;
    }

    updateDiskTelemetryLocked(state);

    if (flushNow) {
        state.telemetry.persistenceDirty = false;
    } else {
        state.telemetry.persistenceDirty = true;
    }

    return {};
}

auto UndoableSpace::loadEntrySnapshotLocked(RootState& state, std::size_t stackIndex, bool undoStack)
    -> Expected<void> {
    auto& stack = undoStack ? state.undoStack : state.redoStack;
    if (stackIndex >= stack.size()) {
        return std::unexpected(Error{Error::Code::UnknownError, "History entry index out of range"});
    }
    auto& entry = stack[stackIndex];
    if (entry.cached)
        return {};
    auto path = entrySnapshotPath(state, entry.snapshot.generation);
    auto data = readBinaryFile(path);
    if (!data)
        return std::unexpected(data.error());
    CowSubtreePrototype loaderPrototype;
    auto snapshotExpected = decodeSnapshot(loaderPrototype, std::span<const std::uint8_t>(data->data(), data->size()));
    if (!snapshotExpected)
        return std::unexpected(snapshotExpected.error());
    entry.snapshot = std::move(snapshotExpected.value());
    entry.cached   = true;
    return {};
}

auto UndoableSpace::applyRamCachePolicyLocked(RootState& state) -> void {
    auto enforceStack = [&](std::vector<RootState::Entry>& stack, bool undoStack) {
        std::size_t limit = state.options.ramCacheEntries;
        if (limit == 0) {
            for (auto& entry : stack) {
                if (entry.cached) {
                    entry.snapshot.root.reset();
                    entry.cached = false;
                }
            }
            return;
        }

        std::size_t cached = 0;
        for (std::size_t idx = stack.size(); idx-- > 0;) {
            auto& entry = stack[idx];
            if (cached < limit) {
                if (!entry.cached && entry.persisted) {
                    auto load = loadEntrySnapshotLocked(state, idx, undoStack);
                    if (!load) {
                        sp_log("Failed to load history snapshot for caching: "
                                   + load.error().message.value_or("unknown"),
                               "UndoableSpace");
                    }
                }
                cached += 1;
            } else if (entry.cached) {
                entry.snapshot.root.reset();
                entry.cached = false;
            }
        }
    };

    enforceStack(state.undoStack, true);
    enforceStack(state.redoStack, false);
    updateCacheTelemetryLocked(state);
}

auto UndoableSpace::updateCacheTelemetryLocked(RootState& state) -> void {
    state.telemetry.cachedUndo = 0;
    for (auto const& entry : state.undoStack) {
        if (entry.cached)
            state.telemetry.cachedUndo += 1;
    }
    state.telemetry.cachedRedo = 0;
    for (auto const& entry : state.redoStack) {
        if (entry.cached)
            state.telemetry.cachedRedo += 1;
    }
}

auto UndoableSpace::updateDiskTelemetryLocked(RootState& state) -> void {
    if (!state.persistenceEnabled) {
        state.telemetry.diskBytes   = 0;
        state.telemetry.diskEntries = 0;
        return;
    }

    std::uintmax_t totalBytes = 0;
    std::size_t    count      = 0;

    auto accumulate = [&](std::filesystem::path const& path) {
        std::error_code ec;
        auto            size = std::filesystem::file_size(path, ec);
        if (!ec)
            totalBytes += size;
    };

    auto addEntryFiles = [&](std::size_t generation, bool persisted) {
        if (!persisted)
            return;
        accumulate(entrySnapshotPath(state, generation));
        accumulate(entryMetaPath(state, generation));
        count += 1;
    };

    addEntryFiles(state.liveSnapshot.generation, true);
    for (auto const& entry : state.undoStack) {
        addEntryFiles(entry.snapshot.generation, entry.persisted);
    }
    for (auto const& entry : state.redoStack) {
        addEntryFiles(entry.snapshot.generation, entry.persisted);
    }

    accumulate(stateMetaPath(state));

    state.telemetry.diskBytes   = static_cast<std::size_t>(totalBytes);
    state.telemetry.diskEntries = count;
}

auto UndoableSpace::encodeRootForPersistence(std::string const& rootPath) const -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0');
    for (unsigned char c : rootPath) {
        oss << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();
}

auto UndoableSpace::persistenceRootPath(HistoryOptions const& opts) const -> std::filesystem::path {
    if (!opts.persistenceRoot.empty()) {
        return std::filesystem::path(opts.persistenceRoot);
    }
    if (!defaultOptions.persistenceRoot.empty()) {
        return std::filesystem::path(defaultOptions.persistenceRoot);
    }
    return defaultPersistenceRoot();
}

auto UndoableSpace::defaultPersistenceRoot() const -> std::filesystem::path {
    if (auto* env = std::getenv("PATHSPACE_HISTORY_ROOT"); env && *env) {
        return std::filesystem::path(env);
    }
    if (auto* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
        return std::filesystem::path(tmp) / "pathspace_history";
    }
    return std::filesystem::temp_directory_path() / "pathspace_history";
}

auto UndoableSpace::entrySnapshotPath(RootState const& state, std::size_t generation) const
    -> std::filesystem::path {
    return state.entriesPath / (snapshotFileStem(generation) + ".snapshot");
}

auto UndoableSpace::entryMetaPath(RootState const& state, std::size_t generation) const
    -> std::filesystem::path {
    return state.entriesPath / (snapshotFileStem(generation) + ".meta");
}

auto UndoableSpace::stateMetaPath(RootState const& state) const -> std::filesystem::path {
    return state.persistencePath / "state.meta";
}

auto UndoableSpace::removeEntryFiles(RootState& state, std::size_t generation) -> void {
    if (!state.persistenceEnabled)
        return;
    removePathIfExists(entrySnapshotPath(state, generation));
    removePathIfExists(entryMetaPath(state, generation));
}

auto UndoableSpace::applySnapshotLocked(RootState& state,
                                        CowSubtreePrototype::Snapshot const& snapshot)
    -> Expected<void> {
    auto* rootNode = resolveRootNode();
    if (!rootNode) {
        return std::unexpected(Error{Error::Code::UnknownError, "PathSpace backend unavailable"});
    }

    Node* node = rootNode;
    for (auto const& component : state.components) {
        node = &node->getOrCreateChild(component);
    }

    if (!snapshot.valid()) {
        clearSubtree(*node);
        return {};
    }

    auto applyNode = [&](auto&& self, Node& target, CowSubtreePrototype::Node const& source)
        -> Expected<void> {
        {
            std::scoped_lock lock(target.payloadMutex);
            target.nested.reset();
            if (source.payload.bytes) {
                auto nodeDataOpt =
                    NodeData::deserializeSnapshot(std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(source.payload.bytes->data()),
                        source.payload.bytes->size()});
                if (!nodeDataOpt.has_value()) {
                    return std::unexpected(
                        Error{Error::Code::UnknownError, "Failed to restore node payload"});
                }
                target.data = std::make_unique<NodeData>(std::move(*nodeDataOpt));
            } else {
                target.data.reset();
            }
        }

        std::unordered_map<std::string, bool> keep;
        for (auto const& [childName, childNode] : source.children) {
            keep.emplace(childName, true);
            Node& childTarget = target.getOrCreateChild(childName);
            auto  result      = self(self, childTarget, *childNode);
            if (!result)
                return result;
        }

        std::vector<std::string> toErase;
        target.children.for_each([&](auto const& kv) {
            if (!keep.contains(kv.first)) {
                toErase.push_back(kv.first);
            }
        });
        for (auto const& key : toErase) {
            if (auto* child = target.getChild(key)) {
                clearSubtree(*child);
            }
            target.eraseChild(key);
        }
        return Expected<void>{};
    };

    return applyNode(applyNode, *node, *snapshot.root);
}

auto UndoableSpace::interpretSteps(InputData const& data) const -> std::size_t {
    if (!data.metadata.typeInfo || data.obj == nullptr)
        return 1;

    auto interpretUnsigned = [&](auto ptr) -> std::size_t {
        using T = std::remove_pointer_t<decltype(ptr)>;
        if (*ptr <= 0)
            return 1;
        return static_cast<std::size_t>(*ptr);
    };

    if (*data.metadata.typeInfo == typeid(int)) {
        return interpretUnsigned(static_cast<int const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(unsigned int)) {
        return interpretUnsigned(static_cast<unsigned int const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::size_t)) {
        return interpretUnsigned(static_cast<std::size_t const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::uint64_t)) {
        return interpretUnsigned(static_cast<std::uint64_t const*>(data.obj));
    }
    if (*data.metadata.typeInfo == typeid(std::int64_t)) {
        return interpretUnsigned(static_cast<std::int64_t const*>(data.obj));
    }

    return 1;
}

auto UndoableSpace::handleControlInsert(MatchedRoot const& matchedRoot,
                                        std::string const& command,
                                         InputData const& data) -> InsertReturn {
    InsertReturn ret;
    if (command == "_history/undo") {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = undo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == "_history/redo") {
        auto steps = interpretSteps(data);
        ConcretePathStringView rootView{matchedRoot.key};
        if (auto result = redo(rootView, steps); !result) {
            ret.errors.push_back(result.error());
        }
        return ret;
    }
    if (command == "_history/garbage_collect") {
        auto state = matchedRoot.state;
        std::unique_lock lock(state->mutex);
        OperationScope scope(*this, *state, "garbage_collect");
        auto stats = applyRetentionLocked(*state, "manual");
        if (stats.entriesRemoved == 0) {
            scope.setResult(true, "no_trim");
        } else {
            scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
        }
        state->stateDirty = true;
        applyRamCachePolicyLocked(*state);
        updateCacheTelemetryLocked(*state);
        auto persist = persistStacksLocked(*state, true);
        if (!persist) {
            ret.errors.push_back(persist.error());
        }
        return ret;
    }
    if (command == "_history/set_manual_garbage_collect") {
        bool manual = false;
        if (data.obj && data.metadata.typeInfo) {
            if (*data.metadata.typeInfo == typeid(bool)) {
                manual = *static_cast<bool const*>(data.obj);
            }
        }
        auto state = matchedRoot.state;
        std::scoped_lock lock(state->mutex);
        state->options.manualGarbageCollect = manual;
        state->stateDirty                   = true;
        auto persist = persistStacksLocked(*state, !manual);
        if (!persist) {
            ret.errors.push_back(persist.error());
        }
        return ret;
    }
    ret.errors.push_back(
        Error{Error::Code::UnknownError, "Unsupported history control command"});
    return ret;
}

auto UndoableSpace::in(Iterator const& path, InputData const& data) -> InsertReturn {
    auto fullPath = path.toString();
    auto matched  = findRootByPath(fullPath);
    if (!matched.has_value()) {
        return inner->in(path, data);
    }

    if (!matched->relativePath.empty() && matched->relativePath.starts_with("_history")) {
        return handleControlInsert(*matched, matched->relativePath, data);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        InsertReturn ret;
        ret.errors.push_back(guardExpected.error());
        return ret;
    }

    auto guard  = std::move(guardExpected.value());
    auto result = inner->in(path, data);
    if (result.errors.empty()) {
        guard.markDirty();
    }
    if (auto commit = guard.commit(); !commit) {
        result.errors.push_back(commit.error());
    }
    return result;
}

auto UndoableSpace::out(Iterator const& path,
                        InputMetadata const& inputMetadata,
                        Out const& options,
                        void* obj) -> std::optional<Error> {
    auto fullPath = path.toString();
    auto matched  = findRootByPath(fullPath);

    if (!options.doPop) {
        if (matched.has_value() && !matched->relativePath.empty()
            && matched->relativePath.starts_with("_history")) {
            return readHistoryValue(*matched, matched->relativePath, inputMetadata, obj);
        }
        return inner->out(path, inputMetadata, options, obj);
    }

    if (!matched.has_value()) {
        return inner->out(path, inputMetadata, options, obj);
    }

    auto guardExpected = beginTransactionInternal(matched->state);
    if (!guardExpected) {
        return guardExpected.error();
    }

    auto guard = std::move(guardExpected.value());
    auto error = inner->out(path, inputMetadata, options, obj);
    if (!error.has_value()) {
        guard.markDirty();
    }
    if (auto commit = guard.commit(); !commit) {
        return commit.error();
    }
    return error;
}

auto UndoableSpace::undo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot undo while transaction open"});
    }
    if (steps == 0)
        steps = 1;

    while (steps-- > 0) {
        OperationScope scope(*this, *state, "undo");
        if (state->undoStack.empty()) {
            scope.setResult(false, "empty");
            return std::unexpected(Error{Error::Code::NoObjectFound, "Nothing to undo"});
        }

        auto index = state->undoStack.size() - 1;
        if (!state->undoStack[index].cached && state->undoStack[index].persisted) {
            auto load = loadEntrySnapshotLocked(*state, index, true);
            if (!load) {
                scope.setResult(false, "load_failed");
                return std::unexpected(load.error());
            }
        }
        auto entry = state->undoStack.back();
        state->undoStack.pop_back();
        if (state->telemetry.undoBytes >= entry.bytes) {
            state->telemetry.undoBytes -= entry.bytes;
        } else {
            state->telemetry.undoBytes = 0;
        }

        auto currentSnapshot = state->liveSnapshot;
        auto currentBytes    = state->liveBytes;

        auto applyResult = applySnapshotLocked(*state, entry.snapshot);
        if (!applyResult) {
            auto revert = applySnapshotLocked(*state, currentSnapshot);
            (void)revert;
            state->liveSnapshot = currentSnapshot;
            state->liveBytes    = currentBytes;
            state->undoStack.push_back(std::move(entry));
            state->telemetry.undoBytes += entry.bytes;
            scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
            return std::unexpected(applyResult.error());
        }

        RootState::Entry redoEntry;
        redoEntry.snapshot  = currentSnapshot;
        redoEntry.bytes     = currentBytes;
        redoEntry.timestamp = std::chrono::system_clock::now();
        redoEntry.persisted = state->persistenceEnabled;
        redoEntry.cached    = true;
        state->redoStack.push_back(std::move(redoEntry));
        state->telemetry.redoBytes += currentBytes;

        state->liveSnapshot = entry.snapshot;
        state->liveBytes    = entry.bytes;

        if (!state->options.manualGarbageCollect) {
            applyRetentionLocked(*state, "undo");
        }
    }

    state->stateDirty = true;
    applyRamCachePolicyLocked(*state);
    updateCacheTelemetryLocked(*state);
    auto persistResult = persistStacksLocked(*state, false);
    if (!persistResult)
        return persistResult;

    return {};
}

auto UndoableSpace::redo(ConcretePathStringView root, std::size_t steps) -> Expected<void> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    if (state->activeTransaction) {
        return std::unexpected(
            Error{Error::Code::InvalidPermissions, "Cannot redo while transaction open"});
    }
    if (steps == 0)
        steps = 1;

    while (steps-- > 0) {
        OperationScope scope(*this, *state, "redo");
        if (state->redoStack.empty()) {
            scope.setResult(false, "empty");
            return std::unexpected(Error{Error::Code::NoObjectFound, "Nothing to redo"});
        }

        auto index = state->redoStack.size() - 1;
        if (!state->redoStack[index].cached && state->redoStack[index].persisted) {
            auto load = loadEntrySnapshotLocked(*state, index, false);
            if (!load) {
                scope.setResult(false, "load_failed");
                return std::unexpected(load.error());
            }
        }
        auto entry = state->redoStack.back();
        state->redoStack.pop_back();
        if (state->telemetry.redoBytes >= entry.bytes) {
            state->telemetry.redoBytes -= entry.bytes;
        } else {
            state->telemetry.redoBytes = 0;
        }

        auto currentSnapshot = state->liveSnapshot;
        auto currentBytes    = state->liveBytes;

        auto applyResult = applySnapshotLocked(*state, entry.snapshot);
        if (!applyResult) {
            auto revert = applySnapshotLocked(*state, currentSnapshot);
            (void)revert;
            state->liveSnapshot = currentSnapshot;
            state->liveBytes    = currentBytes;
            state->redoStack.push_back(std::move(entry));
            state->telemetry.redoBytes += entry.bytes;
            scope.setResult(false, applyResult.error().message.value_or("apply_failed"));
            return std::unexpected(applyResult.error());
        }

        RootState::Entry undoEntry;
        undoEntry.snapshot  = currentSnapshot;
        undoEntry.bytes     = currentBytes;
        undoEntry.timestamp = std::chrono::system_clock::now();
        undoEntry.persisted = state->persistenceEnabled;
        undoEntry.cached    = true;
        state->undoStack.push_back(std::move(undoEntry));
        state->telemetry.undoBytes += currentBytes;

        state->liveSnapshot = entry.snapshot;
        state->liveBytes    = entry.bytes;

        if (!state->options.manualGarbageCollect) {
            applyRetentionLocked(*state, "redo");
        }
    }

    state->stateDirty = true;
    applyRamCachePolicyLocked(*state);
    updateCacheTelemetryLocked(*state);
    auto persistResult = persistStacksLocked(*state, false);
    if (!persistResult)
        return persistResult;

    return {};
}

auto UndoableSpace::trimHistory(ConcretePathStringView root, TrimPredicate predicate)
    -> Expected<TrimStats> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::unique_lock lock(state->mutex);
    OperationScope scope(*this, *state, "trim");

    TrimStats stats{};
    if (!predicate) {
        scope.setResult(true, "no_predicate");
        return stats;
    }

    std::vector<RootState::Entry> kept;
    kept.reserve(state->undoStack.size());
    std::size_t bytesRemoved = 0;
    for (std::size_t i = 0; i < state->undoStack.size(); ++i) {
        auto& entry = state->undoStack[i];
        if (predicate(i)) {
            stats.entriesRemoved += 1;
            bytesRemoved += entry.bytes;
            if (state->persistenceEnabled && entry.persisted) {
                removeEntryFiles(*state, entry.snapshot.generation);
            }
        } else {
            kept.push_back(entry);
        }
    }

    if (stats.entriesRemoved == 0) {
        scope.setResult(true, "no_trim");
        return stats;
    }

    stats.bytesRemoved = bytesRemoved;
    state->undoStack    = std::move(kept);
    if (state->telemetry.undoBytes >= bytesRemoved) {
        state->telemetry.undoBytes -= bytesRemoved;
    } else {
        state->telemetry.undoBytes = 0;
    }

    state->telemetry.trimOperations += 1;
    state->telemetry.trimmedEntries += stats.entriesRemoved;
    state->telemetry.trimmedBytes   += stats.bytesRemoved;
    state->telemetry.lastTrimTimestamp = std::chrono::system_clock::now();

    scope.setResult(true, "trimmed=" + std::to_string(stats.entriesRemoved));
    state->stateDirty = true;
    applyRamCachePolicyLocked(*state);
    updateCacheTelemetryLocked(*state);
    auto persistResult = persistStacksLocked(*state, false);
    if (!persistResult)
        return std::unexpected(persistResult.error());
    return stats;
}

auto UndoableSpace::getHistoryStats(ConcretePathStringView root) const -> Expected<HistoryStats> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    std::scoped_lock lock(state->mutex);
    return gatherStatsLocked(*state);
}

auto UndoableSpace::beginTransaction(ConcretePathStringView root) -> Expected<HistoryTransaction> {
    auto state = findRoot(root);
    if (!state)
        return std::unexpected(Error{Error::Code::NotFound, "History root not enabled"});

    auto guardExpected = beginTransactionInternal(state);
    if (!guardExpected)
        return std::unexpected(guardExpected.error());

    auto guard = std::move(guardExpected.value());
    guard.deactivate();
    return HistoryTransaction(*this, std::move(state));
}

auto UndoableSpace::shutdown() -> void {
    if (inner)
        inner->shutdown();
}

auto UndoableSpace::notify(std::string const& notificationPath) -> void {
    if (inner)
        inner->notify(notificationPath);
}

} // namespace SP::History
