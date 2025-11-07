#include <pathspace/PathSpace.hpp>
#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

using namespace SP;
using namespace SP::History;
namespace fs = std::filesystem;

namespace {

struct ScopedDirectory {
    fs::path path;
    bool     keep = false;
    ~ScopedDirectory() {
        if (keep || path.empty())
            return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void dismiss() {
        keep = true;
    }
};

struct ArtifactArchiver {
    explicit ArtifactArchiver(std::optional<fs::path> destination)
        : destination_(std::move(destination)) {}

    void addFile(fs::path source, std::string name) {
        if (!destination_)
            return;
        files_.emplace_back(std::move(source), std::move(name));
    }

    void addText(std::string name, std::string content) {
        if (!destination_)
            return;
        texts_.emplace_back(std::move(name), std::move(content));
    }

    ~ArtifactArchiver() {
        if (!destination_)
            return;

        std::error_code ec;
        fs::create_directories(*destination_, ec);
        if (ec) {
            std::cerr << "Failed to create archive directory " << destination_->string()
                      << ": " << ec.message() << "\n";
            return;
        }

        for (auto& file : files_) {
            auto const& source = file.first;
            auto const& name   = file.second;
            if (!fs::exists(source))
                continue;
            auto target = *destination_ / name;
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                std::cerr << "Failed to create directory for " << target
                          << ": " << ec.message() << "\n";
                ec.clear();
            }
            fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "Failed to archive " << source << " to " << target
                          << ": " << ec.message() << "\n";
                ec.clear();
            }
        }

        for (auto& text : texts_) {
            auto const& name    = text.first;
            auto const& content = text.second;
            auto        target  = *destination_ / name;
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "Failed to write telemetry file " << target << "\n";
                continue;
            }
            out << content;
        }

        std::cerr << "Archived PathSpace CLI artifacts under " << destination_->string()
                  << "\n";
    }

private:
    std::optional<fs::path> destination_;
    std::vector<std::pair<fs::path, std::string>> files_;
    std::vector<std::pair<std::string, std::string>> texts_;
};

auto encodeRoot(std::string_view root) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0');
    for (unsigned char c : root) {
        oss << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();
}

auto makeScratchDirectory() -> fs::path {
    auto base = fs::temp_directory_path() / "pathspace_cli_roundtrip";
    std::error_code ec;
    fs::create_directories(base, ec);

    std::random_device rd;
    std::mt19937       gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);

    for (int attempt = 0; attempt < 32; ++attempt) {
        std::ostringstream oss;
        oss << "run-";
        for (int i = 0; i < 8; ++i)
            oss << std::hex << dist(gen);

        auto candidate = base / oss.str();
        if (fs::create_directories(candidate, ec))
            return candidate;

        if (ec && ec != std::errc::file_exists)
            break;
    }

    throw std::runtime_error("Failed to allocate scratch directory for CLI roundtrip harness");
}

auto runCommand(fs::path const& exe, std::vector<std::string> const& args) -> int {
    std::ostringstream cmd;
    cmd << "\"" << exe.string() << "\"";
    for (auto const& arg : args) {
        cmd << " \"" << arg << "\"";
    }
    auto command = cmd.str();
    int rc       = std::system(command.c_str());
    if (rc != 0) {
        std::cerr << "Command failed (" << rc << "): " << command << "\n";
    }
    return rc;
}

auto formatError(Error const& error) -> std::string {
    std::ostringstream oss;
    oss << static_cast<int>(error.code);
    if (error.message && !error.message->empty())
        oss << ": " << *error.message;
    return oss.str();
}

auto makeUndoableSpace(HistoryOptions defaults) -> std::unique_ptr<UndoableSpace> {
    auto inner = std::make_unique<PathSpace>();
    return std::make_unique<UndoableSpace>(std::move(inner), std::move(defaults));
}

