#include <pathspace/PathSpace.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace SP;

namespace {

struct Scenario {
    std::string name;
    std::size_t depth = 0;
    std::size_t breadth = 0;
    std::size_t nestedDepth = 0;
    std::size_t nestedBreadth = 0;
    std::size_t valuesPerLeaf = 1;
};

struct RunStats {
    double buildMs = 0.0;
    double readMs = 0.0;
    double totalMs = 0.0;
    std::size_t readCount = 0;
};

struct Options {
    std::size_t warmupRuns = 1;
    std::size_t runs = 10;
    double scale = 1.0;
    enum class Engine { PathSpace, ArrayTrie, SnapshotArrayTrie };
    Engine engine = Engine::PathSpace;
    std::size_t maxReads = std::numeric_limits<std::size_t>::max();
    std::size_t maxInserts = 10'000;
};

auto now() -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::now();
}

auto toMs(std::chrono::steady_clock::duration d) -> double {
    using Ms = std::chrono::duration<double, std::milli>;
    return Ms{d}.count();
}

auto clampSize(std::size_t value, double scale) -> std::size_t {
    auto scaled = static_cast<std::size_t>(static_cast<double>(value) * scale);
    return scaled > 0 ? scaled : 1;
}

struct ReadPaths {
    std::vector<std::string> paths;
    std::size_t maxCount = std::numeric_limits<std::size_t>::max();

    void reserve(std::size_t count) { paths.reserve(count); }

    auto add(std::string value) -> bool {
        if (paths.size() >= maxCount) {
            return false;
        }
        paths.push_back(std::move(value));
        return true;
    }
};

struct InsertLimiter {
    std::size_t maxInserts = std::numeric_limits<std::size_t>::max();
    std::size_t count = 0;

    [[nodiscard]] auto enabled() const noexcept -> bool {
        return maxInserts != std::numeric_limits<std::size_t>::max();
    }

    [[nodiscard]] auto allow() -> bool {
        if (count >= maxInserts) {
            return false;
        }
        ++count;
        return true;
    }

    [[nodiscard]] auto maxExpansion() const noexcept -> std::size_t {
        return enabled() ? maxInserts : std::numeric_limits<std::size_t>::max();
    }
};

void addLeafValues(PathSpace& space,
                   std::string const& basePath,
                   std::size_t valuesPerLeaf,
                   ReadPaths& readPaths,
                   InsertLimiter& limiter) {
    for (std::size_t i = 0; i < valuesPerLeaf; ++i) {
        if (!limiter.allow()) {
            return;
        }
        std::string path = basePath;
        path.append("/value");
        if (i > 0) {
            path.push_back('_');
            path.append(std::to_string(i));
        }
        space.insert(path, static_cast<int>(i));
        readPaths.add(std::move(path));
    }
}

struct ArrayTrie {
    struct TempNode {
        std::unordered_map<std::string, std::uint32_t> children;
        bool hasValue = false;
    };

    struct Node {
        std::uint32_t firstChild = 0;
        std::uint32_t childCount = 0;
        bool hasValue = false;
    };

    struct Edge {
        std::uint32_t labelIndex = 0;
        std::uint32_t childIndex = 0;
    };

    ArrayTrie() {
        tempNodes.emplace_back();
    }

    void insert(std::string_view path) {
        std::uint32_t current = 0;
        forEachComponent(path, [&](std::string_view component) {
            auto& node = tempNodes[current];
            auto [iter, inserted] = node.children.emplace(std::string(component), 0u);
            if (inserted) {
                tempNodes.emplace_back();
                iter->second = static_cast<std::uint32_t>(tempNodes.size() - 1);
            }
            current = iter->second;
        });
        tempNodes[current].hasValue = true;
    }

