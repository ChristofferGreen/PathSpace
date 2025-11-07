#include <pathspace/history/CowSubtreePrototype.hpp>
#include <pathspace/history/UndoHistoryMetadata.hpp>
#include <pathspace/history/UndoHistoryInspection.hpp>
#include <pathspace/history/UndoHistoryUtils.hpp>
#include <pathspace/history/UndoSnapshotCodec.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace SP;
using namespace SP::History;

namespace {

struct CommandLineOptions {
    std::filesystem::path rootPath;
    bool                  jsonOutput        = false;
    bool                  analyzeSnapshots  = true;
    std::optional<std::size_t> dumpGeneration;
    std::size_t           previewBytes      = 16;
    bool                  decodeSnapshot    = false;
    std::optional<std::size_t> decodeGeneration;
    bool                  diffRequested     = false;
    std::optional<std::pair<std::size_t, std::size_t>> diffGenerations;
};

enum class EntryKind {
    Live,
    Undo,
    Redo,
    Unknown
};

struct SnapshotStats {
    std::size_t uniqueNodes  = 0;
    std::size_t payloadBytes = 0;
};

struct EntryInfo {
    EntryKind               kind            = EntryKind::Unknown;
    std::size_t             generation      = 0;
    bool                    hasMeta         = false;
    bool                    hasSnapshot     = false;
    std::size_t             metadataBytes   = 0;
    std::uint64_t           timestampMs     = 0;
    std::string             isoTimestamp;
    std::filesystem::path   metaPath;
    std::filesystem::path   snapshotPath;
    std::uintmax_t          metaFileSize    = 0;
    std::uintmax_t          snapshotFileSize = 0;
    std::optional<SnapshotStats> stats;
    std::vector<std::string>     messages;
};

struct HistorySummary {
    std::filesystem::path rootPath;
    std::filesystem::path entriesPath;
    bool                  hasStateMeta     = false;
    UndoMetadata::StateMetadata stateMeta{};
    bool                  manualGcEnabled = false;
    std::size_t           ramCacheEntries  = 0;
    std::vector<EntryInfo> entries;
    std::vector<std::string> warnings;
    std::uintmax_t        totalMetaFileBytes     = 0;
    std::uintmax_t        totalSnapshotFileBytes = 0;
    std::uintmax_t        totalRecordedPayload   = 0;
};

void print_usage() {
    std::cout << "Usage: pathspace_history_inspect <history_root> [options]\n"
                 "\n"
                 "Options:\n"
                 "  --json                  Emit summary as JSON\n"
                 "  --dump <generation>     Dump snapshot contents for the given generation\n"
                 "  --preview-bytes <n>     Limit payload hex preview to N bytes when dumping (default 16)\n"
                 "  --no-analyze            Skip loading snapshot files (metadata-only summary)\n"
                 "  --help                  Show this message\n";
}

auto parse_arguments(int argc, char** argv) -> std::optional<CommandLineOptions> {
    CommandLineOptions options;
    bool               rootSeen = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(EXIT_SUCCESS);
        } else if (arg == "--json") {
            options.jsonOutput = true;
        } else if (arg == "--no-analyze") {
            options.analyzeSnapshots = false;
        } else if (arg == "--dump") {
            if (i + 1 >= argc) {
                std::cerr << "--dump requires a generation value\n";
                return std::nullopt;
            }
            ++i;
            try {
                options.dumpGeneration = static_cast<std::size_t>(std::stoull(argv[i], nullptr, 10));
            } catch (...) {
                std::cerr << "Invalid generation for --dump: " << argv[i] << "\n";
                return std::nullopt;
            }
        } else if (arg == "--preview-bytes") {
            if (i + 1 >= argc) {
                std::cerr << "--preview-bytes requires a numeric value\n";
                return std::nullopt;
            }
            ++i;
            try {
                auto value = static_cast<std::size_t>(std::stoull(argv[i], nullptr, 10));
                options.previewBytes = value;
            } catch (...) {
                std::cerr << "Invalid value for --preview-bytes: " << argv[i] << "\n";
                return std::nullopt;
            }
        } else if (arg == "--decode") {
            options.decodeSnapshot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                ++i;
                try {
                    options.decodeGeneration =
                        static_cast<std::size_t>(std::stoull(argv[i], nullptr, 10));
                } catch (...) {
                    std::cerr << "Invalid generation for --decode: " << argv[i] << "\n";
                    return std::nullopt;
                }
            }
        } else if (arg == "--diff") {
            if (i + 1 >= argc) {
                std::cerr << "--diff requires a <a>:<b> generation pair\n";
                return std::nullopt;
            }
            ++i;
            std::string_view spec{argv[i]};
            auto             colon = spec.find(':');
            if (colon == std::string_view::npos || colon == 0 || colon == spec.size() - 1) {
                std::cerr << "Invalid diff specification: " << spec << "\n";
                return std::nullopt;
            }
            std::size_t before = 0;
            std::size_t after  = 0;
            auto beforeView    = spec.substr(0, colon);
            auto afterView     = spec.substr(colon + 1);
            try {
                before = static_cast<std::size_t>(
                    std::stoull(std::string(beforeView), nullptr, 10));
                after = static_cast<std::size_t>(
                    std::stoull(std::string(afterView), nullptr, 10));
            } catch (...) {
                std::cerr << "Invalid diff specification: " << spec << "\n";
                return std::nullopt;
            }
            options.diffRequested    = true;
            options.diffGenerations  = std::make_pair(before, after);
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return std::nullopt;
        } else {
            if (rootSeen) {
                std::cerr << "Multiple history roots provided; only one is allowed\n";
                return std::nullopt;
            }
            options.rootPath = std::filesystem::path(arg);
            rootSeen         = true;
        }
    }

    if (!rootSeen) {
        std::cerr << "History root path is required\n";
        return std::nullopt;
    }

    return options;
}