struct HistorySummary {
    std::vector<std::string> values;
    std::size_t              undoCount            = 0;
    std::size_t              redoCount            = 0;
    std::size_t              diskEntries          = 0;
    std::size_t              undoBytes            = 0;
    std::size_t              redoBytes            = 0;
    std::size_t              liveBytes            = 0;
    bool                     manualGarbageCollect = false;
};

auto collectHistorySummary(std::filesystem::path const& savefile,
                           ConcretePathStringView root,
                           bool debugLogging) -> std::optional<HistorySummary> {
    HistoryOptions options;
    auto           space = makeUndoableSpace(options);

    auto enable = space->enableHistory(root);
    if (!enable) {
        std::cerr << "Failed to enable history for summary: " << formatError(enable.error())
                  << "\n";
        return std::nullopt;
    }

    auto import = space->importHistorySavefile(root, savefile, true);
    if (!import) {
        std::cerr << "Failed to import " << savefile << " for summary: "
                  << formatError(import.error()) << "\n";
        return std::nullopt;
    }

    auto stats = space->getHistoryStats(root);
    if (!stats) {
        std::cerr << "Failed to query stats for " << savefile << ": "
                  << formatError(stats.error()) << "\n";
        return std::nullopt;
    }

    HistorySummary summary;
    summary.undoCount            = stats->counts.undo;
    summary.redoCount            = stats->counts.redo;
    summary.diskEntries          = stats->counts.diskEntries;
    summary.undoBytes            = stats->bytes.undo;
    summary.redoBytes            = stats->bytes.redo;
    summary.liveBytes            = stats->bytes.live;
    summary.manualGarbageCollect = stats->counts.manualGarbageCollect;

    for (int i = 0; i < 16; ++i) {
        auto value = space->take<std::string>("/doc/title");
        if (!value)
            break;
        summary.values.push_back(std::move(*value));
    }

    if (debugLogging) {
        std::cerr << "[debug] Summary for " << savefile << ": undo=" << summary.undoCount
                  << " redo=" << summary.redoCount << " values=";
        for (auto const& v : summary.values)
            std::cerr << v << " ";
        std::cerr << "\n";
    }

    return summary;
}

auto formatTimestampIso() -> std::string {
    auto now      = std::chrono::system_clock::now();
    auto millis   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    auto seconds  = std::chrono::duration_cast<std::chrono::seconds>(millis);
    auto fraction = millis - seconds;

    std::time_t t = seconds.count();
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%FT%T")
        << '.' << std::setw(3) << std::setfill('0') << fraction.count() << 'Z';
    return oss.str();
}

auto computeFileHash(fs::path const& file) -> std::optional<std::string> {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return std::nullopt;

    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime  = 1099511628211ull;

    std::uint64_t hash = kOffset;
    std::array<char, 4096> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        auto read = stream.gcount();
        if (read <= 0)
            break;
        for (std::streamsize i = 0; i < read; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= kPrime;
        }
    }

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

