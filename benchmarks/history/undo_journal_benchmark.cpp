#include "PathSpace.hpp"
#include "history/UndoableSpace.hpp"
#include "path/ConcretePath.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

using namespace std::chrono_literals;

namespace {

using Clock      = std::chrono::steady_clock;
using DurationMs = std::chrono::duration<double, std::milli>;

struct CliOptions {
    std::size_t operations    = 2000;
    std::size_t payload_bytes = 256;
    std::size_t repeats       = 5;
};

struct ModeConfig {
    std::string_view name;
    bool             use_journal = false;
};

struct RunDurations {
    double commit_ms = 0.0;
    double undo_ms   = 0.0;
    double redo_ms   = 0.0;
};

struct AggregatedStats {
    double best_ms       = 0.0;
    double worst_ms      = 0.0;
    double mean_ms       = 0.0;
    double ops_per_sec   = 0.0;
    std::size_t samples  = 0;
};

[[noreturn]] void print_usage() {
    std::cout << "PathSpace undo journal benchmark\n";
    std::cout << "Usage: undo_journal_benchmark [--operations N] [--payload-bytes N] [--repeats N]\n";
    std::exit(1);
}

auto parse_positive(std::string_view value, std::string_view flag) -> std::size_t {
    std::size_t result = 0;
    auto        begin  = value.data();
    auto        end    = value.data() + value.size();
    auto parsed        = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result == 0) {
        throw std::runtime_error("invalid value for " + std::string(flag));
    }
    return result;
}

auto parse_cli(int argc, char** argv) -> CliOptions {
    CliOptions opts{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
        } else if (arg == "--operations") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--operations requires a value");
            }
            opts.operations = parse_positive(argv[++i], "--operations");
        } else if (arg == "--payload-bytes") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--payload-bytes requires a value");
            }
            opts.payload_bytes = parse_positive(argv[++i], "--payload-bytes");
        } else if (arg == "--repeats") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--repeats requires a value");
            }
            opts.repeats = parse_positive(argv[++i], "--repeats");
        } else {
            throw std::runtime_error("unknown flag: " + std::string(arg));
        }
    }
    return opts;
}

auto make_payloads(std::size_t count, std::size_t payload_bytes) -> std::vector<std::string> {
    std::vector<std::string> payloads;
    payloads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        char fill = static_cast<char>('a' + static_cast<int>(i % 26U));
        payloads.emplace_back(payload_bytes, fill);
    }
    return payloads;
}

auto make_undoable_space(bool use_journal) -> std::unique_ptr<SP::History::UndoableSpace> {
    SP::History::HistoryOptions defaults;
    defaults.useMutationJournal = use_journal;
    auto inner                  = std::make_unique<SP::PathSpace>();
    return std::make_unique<SP::History::UndoableSpace>(std::move(inner), defaults);
}

auto make_path_strings(std::string_view root, std::size_t count) -> std::vector<std::string> {
    std::vector<std::string> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        paths.emplace_back(std::string(root) + "/entries/" + std::to_string(i));
    }
    return paths;
}

auto run_sample(ModeConfig mode,
                CliOptions const& cli,
                std::vector<std::string> const& paths,
                std::vector<std::string> const& payloads) -> RunDurations {
    auto space = make_undoable_space(mode.use_journal);
    if (!space) {
        throw std::runtime_error("failed to construct UndoableSpace");
    }

    auto root_view = SP::ConcretePathStringView{"/bench"};
    SP::History::HistoryOptions opts;
    opts.useMutationJournal = mode.use_journal;
    opts.maxEntries         = std::max<std::size_t>(cli.operations, 128);
    opts.ramCacheEntries    = std::max<std::size_t>(cli.operations, 8);
    auto enable             = space->enableHistory(root_view, opts);
    if (!enable.has_value()) {
        auto message = enable.error().message.value_or("enableHistory failed");
        throw std::runtime_error(std::string(mode.name) + " enableHistory: " + message);
    }

    // Commit latency: measure inserts that create history entries.
    auto commit_start = Clock::now();
    for (std::size_t i = 0; i < cli.operations; ++i) {
        auto path_view = std::string_view{paths[i]};
        auto insert    = space->insert<std::string const&, std::string_view>(path_view, payloads[i]);
        if (!insert.errors.empty()) {
            auto const& err = insert.errors.front();
            auto message    = err.message.value_or("insert failed");
            throw std::runtime_error(std::string(mode.name) + " insert error: " + message);
        }
    }
    auto commit_end = Clock::now();

    auto statsExpected = space->getHistoryStats(root_view);
    if (!statsExpected.has_value()) {
        auto message = statsExpected.error().message.value_or("getHistoryStats failed");
        throw std::runtime_error(std::string(mode.name) + " stats error: " + message);
    }
    if (statsExpected->counts.undo < cli.operations) {
        throw std::runtime_error(std::string(mode.name) + " insufficient undo entries: expected "
                                 + std::to_string(cli.operations) + ", have "
                                 + std::to_string(statsExpected->counts.undo));
    }

    // Undo latency: replay inverse operations.
    auto undo_start = Clock::now();
    for (std::size_t i = 0; i < cli.operations; ++i) {
        auto undone = space->undo(root_view);
        if (!undone.has_value()) {
            auto message = undone.error().message.value_or("undo failed");
            throw std::runtime_error(std::string(mode.name) + " undo error: " + message);
        }
    }
    auto undo_end = Clock::now();

   // Redo latency: reapply the journal.
    auto redo_start = Clock::now();
    for (std::size_t i = 0; i < cli.operations; ++i) {
        auto redone = space->redo(root_view);
        if (!redone.has_value()) {
            auto message = redone.error().message.value_or("redo failed");
            throw std::runtime_error(std::string(mode.name) + " redo error: " + message);
        }
    }
    auto redo_end = Clock::now();

    // Verify we restored the final payload to catch regressions.
    auto final_path  = std::string_view{paths.back()};
    auto final_value = space->read<std::string, std::string_view>(final_path);
    if (!final_value.has_value() || *final_value != payloads.back()) {
        throw std::runtime_error(std::string(mode.name) + " verification failed: final payload mismatch");
    }

    return {
        DurationMs(commit_end - commit_start).count(),
        DurationMs(undo_end - undo_start).count(),
        DurationMs(redo_end - redo_start).count(),
    };
}