std::string_view kind_to_label(EntryKind kind) {
    switch (kind) {
    case EntryKind::Live:
        return "live";
    case EntryKind::Undo:
        return "undo";
    case EntryKind::Redo:
        return "redo";
    case EntryKind::Unknown:
    default:
        return "unknown";
    }
}

std::string error_to_string(Error const& error) {
    auto code_to_string = [](Error::Code code) -> std::string_view {
        switch (code) {
        case Error::Code::InvalidError: return "InvalidError";
        case Error::Code::UnknownError: return "UnknownError";
        case Error::Code::NoSuchPath: return "NoSuchPath";
        case Error::Code::InvalidPath: return "InvalidPath";
        case Error::Code::InvalidPathSubcomponent: return "InvalidPathSubcomponent";
        case Error::Code::InvalidType: return "InvalidType";
        case Error::Code::Timeout: return "Timeout";
        case Error::Code::MalformedInput: return "MalformedInput";
        case Error::Code::InvalidPermissions: return "InvalidPermissions";
        case Error::Code::SerializationFunctionMissing: return "SerializationFunctionMissing";
        case Error::Code::UnserializableType: return "UnserializableType";
        case Error::Code::NoObjectFound: return "NoObjectFound";
        case Error::Code::TypeMismatch: return "TypeMismatch";
        case Error::Code::NotFound: return "NotFound";
        }
        return "Unknown";
    };

    std::ostringstream oss;
    oss << code_to_string(error.code);
    if (error.message && !error.message->empty()) {
        oss << ": " << *error.message;
    }
    return oss.str();
}

std::string format_bytes(std::uintmax_t bytes) {
    std::ostringstream oss;
    oss << bytes << " B";
    if (bytes == 0)
        return oss.str();

    static constexpr std::string_view units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < std::size(units)) {
        value /= 1024.0;
        ++unitIndex;
    }
    if (unitIndex > 0) {
        oss << " (" << std::fixed;
        if (value >= 100.0) {
            oss << std::setprecision(0);
        } else if (value >= 10.0) {
            oss << std::setprecision(1);
        } else {
            oss << std::setprecision(2);
        }
        oss << value << ' ' << units[unitIndex] << ')';
    }
    return oss.str();
}

std::string format_timestamp(std::uint64_t millis) {
    using namespace std::chrono;
    if (millis == 0)
        return {};

    auto timePoint = system_clock::time_point(milliseconds(millis));
    auto secPart   = floor<seconds>(timePoint);
    auto msPart    = duration_cast<milliseconds>(timePoint - secPart).count();

    std::time_t timeT = system_clock::to_time_t(secPart);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &timeT);
#else
    gmtime_r(&timeT, &tm);
#endif

    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm) == 0) {
        return std::to_string(millis);
    }
    std::ostringstream oss;
    oss << buffer << '.' << std::setw(3) << std::setfill('0') << msPart << 'Z';
    return oss.str();
}