    void finalize() {
        nodes.resize(tempNodes.size());
        edges.clear();
        labels.clear();

        for (std::size_t index = 0; index < tempNodes.size(); ++index) {
            auto const& temp = tempNodes[index];
            Node node{};
            node.firstChild = static_cast<std::uint32_t>(edges.size());
            node.childCount = static_cast<std::uint32_t>(temp.children.size());
            node.hasValue = temp.hasValue;

            std::vector<std::pair<std::string, std::uint32_t>> sorted;
            sorted.reserve(temp.children.size());
            for (auto const& [label, child] : temp.children) {
                sorted.emplace_back(label, child);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](auto const& lhs, auto const& rhs) { return lhs.first < rhs.first; });

            for (auto const& [label, child] : sorted) {
                labels.push_back(label);
                edges.push_back(Edge{
                    static_cast<std::uint32_t>(labels.size() - 1),
                    child,
                });
            }

            nodes[index] = node;
        }
    }

    auto contains(std::string_view path) const -> bool {
        std::uint32_t current = 0;
        bool found = true;
        forEachComponent(path, [&](std::string_view component) {
            if (!found) {
                return;
            }
            auto const& node = nodes[current];
            auto start = edges.begin() + static_cast<std::ptrdiff_t>(node.firstChild);
            auto end = start + static_cast<std::ptrdiff_t>(node.childCount);
            auto iter = std::lower_bound(start, end, component,
                                         [&](Edge const& edge, std::string_view value) {
                                             return std::string_view{labels[edge.labelIndex]} < value;
                                         });
            if (iter == end || std::string_view{labels[iter->labelIndex]} != component) {
                found = false;
                return;
            }
            current = iter->childIndex;
        });
        return found && nodes[current].hasValue;
    }

private:
    template <typename Fn>
    static void forEachComponent(std::string_view path, Fn&& fn) {
        std::size_t start = 0;
        if (!path.empty() && path.front() == '/') {
            start = 1;
        }
        while (start < path.size()) {
            auto slash = path.find('/', start);
            if (slash == std::string_view::npos) {
                slash = path.size();
            }
            if (slash > start) {
                fn(path.substr(start, slash - start));
            }
            start = slash + 1;
        }
    }

    std::vector<TempNode> tempNodes;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<std::string> labels;
};

auto buildSnapshotArray(PathSpace& space, ArrayTrie& trie) -> bool {
    VisitOptions options{};
    options.root = "/";
    options.maxDepth = VisitOptions::UnlimitedDepth;
    options.maxChildren = VisitOptions::UnlimitedChildren;
    options.includeNestedSpaces = true;
    options.includeValues = false;

    auto result = space.visit(
        [&](PathEntry const& entry, ValueHandle&) {
            if (entry.hasValue) {
                trie.insert(entry.path);
            }
            return VisitControl::Continue;
        },
        options);

    return result.has_value();
}

template <typename InsertFn>
void addLeafValuesFlat(InsertFn&& insert,
                       std::string const& basePath,
                       std::size_t valuesPerLeaf,
                       ReadPaths& readPaths,
                       InsertLimiter& limiter) {
    for (std::size_t i = 0; i < valuesPerLeaf; ++i) {
        if (!limiter.allow()) {
            return;
        }
        std::string path = basePath;
        path.append("/value");
        if (i > 0) {
            path.push_back('_');
            path.append(std::to_string(i));
        }
        insert(path);
        readPaths.add(std::move(path));
    }
}

template <typename InsertFn>
void buildWideTreeFlat(InsertFn&& insert,
                       std::string const& basePath,
                       std::size_t depth,
                       std::size_t breadth,
                       std::size_t valuesPerLeaf,
                       ReadPaths& readPaths,
                       InsertLimiter& limiter) {
    std::vector<std::string> current{basePath};
    for (std::size_t level = 0; level < depth; ++level) {
        auto nextSize = current.size() * breadth;
        if (nextSize > limiter.maxExpansion()) {
            break;
        }
        std::vector<std::string> next;
        next.reserve(current.size() * breadth);
        for (auto const& prefix : current) {
            for (std::size_t b = 0; b < breadth; ++b) {
                std::string node = prefix;
                node.push_back('/');
                node.append("n");
                node.append(std::to_string(level));
                node.push_back('_');
                node.append(std::to_string(b));
                next.push_back(std::move(node));
            }
        }
        current = std::move(next);
    }

    for (auto const& leaf : current) {
        addLeafValuesFlat(insert, leaf, valuesPerLeaf, readPaths, limiter);
        if (limiter.count >= limiter.maxInserts) {
            return;
        }
    }
}