auto makeTelemetryJson(std::string const& timestamp,
                       fs::path const&      originalSavefile,
                       HistorySummary const& original,
                       fs::path const&      roundtripSavefile,
                       HistorySummary const& roundtrip,
                       HistoryStats const&   importStats) -> std::string {
    auto originalSize  = fs::exists(originalSavefile) ? fs::file_size(originalSavefile) : 0u;
    auto roundtripSize = fs::exists(roundtripSavefile) ? fs::file_size(roundtripSavefile) : 0u;

    auto originalHash  = computeFileHash(originalSavefile).value_or("");
    auto roundtripHash = computeFileHash(roundtripSavefile).value_or("");

    std::ostringstream json;
    json << std::boolalpha;
    json << "{\n";
    json << "  \"timestampIso\": \"" << timestamp << "\",\n";
    json << "  \"original\": {\n";
    json << "    \"hashFnv1a64\": \"" << originalHash << "\",\n";
    json << "    \"sizeBytes\": " << originalSize << ",\n";
    json << "    \"undoCount\": " << original.undoCount << ",\n";
    json << "    \"redoCount\": " << original.redoCount << ",\n";
    json << "    \"diskEntries\": " << original.diskEntries << ",\n";
    json << "    \"undoBytes\": " << original.undoBytes << ",\n";
    json << "    \"redoBytes\": " << original.redoBytes << ",\n";
    json << "    \"liveBytes\": " << original.liveBytes << ",\n";
    json << "    \"manualGarbageCollect\": " << original.manualGarbageCollect << "\n";
    json << "  },\n";
    json << "  \"roundtrip\": {\n";
    json << "    \"hashFnv1a64\": \"" << roundtripHash << "\",\n";
    json << "    \"sizeBytes\": " << roundtripSize << ",\n";
    json << "    \"undoCount\": " << roundtrip.undoCount << ",\n";
    json << "    \"redoCount\": " << roundtrip.redoCount << ",\n";
    json << "    \"diskEntries\": " << roundtrip.diskEntries << ",\n";
    json << "    \"undoBytes\": " << roundtrip.undoBytes << ",\n";
    json << "    \"redoBytes\": " << roundtrip.redoBytes << ",\n";
    json << "    \"liveBytes\": " << roundtrip.liveBytes << ",\n";
    json << "    \"manualGarbageCollect\": " << roundtrip.manualGarbageCollect << "\n";
    json << "  },\n";
    json << "  \"import\": {\n";
    json << "    \"undoCount\": " << importStats.counts.undo << ",\n";
    json << "    \"redoCount\": " << importStats.counts.redo << ",\n";
    json << "    \"diskEntries\": " << importStats.counts.diskEntries << ",\n";
    json << "    \"cachedUndo\": " << importStats.counts.cachedUndo << ",\n";
    json << "    \"cachedRedo\": " << importStats.counts.cachedRedo << ",\n";
    json << "    \"manualGarbageCollect\": " << importStats.counts.manualGarbageCollect << ",\n";
    json << "    \"undoBytes\": " << importStats.bytes.undo << ",\n";
    json << "    \"redoBytes\": " << importStats.bytes.redo << ",\n";
    json << "    \"liveBytes\": " << importStats.bytes.live << ",\n";
    json << "    \"diskBytes\": " << importStats.bytes.disk << ",\n";
    json << "    \"totalBytes\": " << importStats.bytes.total << "\n";
    json << "  }\n";
    json << "}\n";
    return json.str();
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    try {
        auto selfPath = fs::weakly_canonical(fs::path{argv[0]});
        auto buildDir = selfPath.parent_path();
        auto cliPath  = buildDir / "pathspace_history_savefile";
        if (!fs::exists(cliPath)) {
            std::cerr << "Unable to locate pathspace_history_savefile next to "
                      << selfPath << "\n";
            return EXIT_FAILURE;
        }

        auto scratchRoot = makeScratchDirectory();
        ScopedDirectory cleanup{scratchRoot};
        if (std::getenv("PATHSPACE_CLI_ROUNDTRIP_KEEP"))
            cleanup.dismiss();

        std::optional<fs::path> archiveDir;
        if (auto* env = std::getenv("PATHSPACE_CLI_ROUNDTRIP_ARCHIVE_DIR")) {
            if (*env != '\0')
                archiveDir = fs::path{env};
        }
        if (!archiveDir) {
            if (auto* env = std::getenv("PATHSPACE_TEST_ARTIFACT_DIR")) {
                if (*env != '\0')
                    archiveDir = fs::path{env} / "history_cli_roundtrip";
            }
        }
        ArtifactArchiver archiver{archiveDir};

        bool debugLogging = std::getenv("PATHSPACE_CLI_ROUNDTRIP_DEBUG") != nullptr;

        auto exportBase = scratchRoot / "export_root";
        auto importBase = scratchRoot / "import_root";
        fs::create_directories(exportBase);
        fs::create_directories(importBase);

        HistoryOptions exportDefaults;
        exportDefaults.persistHistory       = true;
        exportDefaults.persistenceRoot      = exportBase.string();
        exportDefaults.persistenceNamespace = "cli_roundtrip_export";
        exportDefaults.ramCacheEntries      = 4;
        exportDefaults.allowNestedUndo      = true;

        auto rootPath = std::string{"/doc"};
        auto rootView = ConcretePathStringView{rootPath};

        {
            auto exporter = makeUndoableSpace(exportDefaults);
            auto enable   = exporter->enableHistory(rootView);
            if (!enable) {
                std::cerr << "Failed to enable history for export: " << formatError(enable.error())
                          << "\n";
                return EXIT_FAILURE;
            }

            auto ensureInsert = [&](std::string const& path, std::string value) -> bool {
                auto inserted = exporter->insert(path, std::move(value));
                if (!inserted.errors.empty()) {
                    std::cerr << "Insert error on " << path
                              << ": " << formatError(inserted.errors.front()) << "\n";
                    return false;
                }
                return true;
            };

            if (!ensureInsert("/doc/title", "alpha"))
                return EXIT_FAILURE;
            if (!ensureInsert("/doc/title", "beta"))
                return EXIT_FAILURE;

            auto stats = exporter->getHistoryStats(rootView);
            if (!stats) {
                std::cerr << "Failed to query history stats: " << formatError(stats.error())
                          << "\n";
                return EXIT_FAILURE;
            }
            if (stats->counts.undo == 0) {
                std::cerr << "Expected at least one undo entry before export\n";
                return EXIT_FAILURE;
            }
        }

        auto encodedRoot      = encodeRoot(rootPath);
        auto exportHistoryDir = exportBase / exportDefaults.persistenceNamespace / encodedRoot;
        if (!fs::exists(exportHistoryDir / "state.meta")) {
            std::cerr << "Export history directory missing expected state.meta at "
                      << exportHistoryDir << "\n";
            return EXIT_FAILURE;
        }
        if (debugLogging) {
            std::cerr << "[debug] Export history dir: " << exportHistoryDir << "\n";
        }

        auto originalSavefile = scratchRoot / "roundtrip.pshd";
        {
            std::error_code ec;
            fs::remove(originalSavefile, ec);
        }

        std::vector<std::string> exportArgs{
            "export",
            "--root",
            rootPath,
            "--history-dir",
            exportHistoryDir.string(),
            "--out",
            originalSavefile.string(),
        };
        if (runCommand(cliPath, exportArgs) != 0)
            return EXIT_FAILURE;
        if (!fs::exists(originalSavefile)) {
            std::cerr << "Export did not produce savefile: " << originalSavefile << "\n";
            return EXIT_FAILURE;
        }
        archiver.addFile(originalSavefile, "original.pshd");

        auto importNamespace = std::string{"cli_roundtrip_import"};
        auto importHistoryDir = importBase / importNamespace / encodedRoot;
        std::vector<std::string> importArgs{
            "import",
            "--root",
            rootPath,
            "--history-dir",
            importHistoryDir.string(),
            "--in",
            originalSavefile.string(),
            "--persistence-root",
            importBase.string(),
            "--namespace",
            importNamespace,
        };
        if (runCommand(cliPath, importArgs) != 0)
            return EXIT_FAILURE;
        if (!fs::exists(importHistoryDir / "state.meta")) {
            std::cerr << "Import did not materialize state.meta at " << importHistoryDir << "\n";
            return EXIT_FAILURE;
        }
        if (debugLogging) {
            std::cerr << "[debug] Import history dir: " << importHistoryDir << "\n";
            std::error_code iterEc;
            for (auto const& entry : fs::directory_iterator(importHistoryDir, iterEc)) {
                std::cerr << "  [entry] " << entry.path() << "\n";
            }
            if (iterEc)
                std::cerr << "  [entry] iteration error: " << iterEc.message() << "\n";
        }

        HistoryOptions importDefaults;
        importDefaults.persistHistory       = true;
        importDefaults.persistenceRoot      = importBase.string();
        importDefaults.persistenceNamespace = importNamespace;
        importDefaults.restoreFromPersistence = true;
        importDefaults.allowNestedUndo      = true;

        auto reloaded = makeUndoableSpace(importDefaults);
        auto enableReload = reloaded->enableHistory(rootView);
        if (!enableReload) {
            std::cerr << "Failed to enable history after import: "
                      << formatError(enableReload.error()) << "\n";
            return EXIT_FAILURE;
        }

        auto statsAfterImport = reloaded->getHistoryStats(rootView);
        if (!statsAfterImport) {
            std::cerr << "Failed to fetch history stats after import: "
                      << formatError(statsAfterImport.error()) << "\n";
            return EXIT_FAILURE;
        }
        if (debugLogging) {
            std::cerr << "[debug] Imported undo count: " << statsAfterImport->counts.undo
                      << " redo count: " << statsAfterImport->counts.redo << "\n";
        }
        if (statsAfterImport->counts.undo == 0) {
            std::cerr << "Import should yield at least one undo entry\n";
            return EXIT_FAILURE;
        }


        auto roundtripSavefile = scratchRoot / "roundtrip-reexport.pshd";
        {
            std::error_code ec;
            fs::remove(roundtripSavefile, ec);
        }

        std::vector<std::string> reExportArgs{
            "export",
            "--root",
            rootPath,
            "--history-dir",
            importHistoryDir.string(),
            "--out",
            roundtripSavefile.string(),
            "--persistence-root",
            importBase.string(),
            "--namespace",
            importNamespace,
        };
        if (runCommand(cliPath, reExportArgs) != 0)
            return EXIT_FAILURE;
        if (!fs::exists(roundtripSavefile)) {
            std::cerr << "Roundtrip export did not produce savefile\n";
            return EXIT_FAILURE;
        }
        archiver.addFile(roundtripSavefile, "roundtrip.pshd");

        auto originalSummary = collectHistorySummary(originalSavefile, rootView, debugLogging);
        auto roundtripSummary = collectHistorySummary(roundtripSavefile, rootView, debugLogging);
        if (!originalSummary || !roundtripSummary) {
            std::cerr << "Failed to collect history summaries for comparison\n";
            return EXIT_FAILURE;
        }
        if (originalSummary->undoCount != roundtripSummary->undoCount
            || originalSummary->redoCount != roundtripSummary->redoCount) {
            std::cerr << "Roundtrip summary counters diverged (undo "
                      << originalSummary->undoCount << " vs " << roundtripSummary->undoCount
                      << ", redo " << originalSummary->redoCount << " vs "
                      << roundtripSummary->redoCount << ")\n";
            return EXIT_FAILURE;
        }
        if (originalSummary->values != roundtripSummary->values) {
            std::cerr << "Roundtrip replay produced differing payload ordering\n";
            return EXIT_FAILURE;
        }
        if (originalSummary->values.size() < 2 || originalSummary->values[0] != "alpha"
            || originalSummary->values[1] != "beta") {
            std::cerr << "Original summary did not contain expected baseline values\n";
            return EXIT_FAILURE;
        }

        HistoryStats importStatsCopy = *statsAfterImport;
        auto         timestamp       = formatTimestampIso();
        auto         telemetryJson   = makeTelemetryJson(timestamp,
                                            originalSavefile,
                                            *originalSummary,
                                            roundtripSavefile,
                                            *roundtripSummary,
                                            importStatsCopy);

        archiver.addText("telemetry.json", telemetryJson);

        std::cout << "History savefile CLI roundtrip verified successfully\n";
        std::cout << "Telemetry: " << telemetryJson;
        return EXIT_SUCCESS;
    } catch (std::exception const& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << "\n";
    } catch (...) {
        std::cerr << "Unhandled unknown exception\n";
    }
    return EXIT_FAILURE;
}
