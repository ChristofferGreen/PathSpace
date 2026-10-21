#include "UndoableSpaceHistoryCoverageTestSupport.hpp"

namespace {

struct EnvVarGuard {
    explicit EnvVarGuard(std::string varName)
        : name(std::move(varName)) {
        if (auto* value = std::getenv(name.c_str())) {
            previous = std::string{value};
        }
    }

    auto set(std::string const& value) const -> bool {
#ifdef _WIN32
        return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
        return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    }

    ~EnvVarGuard() {
        if (previous) {
#ifdef _WIN32
            (void)_putenv_s(name.c_str(), previous->c_str());
#else
            (void)::setenv(name.c_str(), previous->c_str(), 1);
#endif
        } else {
#ifdef _WIN32
            (void)_putenv_s(name.c_str(), "");
#else
            (void)::unsetenv(name.c_str());
#endif
        }
    }

    std::string                    name;
    std::optional<std::string>     previous;
};

auto makeTempJournalPath(std::string_view label) -> std::filesystem::path {
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / (std::string("journal_") + std::string(label) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir / "journal.log";
}

auto makeTempSavefilePath(std::string_view label) -> std::filesystem::path {
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / (std::string("savefile_") + std::string(label) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir / "history.save";
}

auto makeTempHistoryRoot(std::string_view label) -> std::filesystem::path {
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / (std::string("history_root_") + std::string(label) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

auto writeSavefile(std::filesystem::path const& path,
                   UndoSavefile::Document const& document) -> void {
    auto encoded = UndoSavefile::encode(document);
    REQUIRE(encoded.has_value());
    auto span = std::span<const std::byte>(encoded->data(), encoded->size());
    auto write = UndoUtilsAlias::writeFileAtomic(path, span, false, true);
    REQUIRE(write.has_value());
}

} // namespace

TEST_SUITE_BEGIN("history.undoable.history.coverage.persistence");

TEST_CASE("UndoableSpace persistence setup validates tokens") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";

    state.options.persistenceNamespace = "bad/ns";
    auto badNamespace = space.ensureJournalPersistenceSetup(state);
    CHECK_FALSE(badNamespace.has_value());
    CHECK(badNamespace.error().code == Error::Code::InvalidPermissions);

    state.options.persistenceNamespace = "";
    state.encodedRoot = "bad/ns";
    auto badRoot = space.ensureJournalPersistenceSetup(state);
    CHECK_FALSE(badRoot.has_value());
    CHECK(badRoot.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace loadJournalPersistence resets on missing file") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    state.journalPath = std::filesystem::temp_directory_path()
                        / ("missing_journal_" + suffix + ".log");
    state.liveBytes = 123;
    state.telemetry.trimmedEntries = 7;
    state.telemetry.trimmedBytes = 8;
    state.telemetry.trimOperations = 2;

    auto result = space.loadJournalPersistence(state);
    CHECK(result.has_value());
    CHECK(state.liveBytes == 0);
    CHECK(state.nextSequence == 0);
    CHECK(state.telemetry.trimmedEntries == 0);
    CHECK(state.telemetry.trimmedBytes == 0);
    CHECK(state.telemetry.trimOperations == 0);
    CHECK(state.telemetry.diskEntries == 0);
}

TEST_CASE("UndoableSpace loadJournalPersistence replays valid entries") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};

    state.journalPath = makeTempJournalPath("replay");

    JournalEntry entry;
    entry.path = "/doc/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
        REQUIRE(writer.flush().has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK(result.has_value());
    CHECK(state.journal.size() == 1);
    CHECK(state.journal.entryAt(0).path == "/doc/value");
}

TEST_CASE("UndoableSpace loadJournalPersistence surfaces decode failures") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("bad_payload");

    JournalEntry entry;
    entry.path = "/doc/value";
    entry.value.present = true;

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::MalformedInput);
}

TEST_CASE("UndoableSpace loadJournalPersistence rejects entries outside root") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("bad_root");

    JournalEntry entry;
    entry.path = "/other/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::InvalidPermissions);
}

TEST_CASE("UndoableSpace loadJournalPersistence reports missing backend") {
    using namespace SP::History::UndoJournal;
    UndoableSpace space{std::unique_ptr<PathSpaceBase>{}, {}};
    UndoJournalRootState state;
    state.persistenceEnabled = true;
    state.rootPath = "/doc";
    state.components = {"doc"};
    state.journalPath = makeTempJournalPath("no_backend");

    JournalEntry entry;
    entry.path = "/doc/value";

    {
        JournalFileWriter writer(state.journalPath);
        REQUIRE(writer.open(false).has_value());
        REQUIRE(writer.append(entry).has_value());
    }

    auto result = space.loadJournalPersistence(state);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace updateJournalDiskTelemetry zeros when disabled") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = false;
    state.telemetry.diskBytes = 5;
    state.telemetry.diskEntries = 4;

    space.updateJournalDiskTelemetry(state);
    CHECK(state.telemetry.diskBytes == 0);
    CHECK(state.telemetry.diskEntries == 0);
}