template <typename InsertFn>
void buildNestedChainFlat(InsertFn&& insert,
                          std::size_t nestedDepth,
                          std::size_t branchWidth,
                          std::size_t leafDepth,
                          std::size_t breadth,
                          std::size_t valuesPerLeaf,
                          ReadPaths& readPaths,
                          InsertLimiter& limiter) {
    std::string pathPrefix;

    for (std::size_t i = 0; i < nestedDepth; ++i) {
        std::string mount = "/chain_" + std::to_string(i);
        pathPrefix.append(mount);

        for (std::size_t b = 0; b < branchWidth; ++b) {
            std::string branchRoot = "/branch_" + std::to_string(i) + "_" + std::to_string(b);
            buildWideTreeFlat(insert, pathPrefix + branchRoot, leafDepth, breadth, valuesPerLeaf, readPaths, limiter);
            if (limiter.count >= limiter.maxInserts) {
                return;
            }
            std::string marker = pathPrefix + branchRoot + "/marker";
            if (!limiter.allow()) {
                return;
            }
            insert(marker);
            readPaths.add(std::move(marker));
        }
    }
}

template <typename InsertFn>
void buildNestedFanoutFlat(InsertFn&& insert,
                           std::size_t nestedBreadth,
                           std::size_t depth,
                           std::size_t breadth,
                           std::size_t valuesPerLeaf,
                           ReadPaths& readPaths,
                           InsertLimiter& limiter) {
    for (std::size_t i = 0; i < nestedBreadth; ++i) {
        std::string mount = "/space_" + std::to_string(i);
        buildWideTreeFlat(insert, mount, depth, breadth, valuesPerLeaf, readPaths, limiter);
        if (limiter.count >= limiter.maxInserts) {
            return;
        }
    }
}

void buildWideTree(PathSpace& space,
                   std::string const& basePath,
                   std::size_t depth,
                   std::size_t breadth,
                   std::size_t valuesPerLeaf,
                   ReadPaths& readPaths,
                   InsertLimiter& limiter) {
    std::vector<std::string> current{basePath};
    for (std::size_t level = 0; level < depth; ++level) {
        auto nextSize = current.size() * breadth;
        if (nextSize > limiter.maxExpansion()) {
            break;
        }
        std::vector<std::string> next;
        next.reserve(current.size() * breadth);
        for (auto const& prefix : current) {
            for (std::size_t b = 0; b < breadth; ++b) {
                std::string node = prefix;
                node.push_back('/');
                node.append("n");
                node.append(std::to_string(level));
                node.push_back('_');
                node.append(std::to_string(b));
                next.push_back(std::move(node));
            }
        }
        current = std::move(next);
    }

    for (auto const& leaf : current) {
        addLeafValues(space, leaf, valuesPerLeaf, readPaths, limiter);
        if (limiter.count >= limiter.maxInserts) {
            return;
        }
    }
}

void buildNestedChain(PathSpace& root,
                      std::size_t nestedDepth,
                      std::size_t branchWidth,
                      std::size_t leafDepth,
                      std::size_t breadth,
                      std::size_t valuesPerLeaf,
                      ReadPaths& readPaths,
                      InsertLimiter& limiter) {
    PathSpace* current = &root;
    std::string pathPrefix;

    for (std::size_t i = 0; i < nestedDepth; ++i) {
        if (!limiter.allow()) {
            return;
        }
        auto child = std::make_unique<PathSpace>();
        PathSpace* childPtr = child.get();
        std::string mount = "/chain_" + std::to_string(i);
        current->insert(mount, std::move(child));

        pathPrefix.append(mount);
        current = childPtr;

        for (std::size_t b = 0; b < branchWidth; ++b) {
            std::string branchRoot = "/branch_" + std::to_string(i) + "_" + std::to_string(b);
            auto startIndex = readPaths.paths.size();
            buildWideTree(*current, branchRoot, leafDepth, breadth, valuesPerLeaf, readPaths, limiter);
            for (std::size_t idx = startIndex; idx < readPaths.paths.size(); ++idx) {
                readPaths.paths[idx] = pathPrefix + readPaths.paths[idx];
            }
            if (!limiter.allow()) {
                return;
            }
            current->insert(branchRoot + "/marker", static_cast<int>(b));
            readPaths.add(pathPrefix + branchRoot + "/marker");
            if (limiter.count >= limiter.maxInserts) {
                return;
            }
        }
    }
}

