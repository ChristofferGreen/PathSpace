#include <pathspace/PathSpace.hpp>
#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <cstdlib>
#include <filesystem>
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
    std::size_t              undoCount = 0;
    std::size_t              redoCount = 0;
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
    summary.undoCount = stats->counts.undo;
    summary.redoCount = stats->counts.redo;

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

        std::cout << "History savefile CLI roundtrip verified successfully\n";
        return EXIT_SUCCESS;
    } catch (std::exception const& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << "\n";
    } catch (...) {
        std::cerr << "Unhandled unknown exception\n";
    }
    return EXIT_FAILURE;
}