std::string escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
        case '\"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                out.append(buffer);
            } else {
                out.push_back(ch);
            }
        }
    }
    return out;
}

std::string relative_to(std::filesystem::path const& base, std::filesystem::path const& target) {
    if (target.empty())
        return {};
    std::error_code ec;
    auto relative = std::filesystem::relative(target, base, ec);
    if (!ec)
        return relative.generic_string();
    return target.generic_string();
}

auto compute_snapshot_stats(std::filesystem::path const& snapshotPath) -> Expected<SnapshotStats> {
    CowSubtreePrototype prototype;
    auto snapshotExpected = UndoSnapshotCodec::loadSnapshotFromFile(prototype, snapshotPath);
    if (!snapshotExpected) {
        return std::unexpected(snapshotExpected.error());
    }
    auto memory = prototype.analyze(snapshotExpected.value());
    SnapshotStats stats;
    stats.uniqueNodes  = memory.uniqueNodes;
    stats.payloadBytes = memory.payloadBytes;
    return stats;
}

auto gather_summary(CommandLineOptions const& options)
    -> std::expected<HistorySummary, std::string> {
    HistorySummary summary;
    summary.rootPath    = std::filesystem::absolute(options.rootPath);
    summary.entriesPath = summary.rootPath / "entries";

    std::error_code ec;
    if (!std::filesystem::exists(summary.rootPath, ec) || ec) {
        std::ostringstream oss;
        oss << "History root does not exist: " << options.rootPath.generic_string();
        if (ec) {
            oss << " (" << ec.message() << ")";
        }
        return std::unexpected(oss.str());
    }

    auto statePath = summary.rootPath / "state.meta";
    auto stateBytesExpected = UndoUtils::readBinaryFile(statePath);
    if (stateBytesExpected) {
        auto stateMetaExpected = UndoMetadata::parseStateMeta(
            std::span<const std::byte>(stateBytesExpected->data(), stateBytesExpected->size()));
        if (stateMetaExpected) {
            summary.stateMeta       = *stateMetaExpected;
            summary.hasStateMeta    = true;
            summary.manualGcEnabled = stateMetaExpected->manualGc;
            summary.ramCacheEntries = stateMetaExpected->ramCacheEntries;
        } else {
            summary.warnings.push_back(
                "Failed to parse state.meta: " + error_to_string(stateMetaExpected.error()));
        }
    } else {
        auto message = error_to_string(stateBytesExpected.error());
        summary.warnings.push_back("Unable to read state.meta: " + message);
    }

    std::map<std::size_t, EntryInfo> entryMap;
    auto ensure_entry = [&](std::size_t generation, EntryKind kind) -> EntryInfo& {
        auto [it, inserted] = entryMap.try_emplace(generation);
        auto& info = it->second;
        if (inserted) {
            info.generation = generation;
            info.kind       = kind;
        } else if (kind != EntryKind::Unknown) {
            if (info.kind == EntryKind::Unknown) {
                info.kind = kind;
            } else if (info.kind != kind) {
                info.messages.push_back("Conflicting classification: " +
                                        std::string(kind_to_label(info.kind)) + " vs. " +
                                        std::string(kind_to_label(kind)));
            }
        }
        return info;
    };

    if (summary.hasStateMeta) {
        ensure_entry(summary.stateMeta.liveGeneration, EntryKind::Live);
        for (auto generation : summary.stateMeta.undoGenerations) {
            ensure_entry(generation, EntryKind::Undo);
        }
        for (auto generation : summary.stateMeta.redoGenerations) {
            ensure_entry(generation, EntryKind::Redo);
        }
    }

    if (std::filesystem::exists(summary.entriesPath, ec) && !ec) {
        for (auto const& dirEntry : std::filesystem::directory_iterator(summary.entriesPath)) {
            if (!dirEntry.is_regular_file(ec) || ec)
                continue;

            auto path = dirEntry.path();
            auto ext  = path.extension().generic_string();
            if (ext != ".meta" && ext != ".snapshot")
                continue;

            auto stem = path.stem().generic_string();
            std::size_t generation = 0;
            try {
                generation = static_cast<std::size_t>(std::stoull(stem, nullptr, 10));
            } catch (...) {
                summary.warnings.push_back("Skipping entry with non-numeric generation: " +
                                           path.filename().generic_string());
                continue;
            }

            auto& info = ensure_entry(generation, EntryKind::Unknown);

            if (ext == ".meta") {
                info.metaPath   = path;
                info.hasMeta    = true;
                info.metaFileSize = dirEntry.file_size(ec);
                if (ec) {
                    info.messages.push_back("Unable to read meta file size: " + ec.message());
                    ec.clear();
                } else {
                    summary.totalMetaFileBytes += info.metaFileSize;
                }

                auto metaBytesExpected = UndoUtils::readBinaryFile(path);
                if (metaBytesExpected) {
                    auto metaExpected = UndoMetadata::parseEntryMeta(
                        std::span<const std::byte>(metaBytesExpected->data(),
                                                   metaBytesExpected->size()));
                    if (metaExpected) {
                        info.metadataBytes = metaExpected->bytes;
                        info.timestampMs   = metaExpected->timestampMs;
                        info.isoTimestamp  = format_timestamp(metaExpected->timestampMs);
                        summary.totalRecordedPayload += metaExpected->bytes;
                    } else {
                        info.messages.push_back(
                            "Failed to parse entry meta: " +
                            error_to_string(metaExpected.error()));
                    }
                } else {
                    info.messages.push_back("Unable to read entry meta: " +
                                            error_to_string(metaBytesExpected.error()));
                }
            } else {
                info.snapshotPath   = path;
                info.hasSnapshot    = true;
                info.snapshotFileSize = dirEntry.file_size(ec);
                if (ec) {
                    info.messages.push_back("Unable to read snapshot size: " + ec.message());
                    ec.clear();
                } else {
                    summary.totalSnapshotFileBytes += info.snapshotFileSize;
                }
            }
        }
    } else {
        std::string reason = ec ? ec.message() : "directory missing";
        summary.warnings.push_back("Entries directory unavailable: " +
                                   summary.entriesPath.generic_string() + " (" + reason + ")");
        ec.clear();
    }

    if (summary.hasStateMeta) {
        auto note_missing_files = [&](std::size_t generation, std::string_view label) {
            auto& info = ensure_entry(generation, EntryKind::Unknown);
            if (!info.hasMeta) {
                info.messages.push_back("state.meta references " + std::string(label) +
                                        " generation with missing .meta file");
            }
            if (!info.hasSnapshot) {
                info.messages.push_back("state.meta references " + std::string(label) +
                                        " generation with missing .snapshot file");
            }
        };

        note_missing_files(summary.stateMeta.liveGeneration, "live");
        for (auto generation : summary.stateMeta.undoGenerations) {
            note_missing_files(generation, "undo");
        }
        for (auto generation : summary.stateMeta.redoGenerations) {
            note_missing_files(generation, "redo");
        }
    }

    summary.entries.reserve(entryMap.size());
    for (auto& [generation, info] : entryMap) {
        summary.entries.push_back(std::move(info));
    }

    if (options.analyzeSnapshots) {
        for (auto& entry : summary.entries) {
            if (!entry.hasSnapshot)
                continue;

            auto statsExpected = compute_snapshot_stats(entry.snapshotPath);
            if (statsExpected) {
                entry.stats = *statsExpected;
                if (entry.metadataBytes != 0 &&
                    entry.stats->payloadBytes != entry.metadataBytes) {
                    entry.messages.push_back("Metadata bytes (" +
                                             std::to_string(entry.metadataBytes) +
                                             ") differ from snapshot payload bytes (" +
                                             std::to_string(entry.stats->payloadBytes) + ")");
                }
            } else {
                entry.messages.push_back("Failed to load snapshot: " +
                                         error_to_string(statsExpected.error()));
            }
        }
    }

    return summary;
}