void buildNestedFanout(PathSpace& root,
                       std::size_t nestedBreadth,
                       std::size_t depth,
                       std::size_t breadth,
                       std::size_t valuesPerLeaf,
                       ReadPaths& readPaths,
                       InsertLimiter& limiter) {
    for (std::size_t i = 0; i < nestedBreadth; ++i) {
        if (!limiter.allow()) {
            return;
        }
        auto child = std::make_unique<PathSpace>();
        PathSpace* childPtr = child.get();
        std::string mount = "/space_" + std::to_string(i);
        root.insert(mount, std::move(child));

        auto startIndex = readPaths.paths.size();
        buildWideTree(*childPtr, "", depth, breadth, valuesPerLeaf, readPaths, limiter);
        for (std::size_t idx = startIndex; idx < readPaths.paths.size(); ++idx) {
            readPaths.paths[idx] = mount + readPaths.paths[idx];
        }
        if (limiter.count >= limiter.maxInserts) {
            return;
        }
    }
}

auto readAll(PathSpace& space, ReadPaths const& readPaths) -> std::size_t {
    std::size_t count = 0;
    for (auto const& path : readPaths.paths) {
        auto value = space.read<int>(path, Block{});
        if (value.has_value()) {
            ++count;
        }
    }
    return count;
}