auto aggregate(std::vector<double> const& samples, std::size_t operations) -> AggregatedStats {
    AggregatedStats stats{};
    stats.samples = samples.size();
    if (samples.empty()) {
        return stats;
    }
    stats.best_ms  = *std::min_element(samples.begin(), samples.end());
    stats.worst_ms = *std::max_element(samples.begin(), samples.end());
    auto sum       = std::accumulate(samples.begin(), samples.end(), 0.0);
    stats.mean_ms  = sum / static_cast<double>(samples.size());
    if (stats.mean_ms > 0.0) {
        stats.ops_per_sec = static_cast<double>(operations) / (stats.mean_ms / 1000.0);
    }
    return stats;
}

void report_mode(ModeConfig mode,
                 CliOptions const& cli,
                 std::vector<double> const& commit_samples,
                 std::vector<double> const& undo_samples,
                 std::vector<double> const& redo_samples) {
    auto commit_stats = aggregate(commit_samples, cli.operations);
    auto undo_stats   = aggregate(undo_samples, cli.operations);
    auto redo_stats   = aggregate(redo_samples, cli.operations);

    auto print_stats = [](char const* label, AggregatedStats const& stats) {
        std::cout << "  " << std::left << std::setw(6) << label << " mean "
                  << std::right << std::setw(8) << std::fixed << std::setprecision(3) << stats.mean_ms << " ms"
                  << "  best " << std::setw(8) << stats.best_ms << " ms"
                  << "  worst " << std::setw(8) << stats.worst_ms << " ms"
                  << "  ops/s " << std::setw(10) << std::setprecision(1) << std::fixed << stats.ops_per_sec << "\n";
    };

    std::cout << "\nMode: " << mode.name << " (repeats=" << commit_samples.size()
              << ", operations=" << cli.operations << ", payload=" << cli.payload_bytes << " bytes)\n";
    print_stats("commit", commit_stats);
    print_stats("undo", undo_stats);
    print_stats("redo", redo_stats);
}

void run_benchmark(CliOptions const& cli) {
    auto paths     = make_path_strings("/bench", cli.operations);
    auto payloads  = make_payloads(cli.operations, cli.payload_bytes);

    std::vector<double> snapshot_commit;
    std::vector<double> snapshot_undo;
    std::vector<double> snapshot_redo;
    std::vector<double> journal_commit;
    std::vector<double> journal_undo;
    std::vector<double> journal_redo;

    snapshot_commit.reserve(cli.repeats);
    snapshot_undo.reserve(cli.repeats);
    snapshot_redo.reserve(cli.repeats);
    journal_commit.reserve(cli.repeats);
    journal_undo.reserve(cli.repeats);
    journal_redo.reserve(cli.repeats);

    ModeConfig const snapshot_mode{.name = "snapshot", .use_journal = false};
    ModeConfig const journal_mode{.name = "journal", .use_journal = true};

    for (std::size_t i = 0; i < cli.repeats; ++i) {
        auto snapshot = run_sample(snapshot_mode, cli, paths, payloads);
        snapshot_commit.push_back(snapshot.commit_ms);
        snapshot_undo.push_back(snapshot.undo_ms);
        snapshot_redo.push_back(snapshot.redo_ms);

        auto journal = run_sample(journal_mode, cli, paths, payloads);
        journal_commit.push_back(journal.commit_ms);
        journal_undo.push_back(journal.undo_ms);
        journal_redo.push_back(journal.redo_ms);
    }

    report_mode(snapshot_mode, cli, snapshot_commit, snapshot_undo, snapshot_redo);
    report_mode(journal_mode, cli, journal_commit, journal_undo, journal_redo);

    if (!snapshot_commit.empty() && !journal_commit.empty()) {
        auto snapshot_stats = aggregate(snapshot_commit, cli.operations);
        auto journal_stats  = aggregate(journal_commit, cli.operations);
        double ratio        = snapshot_stats.mean_ms > 0.0
                           ? journal_stats.mean_ms / snapshot_stats.mean_ms
                           : 0.0;
        std::cout << "\nRelative commit mean (journal / snapshot): " << std::setprecision(3) << ratio << "x\n";
    }
}

} // namespace

int main(int argc, char** argv) try {
    auto cli = parse_cli(argc, argv);
    run_benchmark(cli);
    return 0;
} catch (std::exception const& ex) {
    std::cerr << "benchmark failed: " << ex.what() << "\n";
    return 1;
}