void print_summary_text(HistorySummary const& summary) {
    std::cout << "History root:      " << summary.rootPath.generic_string() << "\n";
    std::cout << "Entries directory: " << summary.entriesPath.generic_string() << "\n";
    if (summary.hasStateMeta) {
        std::cout << "Live generation:   " << summary.stateMeta.liveGeneration << "\n";
        std::cout << "Undo generations:  ";
        if (summary.stateMeta.undoGenerations.empty()) {
            std::cout << "(none)\n";
        } else {
            for (std::size_t i = 0; i < summary.stateMeta.undoGenerations.size(); ++i) {
                if (i > 0)
                    std::cout << ", ";
                std::cout << summary.stateMeta.undoGenerations[i];
            }
            std::cout << "\n";
        }
        std::cout << "Redo generations:  ";
        if (summary.stateMeta.redoGenerations.empty()) {
            std::cout << "(none)\n";
        } else {
            for (std::size_t i = 0; i < summary.stateMeta.redoGenerations.size(); ++i) {
                if (i > 0)
                    std::cout << ", ";
                std::cout << summary.stateMeta.redoGenerations[i];
            }
            std::cout << "\n";
        }
        std::cout << "Manual GC:         " << (summary.manualGcEnabled ? "enabled" : "disabled")
                  << "\n";
        std::cout << "RAM cache entries: " << summary.ramCacheEntries << "\n";
    } else {
        std::cout << "State metadata:    unavailable\n";
    }

    std::map<EntryKind, std::size_t> counts;
    for (auto const& entry : summary.entries) {
        counts[entry.kind] += 1;
    }

    std::cout << "\nEntries found:     " << summary.entries.size() << " total";
    if (!counts.empty()) {
        std::cout << " (";
        bool first = true;
        for (auto const& [kind, count] : counts) {
            if (!first)
                std::cout << ", ";
            std::cout << kind_to_label(kind) << '=' << count;
            first = false;
        }
        std::cout << ")\n";
    } else {
        std::cout << "\n";
    }

    std::cout << "Disk usage:        meta files "
              << format_bytes(summary.totalMetaFileBytes) << ", snapshots "
              << format_bytes(summary.totalSnapshotFileBytes) << "\n";
    if (summary.totalRecordedPayload > 0) {
        std::cout << "Recorded payload:  " << format_bytes(summary.totalRecordedPayload) << "\n";
    }

    if (!summary.warnings.empty()) {
        std::cout << "\nWarnings:\n";
        for (auto const& warning : summary.warnings) {
            std::cout << "  - " << warning << "\n";
        }
    }

    if (!summary.entries.empty()) {
        std::cout << "\nEntries:\n";
    }
    for (auto const& entry : summary.entries) {
        std::cout << "- [" << kind_to_label(entry.kind) << "] generation " << entry.generation;
        if (!entry.isoTimestamp.empty()) {
            std::cout << " (" << entry.isoTimestamp << ")";
        }
        std::cout << "\n";

        if (entry.hasMeta) {
            std::cout << "    meta file:     "
                      << relative_to(summary.rootPath, entry.metaPath) << " ("
                      << format_bytes(entry.metaFileSize) << ")\n";
            std::cout << "    payload bytes: " << format_bytes(entry.metadataBytes) << "\n";
        } else {
            std::cout << "    meta file:     missing\n";
        }

        if (entry.hasSnapshot) {
            std::cout << "    snapshot file: "
                      << relative_to(summary.rootPath, entry.snapshotPath) << " ("
                      << format_bytes(entry.snapshotFileSize) << ")\n";
        } else {
            std::cout << "    snapshot file: missing\n";
        }

        if (entry.stats) {
            std::cout << "    stats:         uniqueNodes=" << entry.stats->uniqueNodes
                      << ", payloadBytes=" << format_bytes(entry.stats->payloadBytes) << "\n";
        }

        if (!entry.messages.empty()) {
            std::cout << "    notes:\n";
            for (auto const& note : entry.messages) {
                std::cout << "      • " << note << "\n";
            }
        }
    }
}