auto runScenario(Scenario const& scenario, Options const& options) -> std::vector<RunStats> {
    std::vector<RunStats> stats;
    stats.reserve(options.runs);

    std::size_t depth = clampSize(scenario.depth, options.scale);
    std::size_t breadth = clampSize(scenario.breadth, options.scale);
    std::size_t nestedDepth = clampSize(scenario.nestedDepth, options.scale);
    std::size_t nestedBreadth = clampSize(scenario.nestedBreadth, options.scale);
    std::size_t valuesPerLeaf = clampSize(scenario.valuesPerLeaf, 1.0);

    for (std::size_t run = 0; run < options.warmupRuns + options.runs; ++run) {
        ReadPaths readPaths;
        readPaths.maxCount = options.maxReads;
        readPaths.reserve(1024);
        InsertLimiter limiter;
        if (options.maxInserts == std::numeric_limits<std::size_t>::max()) {
            limiter.maxInserts = options.maxInserts;
        } else {
            limiter.maxInserts = std::max<std::size_t>(1, options.maxInserts);
        }

        auto buildStart = now();
        std::size_t readCount = 0;
        if (options.engine == Options::Engine::PathSpace) {
            PathSpace space;
            if (nestedDepth > 0) {
                buildNestedChain(space, nestedDepth, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else if (nestedBreadth > 0) {
                buildNestedFanout(space, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else {
                buildWideTree(space, "", depth, breadth, valuesPerLeaf, readPaths, limiter);
            }
            auto buildEnd = now();

            auto readStart = now();
            readCount = readAll(space, readPaths);
            auto readEnd = now();

            if (run >= options.warmupRuns) {
                RunStats sample{};
                sample.buildMs = toMs(buildEnd - buildStart);
                sample.readMs = toMs(readEnd - readStart);
                sample.totalMs = toMs(readEnd - buildStart);
                sample.readCount = readCount;
                stats.push_back(sample);
            }
            continue;
        }

        ArrayTrie trie;
        if (options.engine == Options::Engine::ArrayTrie) {
            auto insertValue = [&](std::string const& path) {
                trie.insert(path);
            };
            if (nestedDepth > 0) {
                buildNestedChainFlat(insertValue, nestedDepth, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else if (nestedBreadth > 0) {
                buildNestedFanoutFlat(insertValue, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else {
                buildWideTreeFlat(insertValue, "", depth, breadth, valuesPerLeaf, readPaths, limiter);
            }
        } else {
            PathSpace space;
            if (nestedDepth > 0) {
                buildNestedChain(space, nestedDepth, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else if (nestedBreadth > 0) {
                buildNestedFanout(space, nestedBreadth, depth, breadth, valuesPerLeaf, readPaths, limiter);
            } else {
                buildWideTree(space, "", depth, breadth, valuesPerLeaf, readPaths, limiter);
            }
            if (!buildSnapshotArray(space, trie)) {
                break;
            }
        }

        trie.finalize();
        auto buildEnd = now();

        auto readStart = now();
        for (auto const& path : readPaths.paths) {
            if (trie.contains(path)) {
                ++readCount;
            }
        }
        auto readEnd = now();

        if (run >= options.warmupRuns) {
            RunStats sample{};
            sample.buildMs = toMs(buildEnd - buildStart);
            sample.readMs = toMs(readEnd - readStart);
            sample.totalMs = toMs(readEnd - buildStart);
            sample.readCount = readCount;
            stats.push_back(sample);
        }
    }

    return stats;
}

auto percentile(std::vector<double> values, double p) -> double {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    double pos = (p / 100.0) * (values.size() - 1);
    auto idx = static_cast<std::size_t>(pos);
    double frac = pos - static_cast<double>(idx);
    if (idx + 1 < values.size()) {
        return values[idx] * (1.0 - frac) + values[idx + 1] * frac;
    }
    return values.back();
}

void printStats(std::string_view label, std::vector<RunStats> const& stats) {
    std::vector<double> build;
    std::vector<double> read;
    std::vector<double> total;
    build.reserve(stats.size());
    read.reserve(stats.size());
    total.reserve(stats.size());
    std::size_t reads = 0;
    for (auto const& s : stats) {
        build.push_back(s.buildMs);
        read.push_back(s.readMs);
        total.push_back(s.totalMs);
        reads = s.readCount;
    }

    auto mean = [](std::vector<double> const& v) {
        double sum = std::accumulate(v.begin(), v.end(), 0.0);
        return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
    };

    std::cout << "\nScenario: " << label << "\n";
    std::cout << "  runs: " << stats.size() << ", reads per run: " << reads << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  build ms: mean " << mean(build)
              << ", p50 " << percentile(build, 50)
              << ", p95 " << percentile(build, 95) << "\n";
    std::cout << "  read  ms: mean " << mean(read)
              << ", p50 " << percentile(read, 50)
              << ", p95 " << percentile(read, 95) << "\n";
    std::cout << "  total ms: mean " << mean(total)
              << ", p50 " << percentile(total, 50)
              << ", p95 " << percentile(total, 95) << "\n";
}

auto parseOptions(int argc, char** argv) -> Options {
    Options options{};
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        auto takeValue = [&](std::string_view flag) -> std::optional<std::string_view> {
            if (arg == flag && i + 1 < argc) {
                return std::string_view{argv[++i]};
            }
            return std::nullopt;
        };
        if (auto val = takeValue("--runs")) {
            options.runs = std::max<std::size_t>(1, static_cast<std::size_t>(std::stoul(std::string(*val))));
        } else if (auto val = takeValue("--warmup")) {
            options.warmupRuns = static_cast<std::size_t>(std::stoul(std::string(*val)));
        } else if (auto val = takeValue("--scale")) {
            options.scale = std::max(0.1, std::stod(std::string(*val)));
        } else if (auto val = takeValue("--engine")) {
            if (*val == "array") {
                options.engine = Options::Engine::ArrayTrie;
            } else if (*val == "snapshot") {
                options.engine = Options::Engine::SnapshotArrayTrie;
            } else {
                options.engine = Options::Engine::PathSpace;
            }
        } else if (auto val = takeValue("--max-reads")) {
            auto parsed = static_cast<std::size_t>(std::stoull(std::string(*val)));
            options.maxReads = parsed == 0 ? std::numeric_limits<std::size_t>::max() : parsed;
        } else if (auto val = takeValue("--max-inserts")) {
            auto parsed = static_cast<std::size_t>(std::stoull(std::string(*val)));
            options.maxInserts = parsed == 0 ? std::numeric_limits<std::size_t>::max() : parsed;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parseOptions(argc, argv);

    std::vector<Scenario> scenarios = {
        {"Wide tree", 3, 12, 0, 0, 2},
        {"Deep chain", 9, 4, 5, 2, 1},
        {"Nested chain", 2, 8, 4, 2, 1},
        {"Nested fanout", 2, 6, 0, 6, 2},
    };

    std::cout << "PathSpace hierarchy benchmark\n";
    std::cout << "  warmup runs: " << options.warmupRuns << "\n";
    std::cout << "  measured runs: " << options.runs << "\n";
    std::cout << "  scale: " << options.scale << "\n";
    if (options.maxReads != std::numeric_limits<std::size_t>::max()) {
        std::cout << "  max reads: " << options.maxReads << "\n";
    }
    if (options.maxInserts != std::numeric_limits<std::size_t>::max()) {
        std::cout << "  max inserts: " << options.maxInserts << "\n";
    }
    std::cout << "  engine: ";
    if (options.engine == Options::Engine::PathSpace) {
        std::cout << "pathspace\n";
    } else if (options.engine == Options::Engine::ArrayTrie) {
        std::cout << "array\n";
    } else {
        std::cout << "snapshot\n";
    }

    for (auto const& scenario : scenarios) {
        auto stats = runScenario(scenario, options);
        printStats(scenario.name, stats);
    }

    return 0;
}
