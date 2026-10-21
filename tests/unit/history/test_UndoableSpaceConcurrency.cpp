#include "UndoableSpaceTestSupport.hpp"

TEST_SUITE_BEGIN("history.undoable.concurrency");

TEST_CASE("journal handles concurrent mutation and history operations") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.maxEntries         = 4096;
    opts.maxBytesRetained   = 512 * 1024;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/stress"}, opts).has_value());

    constexpr int kThreadCount = 4;
    constexpr int kIterations  = 80;

    std::atomic<int> undoSuccess{0};
    std::atomic<int> redoSuccess{0};
    std::atomic<int> gcSuccess{0};

    std::mutex errorMutex;
    std::vector<std::string> errors;

    auto recordError = [&](int threadIndex, std::string message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errors.emplace_back("[thread " + std::to_string(threadIndex) + "] " + std::move(message));
    };

    auto const root = ConcretePathStringView{"/stress"};
    auto* const rawSpace = space.get();

    auto worker = [&](int threadIndex) {
        for (int iter = 0; iter < kIterations; ++iter) {
            std::string key = "/stress/thread";
            key += std::to_string(threadIndex);
            key += "/value";
            key += std::to_string(iter);

            auto insertResult = rawSpace->insert(key.c_str(), iter);
            if (!insertResult.errors.empty()) {
                auto const& err = insertResult.errors.front();
                std::string msg = "insert reported errors for ";
                msg.append(key);
                msg.append(" [code=");
                msg.append(std::to_string(static_cast<int>(err.code)));
                msg.push_back(']');
                if (err.message) {
                    msg.push_back(' ');
                    msg.append(*err.message);
                }
                recordError(threadIndex, std::move(msg));
            }

            switch (iter % 3) {
            case 0: {
                auto taken = rawSpace->take<int>(key.c_str());
                if (taken.has_value()) {
                    if (*taken != iter) {
                        recordError(threadIndex, std::string("take returned unexpected value for ") + key);
                    }
                }

                auto undoResult = rawSpace->undo(root);
                if (undoResult.has_value()) {
                    undoSuccess.fetch_add(1, std::memory_order_relaxed);
                    auto redoResult = rawSpace->redo(root);
                    if (redoResult.has_value()) {
                        redoSuccess.fetch_add(1, std::memory_order_relaxed);
                    } else if (redoResult.error().code != Error::Code::NoObjectFound) {
                        auto msg = std::string("redo returned unexpected error code [code=")
                                   + std::to_string(static_cast<int>(redoResult.error().code)) + "]";
                        if (redoResult.error().message) {
                            msg.push_back(' ');
                            msg.append(*redoResult.error().message);
                        }
                        recordError(threadIndex, std::move(msg));
                    }
                } else if (undoResult.error().code != Error::Code::NoObjectFound) {
                    auto msg = std::string("undo returned unexpected error code [code=")
                               + std::to_string(static_cast<int>(undoResult.error().code)) + "]";
                    if (undoResult.error().message) {
                        msg.push_back(' ');
                        msg.append(*undoResult.error().message);
                    }
                    recordError(threadIndex, std::move(msg));
                }
                break;
            }
            case 1: {
                auto undoResult = rawSpace->undo(root);
                if (undoResult.has_value()) {
                    undoSuccess.fetch_add(1, std::memory_order_relaxed);
                    auto redoResult = rawSpace->redo(root);
                    if (redoResult.has_value()) {
                        redoSuccess.fetch_add(1, std::memory_order_relaxed);
                    } else if (redoResult.error().code != Error::Code::NoObjectFound) {
                        auto msg = std::string("redo returned unexpected error code [code=")
                                   + std::to_string(static_cast<int>(redoResult.error().code)) + "]";
                        if (redoResult.error().message) {
                            msg.push_back(' ');
                            msg.append(*redoResult.error().message);
                        }
                        recordError(threadIndex, std::move(msg));
                    }
                } else if (undoResult.error().code != Error::Code::NoObjectFound) {
                    auto msg = std::string("undo returned unexpected error code [code=")
                               + std::to_string(static_cast<int>(undoResult.error().code)) + "]";
                    if (undoResult.error().message) {
                        msg.push_back(' ');
                        msg.append(*undoResult.error().message);
                    }
                    recordError(threadIndex, std::move(msg));
                }
                break;
            }
            default: {
                auto gcResult = rawSpace->insert("/stress/_history/garbage_collect", true);
                if (!gcResult.errors.empty()) {
                    auto const& err = gcResult.errors.front();
                    std::string msg = "garbage_collect insert reported errors [code=";
                    msg.append(std::to_string(static_cast<int>(err.code)));
                    msg.push_back(']');
                    if (err.message) {
                        msg.push_back(' ');
                        msg.append(*err.message);
                    }
                    recordError(threadIndex, std::move(msg));
                } else {
                    gcSuccess.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            }

            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& workerThread : threads) {
        workerThread.join();
    }

    if (!errors.empty()) {
        for (auto const& err : errors) {
            INFO(err);
        }
        FAIL("concurrent journal stress encountered errors");
    }

    CHECK(undoSuccess.load(std::memory_order_relaxed) > 0);
    CHECK(redoSuccess.load(std::memory_order_relaxed) > 0);
    CHECK(gcSuccess.load(std::memory_order_relaxed) > 0);

    auto stats = space->getHistoryStats(root);
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo >= 0);
    CHECK(stats->counts.redo >= 0);

    auto markerInsert = space->insert("/stress/marker", std::string{"marker"});
    CHECK(markerInsert.errors.empty());

    std::size_t undone = 0;
    for (;;) {
        auto undoResult = space->undo(root);
        if (!undoResult.has_value()) {
            CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        ++undone;
    }
    CHECK(undone > 0);

    std::size_t redone = 0;
    for (;;) {
        auto redoResult = space->redo(root);
        if (!redoResult.has_value()) {
            CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        ++redone;
    }
    CHECK(redone == undone);

    auto markerCleanup = space->take<std::string>("/stress/marker");
    REQUIRE(markerCleanup.has_value());
    CHECK(*markerCleanup == "marker");

    auto postInsert = space->insert("/stress/post_check", std::string{"ok"});
    CHECK(postInsert.errors.empty());

    REQUIRE(space->undo(root).has_value());
    REQUIRE(space->redo(root).has_value());

    auto cleanup = space->take<std::string>("/stress/post_check");
    REQUIRE(cleanup.has_value());
    CHECK(*cleanup == "ok");

    auto finalGc = space->insert("/stress/_history/garbage_collect", true);
    CHECK(finalGc.errors.empty());
}

TEST_CASE("journal fuzz sequence maintains parity with reference model") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal   = true;
    opts.manualGarbageCollect = true;
    opts.maxEntries           = 4096;
    opts.maxBytesRetained     = 2 * 1024 * 1024;
    REQUIRE(space->enableHistory(ConcretePathStringView{"/fuzz"}, opts).has_value());

    struct ReferenceModel {
        enum class Kind { Insert, Take };
        struct Entry {
            Kind kind;
            std::string key;
            std::optional<int> value;
        };

        std::unordered_map<std::string, std::deque<int>> values;
        std::vector<Entry> undoStack;
        std::vector<Entry> redoStack;

        void insert(std::string const& key, int value) {
            Entry entry;
            entry.kind  = Kind::Insert;
            entry.key   = key;
            entry.value = value;
            values[key].push_back(value);
            undoStack.push_back(entry);
            redoStack.clear();
        }

        std::optional<int> take(std::string const& key) {
            auto it = values.find(key);
            if (it == values.end() || it->second.empty()) {
                return std::nullopt;
            }
            Entry entry;
            entry.kind  = Kind::Take;
            entry.key   = key;
            entry.value = it->second.front();
            it->second.pop_front();
            if (it->second.empty()) {
                values.erase(it);
            }
            undoStack.push_back(entry);
            redoStack.clear();
            return entry.value;
        }

        bool undo() {
            if (undoStack.empty()) {
                return false;
            }
            auto entry = undoStack.back();
            undoStack.pop_back();
            switch (entry.kind) {
            case Kind::Insert: {
                auto it = values.find(entry.key);
                if (it != values.end() && !it->second.empty()) {
                    it->second.pop_back();
                    if (it->second.empty()) {
                        values.erase(it);
                    }
                }
                break;
            }
            case Kind::Take:
                if (entry.value.has_value()) {
                    values[entry.key].push_front(*entry.value);
                }
                break;
            }
            redoStack.push_back(entry);
            return true;
        }

        bool redo() {
            if (redoStack.empty()) {
                return false;
            }
            auto entry = redoStack.back();
            redoStack.pop_back();
            switch (entry.kind) {
            case Kind::Insert:
                REQUIRE(entry.value.has_value());
                values[entry.key].push_back(*entry.value);
                break;
            case Kind::Take:
                if (auto it = values.find(entry.key); it != values.end() && !it->second.empty()) {
                    it->second.pop_front();
                    if (it->second.empty()) {
                        values.erase(it);
                    }
                }
                break;
            }
            undoStack.push_back(entry);
            return true;
        }

        std::optional<int> read(std::string const& key) const {
            if (auto it = values.find(key); it != values.end() && !it->second.empty()) {
                return it->second.front();
            }
            return std::nullopt;
        }

        void alignToStats(std::size_t undoCount, std::size_t redoCount) {
            trimStackTo(undoStack, undoCount);
            trimStackTo(redoStack, redoCount);
        }

        std::size_t undoCount() const { return undoStack.size(); }
        std::size_t redoCount() const { return redoStack.size(); }

    private:
        static void trimStackTo(std::vector<Entry>& stack, std::size_t target) {
            REQUIRE(stack.size() >= target);
            while (stack.size() > target) {
                stack.erase(stack.begin());
            }
        }
    };

    ReferenceModel reference;

    constexpr std::array<std::string_view, 6> kKeySuffixes{
        "/value/a", "/value/b", "/value/c", "/value/d", "/value/e", "/value/f"};
    const ConcretePathStringView root{"/fuzz"};

    auto buildKey = [](std::string_view suffix) {
        std::string key{"/fuzz"};
        key.append(suffix);
        return key;
    };

    std::vector<std::string> keyPaths;
    keyPaths.reserve(kKeySuffixes.size());
    for (auto suffix : kKeySuffixes) {
        keyPaths.push_back(buildKey(suffix));
    }

    auto checkStateMatches = [&] {
        for (auto const& key : keyPaths) {
            auto expected = reference.read(key);
            auto actual   = space->read<int>(key.c_str());
            if (expected.has_value()) {
                REQUIRE(actual.has_value());
                CHECK(*actual == *expected);
            } else {
                CHECK_FALSE(actual.has_value());
            }
        }
    };

    std::mt19937 rng{1337};
    std::uniform_int_distribution<int> opDist(0, 5);
    std::uniform_int_distribution<int> keyDist(0, static_cast<int>(keyPaths.size() - 1));
    std::uniform_int_distribution<int> valueDist(-1000, 1000);

    constexpr int kIterations = 250;

    for (int iter = 0; iter < kIterations; ++iter) {
        int opIndex = opDist(rng);
        auto const& key = keyPaths[static_cast<std::size_t>(keyDist(rng))];

        switch (opIndex) {
        case 0:
        case 1: {
            int value = valueDist(rng);
            auto result = space->insert(key.c_str(), value);
            CHECK(result.errors.empty());
            reference.insert(key, value);
            break;
        }
        case 2: {
            auto taken = space->take<int>(key.c_str());
            auto refTaken = reference.take(key);
            CHECK(taken.has_value() == refTaken.has_value());
            if (taken.has_value()) {
                CHECK(*taken == *refTaken);
            }
            break;
        }
        case 3: {
            auto undoResult = space->undo(root);
            bool refUndid   = reference.undo();
            if (undoResult.has_value()) {
                CHECK(refUndid);
            } else {
                CHECK_FALSE(refUndid);
                CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            }
            break;
        }
        case 4: {
            auto redoResult = space->redo(root);
            bool refRedid   = reference.redo();
            if (redoResult.has_value()) {
                CHECK(refRedid);
            } else {
                CHECK_FALSE(refRedid);
                CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            }
            break;
        }
        default: {
            auto gc = space->insert("/fuzz/_history/garbage_collect", true);
            CHECK(gc.errors.empty());
            auto statsAfterGc = space->getHistoryStats(root);
            REQUIRE(statsAfterGc.has_value());
            reference.alignToStats(statsAfterGc->counts.undo, statsAfterGc->counts.redo);
            CHECK(reference.undoCount() == statsAfterGc->counts.undo);
            CHECK(reference.redoCount() == statsAfterGc->counts.redo);
            checkStateMatches();
            continue;
        }
        }

        auto stats = space->getHistoryStats(root);
        REQUIRE(stats.has_value());
        reference.alignToStats(stats->counts.undo, stats->counts.redo);

        CHECK(reference.undoCount() == stats->counts.undo);
        CHECK(reference.redoCount() == stats->counts.redo);

        checkStateMatches();
    }

    // Drain undo stack and ensure redo parity follows.
    for (;;) {
        auto undoResult = space->undo(root);
        bool refUndid   = reference.undo();
        if (!undoResult.has_value()) {
            CHECK_FALSE(refUndid);
            CHECK(undoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        CHECK(refUndid);
        checkStateMatches();
    }

    for (;;) {
        auto redoResult = space->redo(root);
        bool refRedid   = reference.redo();
        if (!redoResult.has_value()) {
            CHECK_FALSE(refRedid);
            CHECK(redoResult.error().code == Error::Code::NoObjectFound);
            break;
        }
        CHECK(refRedid);
        checkStateMatches();
    }
}

TEST_CASE("mutation journal roots require explicit opt-in") {
    auto space = makeUndoableSpace();
    REQUIRE(space);

    HistoryOptions opts;
    opts.useMutationJournal = true;

    REQUIRE(space->enableHistory(ConcretePathStringView{"/journal"}, opts).has_value());

    auto stats = space->getHistoryStats(ConcretePathStringView{"/journal"});
    REQUIRE(stats.has_value());
    CHECK(stats->counts.undo == 0);
    CHECK(stats->counts.redo == 0);

    auto insertResult = space->insert("/journal/value", std::string{"alpha"});
    CHECK(insertResult.errors.empty());

    auto value = space->read<std::string>("/journal/value");
    REQUIRE(value.has_value());
    CHECK(*value == "alpha");

    REQUIRE(space->undo(ConcretePathStringView{"/journal"}).has_value());
    CHECK_FALSE(space->read<std::string>("/journal/value").has_value());

    auto redoResult = space->redo(ConcretePathStringView{"/journal"});
    REQUIRE(redoResult.has_value());
    auto restored = space->read<std::string>("/journal/value");
    REQUIRE(restored.has_value());
    CHECK(*restored == "alpha");

    REQUIRE(space->disableHistory(ConcretePathStringView{"/journal"}).has_value());
}

TEST_SUITE_END();