void print_summary_json(HistorySummary const& summary) {
    std::cout << "{\n";
    std::cout << "  \"root\": \"" << escape_json(summary.rootPath.generic_string()) << "\",\n";
    std::cout << "  \"entriesDirectory\": \""
              << escape_json(summary.entriesPath.generic_string()) << "\",\n";
    std::cout << "  \"stateMeta\": {\n";
    std::cout << "    \"present\": " << (summary.hasStateMeta ? "true" : "false");
    if (summary.hasStateMeta) {
        std::cout << ",\n";
        std::cout << "    \"liveGeneration\": " << summary.stateMeta.liveGeneration << ",\n";
        std::cout << "    \"undoGenerations\": [";
        for (std::size_t i = 0; i < summary.stateMeta.undoGenerations.size(); ++i) {
            if (i > 0)
                std::cout << ", ";
            std::cout << summary.stateMeta.undoGenerations[i];
        }
        std::cout << "],\n";
        std::cout << "    \"redoGenerations\": [";
        for (std::size_t i = 0; i < summary.stateMeta.redoGenerations.size(); ++i) {
            if (i > 0)
                std::cout << ", ";
            std::cout << summary.stateMeta.redoGenerations[i];
        }
        std::cout << "],\n";
        std::cout << "    \"manualGarbageCollect\": "
                  << (summary.manualGcEnabled ? "true" : "false") << ",\n";
        std::cout << "    \"ramCacheEntries\": " << summary.ramCacheEntries << "\n";
    } else {
        std::cout << "\n";
    }
    std::cout << "  },\n";

    std::cout << "  \"totals\": {\n";
    std::cout << "    \"entryCount\": " << summary.entries.size() << ",\n";
    std::cout << "    \"metaFileBytes\": " << summary.totalMetaFileBytes << ",\n";
    std::cout << "    \"snapshotFileBytes\": " << summary.totalSnapshotFileBytes << ",\n";
    std::cout << "    \"recordedPayloadBytes\": " << summary.totalRecordedPayload << "\n";
    std::cout << "  },\n";

    std::cout << "  \"entries\": [\n";
    for (std::size_t index = 0; index < summary.entries.size(); ++index) {
        auto const& entry = summary.entries[index];
        std::cout << "    {\n";
        std::cout << "      \"kind\": \"" << kind_to_label(entry.kind) << "\",\n";
        std::cout << "      \"generation\": " << entry.generation << ",\n";
        std::cout << "      \"hasMeta\": " << (entry.hasMeta ? "true" : "false") << ",\n";
        std::cout << "      \"hasSnapshot\": " << (entry.hasSnapshot ? "true" : "false") << ",\n";
        std::cout << "      \"metadataBytes\": " << entry.metadataBytes << ",\n";
        std::cout << "      \"metaFileBytes\": " << entry.metaFileSize << ",\n";
        std::cout << "      \"snapshotFileBytes\": " << entry.snapshotFileSize << ",\n";
        std::cout << "      \"timestampMs\": " << entry.timestampMs << ",\n";
        std::cout << "      \"timestampIso\": \""
                  << escape_json(entry.isoTimestamp) << "\",\n";
        std::cout << "      \"metaPath\": \""
                  << escape_json(relative_to(summary.rootPath, entry.metaPath)) << "\",\n";
        std::cout << "      \"snapshotPath\": \""
                  << escape_json(relative_to(summary.rootPath, entry.snapshotPath)) << "\",\n";
        if (entry.stats) {
            std::cout << "      \"stats\": {\n";
            std::cout << "        \"uniqueNodes\": " << entry.stats->uniqueNodes << ",\n";
            std::cout << "        \"payloadBytes\": " << entry.stats->payloadBytes << "\n";
            std::cout << "      },\n";
        } else {
            std::cout << "      \"stats\": null,\n";
        }
        std::cout << "      \"messages\": [";
        for (std::size_t mi = 0; mi < entry.messages.size(); ++mi) {
            if (mi > 0)
                std::cout << ", ";
            std::cout << "\"" << escape_json(entry.messages[mi]) << "\"";
        }
        std::cout << "]\n";
        std::cout << "    }";
        if (index + 1 < summary.entries.size())
            std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "  ],\n";

    std::cout << "  \"warnings\": [";
    for (std::size_t wi = 0; wi < summary.warnings.size(); ++wi) {
        if (wi > 0)
            std::cout << ", ";
        std::cout << "\"" << escape_json(summary.warnings[wi]) << "\"";
    }
    std::cout << "]\n";
    std::cout << "}\n";
}

std::string hex_preview(std::vector<std::byte> const& bytes, std::size_t limit) {
    if (limit == 0 || bytes.empty())
        return {};
    std::ostringstream oss;
    auto count = std::min<std::size_t>(bytes.size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0)
            oss << ' ';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[i]));
    }
    if (bytes.size() > count) {
        oss << " …";
    }
    return oss.str();
}

