#pragma once

#define private public
#include "history/UndoableSpace.hpp"
#undef private
#include "PathSpace.hpp"
#include "core/NodeData.hpp"
#include "history/UndoHistoryUtils.hpp"
#include "history/UndoJournalPersistence.hpp"
#include "history/UndoSavefileCodec.hpp"
#include "third_party/doctest.h"
#include "type/InputData.hpp"
#include "type/InputMetadataT.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace SP;
using namespace SP::History;
namespace UndoPaths = SP::History::UndoUtils::Paths;
namespace UndoUtilsAlias = SP::History::UndoUtils;

namespace {

[[maybe_unused]] auto makeSavefileEntry(std::string path) -> UndoJournal::JournalEntry {
    UndoJournal::JournalEntry entry;
    entry.operation = UndoJournal::OperationKind::Insert;
    entry.path = std::move(path);
    entry.sequence = 1;
    return entry;
}

} // namespace