TEST_CASE("UndoableSpace persistence helpers no-op when disabled") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    UndoJournalRootState state;
    state.persistenceEnabled = false;

    auto setup = space.ensureJournalPersistenceSetup(state);
    CHECK(setup.has_value());

    auto load = space.loadJournalPersistence(state);
    CHECK(load.has_value());

    auto compact = space.compactJournalPersistence(state, false);
    CHECK(compact.has_value());
}

TEST_CASE("UndoableSpace persistence root helpers honor env override") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    EnvVarGuard envGuard("PATHSPACE_HISTORY_ROOT");

    auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto envRoot = std::filesystem::temp_directory_path() / ("history_env_root_" + suffix);
    REQUIRE(envGuard.set(envRoot.string()));

    CHECK(space.defaultPersistenceRoot() == (envRoot / "history"));

    auto explicitRoot = std::filesystem::temp_directory_path() / ("history_explicit_root_" + suffix);
    HistoryOptions opts;
    opts.persistenceRoot = explicitRoot.string();
    CHECK(space.persistenceRootPath(opts) == explicitRoot);
}

TEST_CASE("UndoableSpace encodeRootForPersistence replaces separators") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};

    CHECK(space.encodeRootForPersistence("") == "__root");
    CHECK(space.encodeRootForPersistence("/") == "__root");
    CHECK(space.encodeRootForPersistence("/doc") == "_doc");
    CHECK(space.encodeRootForPersistence("/doc/child") == "_doc_child");
}

TEST_CASE("UndoableSpace importHistorySavefile applies options and defaults ram cache") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    stateIt->second->options.ramCacheEntries = 0;

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.options.maxEntries = 12;
    document.options.maxBytesRetained = 256;
    document.options.maxDiskBytes = 512;
    document.options.keepLatestForMs = 0;
    document.options.manualGarbageCollect = true;
    document.undoCount = 0;
    document.nextSequence = 0;

    auto savefile = makeTempSavefilePath("apply_options");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, true);
    CHECK(result.has_value());
    CHECK(stateIt->second->options.ramCacheEntries == 8);
}

TEST_CASE("UndoableSpace importHistorySavefile propagates decode undo-count errors") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.entries.push_back(makeSavefileEntry("/doc/value"));
    document.undoCount = 2;

    auto savefile = makeTempSavefilePath("undo_count");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace importHistorySavefile reports missing backend on apply") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    UndoSavefile::Document document;
    document.rootPath = "/doc";
    document.entries.push_back(makeSavefileEntry("/doc/value"));
    document.undoCount = 1;

    auto savefile = makeTempSavefilePath("missing_backend");
    writeSavefile(savefile, document);

    space.inner.reset();
    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace importHistorySavefile propagates persistence failures") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.persistHistory = true;
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    stateIt->second->persistenceEnabled = true;

    auto parentFile = makeTempSavefilePath("persist_parent");
    {
        std::ofstream stream(parentFile);
        REQUIRE(stream.good());
    }
    stateIt->second->journalPath = parentFile / "journal.log";

    UndoSavefile::Document document;
    document.rootPath = "/doc";

    auto savefile = makeTempSavefilePath("persist_error");
    writeSavefile(savefile, document);

    auto result = space.importHistorySavefile(ConcretePathStringView{"/doc"}, savefile, false);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::UnknownError);
}

TEST_CASE("UndoableSpace garbage collect compacts persistence on trim") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.persistHistory = true;
    opts.manualGarbageCollect = true;
    opts.maxEntries = 1;
    auto root = makeTempHistoryRoot("gc_trim");
    opts.persistenceRoot = root.string();
    opts.persistenceNamespace = "gc_trim";
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space.insert("/doc/a", 1).errors.empty());
    REQUIRE(space.insert("/doc/b", 2).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    auto state = stateIt->second;
    REQUIRE(state);

    auto gc = space.insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    CHECK_FALSE(state->persistenceDirty);
    CHECK_FALSE(state->telemetry.persistenceDirty);
    CHECK(state->telemetry.trimmedEntries > 0);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("UndoableSpace garbage collect updates persistence telemetry without trim") {
    UndoableSpace space{std::make_unique<PathSpace>(), {}};
    HistoryOptions opts;
    opts.useMutationJournal = true;
    opts.persistHistory = true;
    opts.manualGarbageCollect = true;
    opts.maxEntries = 10;
    auto root = makeTempHistoryRoot("gc_no_trim");
    opts.persistenceRoot = root.string();
    opts.persistenceNamespace = "gc_no_trim";
    REQUIRE(space.enableHistory(ConcretePathStringView{"/doc"}, opts).has_value());

    REQUIRE(space.insert("/doc/a", 1).errors.empty());

    auto stateIt = space.journalRoots.find("/doc");
    REQUIRE(stateIt != space.journalRoots.end());
    auto state = stateIt->second;
    REQUIRE(state);

    auto gc = space.insert("/doc/_history/garbage_collect", true);
    CHECK(gc.errors.empty());

    CHECK_FALSE(state->persistenceDirty);
    CHECK_FALSE(state->telemetry.persistenceDirty);
    CHECK(state->telemetry.diskEntries == state->journal.stats().totalEntries);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_SUITE_END();