std::optional<std::string> decode_root_path(HistorySummary const& summary) {
    auto encoded = summary.rootPath.filename().generic_string();
    if (encoded.empty())
        return std::nullopt;
    if (encoded.size() % 2 != 0)
        return std::nullopt;
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        auto const* begin = encoded.data() + static_cast<std::ptrdiff_t>(i);
        auto const* end   = begin + 2;
        unsigned int value = 0;
        auto result = std::from_chars(begin, end, value, 16);
        if (result.ec != std::errc{} || result.ptr != end) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>(value));
    }
    return decoded;
}

EntryInfo const* find_entry(HistorySummary const& summary, std::size_t generation) {
    auto it = std::find_if(summary.entries.begin(),
                           summary.entries.end(),
                           [&](EntryInfo const& info) { return info.generation == generation; });
    if (it == summary.entries.end())
        return nullptr;
    return &*it;
}

bool decode_snapshot_payloads(CommandLineOptions const& options,
                              HistorySummary const& summary) {
    if (!options.decodeSnapshot)
        return true;

    std::size_t generation = 0;
    if (options.decodeGeneration) {
        generation = *options.decodeGeneration;
    } else if (summary.hasStateMeta) {
        generation = summary.stateMeta.liveGeneration;
    } else {
        std::cerr << "Unable to determine generation for --decode; state metadata missing\n";
        return false;
    }

    auto* entry = find_entry(summary, generation);
    if (!entry) {
        std::cerr << "No snapshot found for generation " << generation << "\n";
        return false;
    }
    if (!entry->hasSnapshot) {
        std::cerr << "Generation " << generation << " has no snapshot file\n";
        return false;
    }

    CowSubtreePrototype prototype;
    auto snapshotExpected = UndoSnapshotCodec::loadSnapshotFromFile(prototype, entry->snapshotPath);
    if (!snapshotExpected) {
        std::cerr << "Failed to load snapshot: "
                  << error_to_string(snapshotExpected.error()) << "\n";
        return false;
    }

    auto rootPath = decode_root_path(summary).value_or("/");
    auto decoded  = Inspection::decodeSnapshot(*snapshotExpected, rootPath);

    std::cout << "\nDecoded payloads for generation " << generation << " ("
              << kind_to_label(entry->kind) << ")\n";
    if (decoded.values.empty()) {
        std::cout << "  (no payloads)\n";
        return true;
    }

    for (auto const& value : decoded.values) {
        std::cout << "- " << value.path << "\n";
        std::cout << "    type:   " << value.typeName << " [" << value.category << "]\n";
        std::cout << "    value:  " << value.summary << "\n";
        std::cout << "    size:   " << format_bytes(value.bytes) << "\n";
        std::cout << "    digest: 0x" << std::hex << std::setw(16) << std::setfill('0')
                  << value.digest << std::dec << "\n";
    }
    return true;
}

bool diff_snapshots(CommandLineOptions const& options,
                    HistorySummary const& summary) {
    if (!options.diffRequested)
        return true;
    if (!options.diffGenerations) {
        std::cerr << "--diff requires a generation pair\n";
        return false;
    }
    auto [beforeGen, afterGen] = *options.diffGenerations;

    auto* beforeEntry = find_entry(summary, beforeGen);
    auto* afterEntry  = find_entry(summary, afterGen);
    if (!beforeEntry) {
        std::cerr << "No snapshot found for generation " << beforeGen << "\n";
        return false;
    }
    if (!afterEntry) {
        std::cerr << "No snapshot found for generation " << afterGen << "\n";
        return false;
    }
    if (!beforeEntry->hasSnapshot || !afterEntry->hasSnapshot) {
        std::cerr << "Both generations must have on-disk snapshots\n";
        return false;
    }

    CowSubtreePrototype prototype;
    auto beforeSnapshot = UndoSnapshotCodec::loadSnapshotFromFile(prototype, beforeEntry->snapshotPath);
    if (!beforeSnapshot) {
        std::cerr << "Failed to load baseline snapshot: "
                  << error_to_string(beforeSnapshot.error()) << "\n";
        return false;
    }
    auto afterSnapshot = UndoSnapshotCodec::loadSnapshotFromFile(prototype, afterEntry->snapshotPath);
    if (!afterSnapshot) {
        std::cerr << "Failed to load updated snapshot: "
                  << error_to_string(afterSnapshot.error()) << "\n";
        return false;
    }

    auto rootPath = decode_root_path(summary).value_or("/");
    auto diff = Inspection::diffSnapshots(*beforeSnapshot, *afterSnapshot, rootPath);

    std::cout << "\nDiff between generation " << beforeGen << " (" << kind_to_label(beforeEntry->kind)
              << ") and " << afterGen << " (" << kind_to_label(afterEntry->kind) << ")\n";

    auto printValue = [](std::string const& label,
                         Inspection::DecodedValue const& value,
                         std::string const& prefix) {
        std::cout << prefix << label << ": " << value.path << "\n";
        std::cout << prefix << "  type:   " << value.typeName << " [" << value.category << "]\n";
        std::cout << prefix << "  value:  " << value.summary << "\n";
        std::cout << prefix << "  size:   " << format_bytes(value.bytes) << "\n";
        std::cout << prefix << "  digest: 0x" << std::hex << std::setw(16) << std::setfill('0')
                  << value.digest << std::dec << "\n";
    };

    if (!diff.added.empty()) {
        std::cout << "  Added:\n";
        for (auto const& value : diff.added) {
            printValue("", value, "    ");
        }
    }
    if (!diff.removed.empty()) {
        std::cout << "  Removed:\n";
        for (auto const& value : diff.removed) {
            printValue("", value, "    ");
        }
    }
    if (!diff.modified.empty()) {
        std::cout << "  Modified:\n";
        for (auto const& change : diff.modified) {
            printValue("before", change.before, "    ");
            printValue("after", change.after, "    ");
        }
    }
    if (diff.added.empty() && diff.removed.empty() && diff.modified.empty()) {
        std::cout << "  (no differences)\n";
    }

    return true;
}

void dump_snapshot_node(std::vector<std::string>& path,
                        std::shared_ptr<const CowSubtreePrototype::Node> const& node,
                        std::size_t previewBytes,
                        std::string const& indent) {
    if (!node)
        return;

    auto currentPath = [&]() -> std::string {
        if (path.empty())
            return "/";
        std::ostringstream oss;
        for (auto const& component : path) {
            if (oss.tellp() == std::streampos{0}) {
                oss << '/';
            } else {
                oss << '/';
            }
            oss << component;
        }
        return oss.str();
    }();

    std::cout << indent << currentPath;
    if (node->payload.bytes && !node->payload.bytes->empty()) {
        auto const& payload = *node->payload.bytes;
        std::cout << "  payload=" << payload.size() << " B";
        auto preview = hex_preview(payload, previewBytes);
        if (!preview.empty()) {
            std::cout << "  hex=" << preview;
        }
    }
    if (node->children.empty()) {
        std::cout << "\n";
    } else {
        std::cout << "  (" << node->children.size() << " child"
                  << (node->children.size() == 1 ? "" : "ren") << ")\n";
    }

    auto childIndent = indent;
    childIndent.append("  ");
    for (auto const& [name, child] : node->children) {
        path.push_back(name);
        dump_snapshot_node(path, child, previewBytes, childIndent);
        path.pop_back();
    }
}

bool dump_snapshot(CommandLineOptions const& options,
                   HistorySummary const& summary) {
    if (!options.dumpGeneration)
        return true;

    auto generation = *options.dumpGeneration;
    auto it = std::find_if(summary.entries.begin(), summary.entries.end(),
                           [&](EntryInfo const& entry) {
                               return entry.generation == generation;
                           });
    if (it == summary.entries.end()) {
        std::cerr << "No entry found for generation " << generation << "\n";
        return false;
    }
    if (!it->hasSnapshot) {
        std::cerr << "Generation " << generation << " has no snapshot on disk\n";
        return false;
    }

    CowSubtreePrototype prototype;
    auto snapshotExpected = UndoSnapshotCodec::loadSnapshotFromFile(prototype, it->snapshotPath);
    if (!snapshotExpected) {
        std::cerr << "Failed to load snapshot: "
                  << error_to_string(snapshotExpected.error()) << "\n";
        return false;
    }

    std::cout << "\nSnapshot tree for generation " << generation << " ("
              << kind_to_label(it->kind) << ")\n";
    std::vector<std::string> path;
    dump_snapshot_node(path, snapshotExpected->root, options.previewBytes, "");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    auto optionsOpt = parse_arguments(argc, argv);
    if (!optionsOpt)
        return EXIT_FAILURE;
    auto options = *optionsOpt;

    auto summaryExpected = gather_summary(options);
    if (!summaryExpected) {
        std::cerr << summaryExpected.error() << "\n";
        return EXIT_FAILURE;
    }

    auto const& summary = *summaryExpected;

    if (options.jsonOutput) {
        print_summary_json(summary);
    } else {
        print_summary_text(summary);
    }

    if (!decode_snapshot_payloads(options, summary))
        return EXIT_FAILURE;

    if (!diff_snapshots(options, summary))
        return EXIT_FAILURE;

    if (!dump_snapshot(options, summary))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
