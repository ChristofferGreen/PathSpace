#include "NodeData.hpp"
#include "PathSpaceBase.hpp"
#include "log/TaggedLogger.hpp"
#include "type/InputData.hpp"
#include "core/ExecutionCategory.hpp"
#include "task/Task.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace {
constexpr std::uint32_t HISTORY_PAYLOAD_VERSION = 2;
std::function<void()> g_borrowWaitHook;
std::function<std::optional<SP::Error>()> g_nestedSerializeHook;

template <typename T>
void appendScalar(std::vector<std::byte>& bytes, T value) {
    std::uint8_t buffer[sizeof(T)];
    std::memcpy(buffer, &value, sizeof(T));
    auto const base = reinterpret_cast<std::byte const*>(buffer);
    bytes.insert(bytes.end(), base, base + sizeof(T));
}

template <typename T>
auto readScalar(std::span<const std::byte>& span) -> std::optional<T> {
    if (span.size() < sizeof(T))
        return std::nullopt;
    T value{};
    std::memcpy(&value, span.data(), sizeof(T));
    span = span.subspan(sizeof(T));
    return value;
}
} // namespace

namespace SP {

std::optional<Error> NodeData::validateNested(std::unique_ptr<PathSpaceBase> const& space) const {
    if (g_nestedSerializeHook) {
        if (auto maybeErr = g_nestedSerializeHook()) {
            return maybeErr;
        }
    }
    if (!space) {
        return Error{Error::Code::InvalidType, "UniquePtr payload is null"};
    }
    return std::nullopt;
}

NodeData::~NodeData() = default;

NodeData::NodeData(NodeData&& other) noexcept
    : data(std::move(other.data)),
      types(std::move(other.types)),
      tasks(std::move(other.tasks)),
      futures(std::move(other.futures)),
      anyFutures(std::move(other.anyFutures)),
      valueSizes(std::move(other.valueSizes)),
      nestedSlots(std::move(other.nestedSlots)) {
}

NodeData& NodeData::operator=(NodeData&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    data              = std::move(other.data);
    types             = std::move(other.types);
    tasks             = std::move(other.tasks);
    futures           = std::move(other.futures);
    anyFutures        = std::move(other.anyFutures);
    valueSizes        = std::move(other.valueSizes);
    nestedSlots       = std::move(other.nestedSlots);
    return *this;
}

NodeData::NodeData(NodeData const& other)
    : data(other.data),
      types(other.types),
      tasks(other.tasks),
      futures(other.futures),
      anyFutures(other.anyFutures),
      valueSizes(other.valueSizes),
      nestedSlots{} {
    if (other.hasNestedSpaces()) {
        sp_log("NodeData copy dropped nested PathSpaces (not copyable)", "NodeData");
        dropNestedTypes();
    }
}

NodeData& NodeData::operator=(NodeData const& other) {
    if (this == &other) {
        return *this;
    }
    this->data       = other.data;
    this->types      = other.types;
    this->tasks      = other.tasks;
    this->futures    = other.futures;
    this->anyFutures = other.anyFutures;
    this->valueSizes = other.valueSizes;
    nestedSlots.clear();
    if (other.hasNestedSpaces()) {
        sp_log("NodeData copy assignment dropped nested PathSpaces (not copyable)", "NodeData");
        dropNestedTypes();
    }
    return *this;
}

NodeData::NodeData(InputData const& inputData) {
    sp_log("NodeData::NodeData", "Function Called");
    this->serialize(inputData);
}

auto NodeData::serialize(const InputData& inputData) -> std::optional<Error> {
    sp_log("NodeData::serialize", "Function Called");
    sp_log("Serializing data of type: " + std::string(inputData.metadata.typeInfo->name()), "NodeData");
    if (inputData.metadata.dataCategory == DataCategory::UniquePtr) {
        auto* ptr = static_cast<std::unique_ptr<PathSpaceBase>*>(inputData.obj);
        if (!ptr) {
            return Error{Error::Code::InvalidType, "UniquePtr payload missing backing pointer"};
        }
        if (auto err = validateNested(*ptr)) {
            return err;
        }
        this->appendNested(std::move(*ptr));
    } else if (inputData.task) {
        // Store task and aligned future handle
        this->tasks.push_back(std::move(inputData.task));
        this->futures.push_back(Future::FromShared(this->tasks.back()));
        // If a type-erased future is provided (typed task path), align it as well
        if (inputData.anyFuture.valid()) {
            this->anyFutures.push_back(inputData.anyFuture);
        }

        bool const isImmediateExecution = (*this->tasks.rbegin())->category() == ExecutionCategory::Immediate;
        if (isImmediateExecution) {
            sp_log("Immediate execution requested; attempting submission", "NodeData");
            // Require an injected executor for immediate execution
            if (inputData.executor) {
                if (auto const ret = inputData.executor->submit(this->tasks.back())) {
                    sp_log("Immediate submission refused by executor", "NodeData");
                    return ret;
                }
                sp_log("Immediate submission accepted by executor", "NodeData");
            } else {
                sp_log("Immediate submission failed: no executor provided in InputData", "NodeData");
                return Error{Error::Code::UnknownError, "No executor available for immediate task submission"};
            }
        }
    } else {
        if (!inputData.metadata.serialize)
            return Error{Error::Code::SerializationFunctionMissing, "Serialization function is missing."};
        size_t oldSize = data.size();
        inputData.metadata.serialize(inputData.obj, data);
        auto appended = data.size() - oldSize;
        valueSizes.push_back(appended);
        sp_log("Buffer size before: " + std::to_string(oldSize) + ", after: " + std::to_string(data.size()), "NodeData");
    }

    pushType(inputData.metadata);
    return std::nullopt;
}

auto NodeData::deserialize(void* obj, const InputMetadata& inputMetadata) const -> std::optional<Error> {
    sp_log("NodeData::deserialize", "Function Called");
    return const_cast<NodeData*>(this)->deserializeImpl(obj, inputMetadata, false);
}

auto NodeData::deserializePop(void* obj, const InputMetadata& inputMetadata) -> std::optional<Error> {
    sp_log("NodeData::deserializePop", "Function Called");
    return this->deserializeImpl(obj, inputMetadata, true);
}

auto NodeData::popFrontSerialized(NodeData& destination) -> std::optional<Error> {
    sp_log("NodeData::popFrontSerialized", "Function Called");
    if (this->types.empty()) {
        return Error{Error::Code::NoObjectFound, "No data available for serialization"};
    }
    if (this->types.front().category == DataCategory::UniquePtr) {
        return Error{Error::Code::NotSupported, "Nested PathSpaces cannot be serialized"};
    }
    if (this->types.front().category == DataCategory::Execution) {
        return Error{Error::Code::NotSupported, "Execution payloads cannot be serialized"};
    }
    if (this->valueSizes.empty()) {
        return Error{Error::Code::MalformedInput,
                     "Serialized payload missing value length metadata"};
    }

    auto length = this->valueSizes.front();
    if (length > this->data.size()) {
        return Error{Error::Code::MalformedInput, "Serialized payload exceeds buffer"};
    }

    NodeData payload;
    if (length > 0) {
        payload.data.append(this->data.data(), length);
    }
    payload.valueSizes.push_back(length);

    ElementType entry = this->types.front();
    entry.elements    = 1;
    payload.types.push_back(entry);

    this->data.advance(length);
    this->valueSizes.pop_front();
    popType();

    destination = std::move(payload);
    return std::nullopt;
}

auto NodeData::deserializeImpl(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    sp_log("NodeData::deserializeImpl", "Function Called");

    // Allow non-destructive reads to skip past nested PathSpaces that sit in front of
    // regular payloads. We keep the authoritative queue ordering for pops/takes.
    if (!doPop && !this->types.empty()
        && this->types.front().category == DataCategory::UniquePtr
        && inputMetadata.dataCategory != DataCategory::UniquePtr) {
        NodeData copy{*this}; // drops nested type metadata
        return copy.deserializeImpl(obj, inputMetadata, false);
    }

    // If the front of the queue is a nested slot that has no restored payload (e.g. a
    // snapshot placeholder), allow accesses to skip it before validating type compatibility.
    if (inputMetadata.dataCategory != DataCategory::UniquePtr) {
        while (!this->types.empty()
               && this->types.front().category == DataCategory::UniquePtr
               && this->nestedAt(0) == nullptr) {
            this->takeNestedAt(0);
            if (this->types.empty()) {
                return Error{Error::Code::NoObjectFound, "No data available for deserialization"};
            }
        }
    }

    if (auto validationResult = validateInputs(inputMetadata))
        return validationResult;

    // Defensive re-check: types may have changed between validation and use
    if (this->types.empty()) {
        sp_log("NodeData::deserializeImpl - no types available after validation (possible concurrent pop)", "NodeData");
        return Error{Error::Code::NoObjectFound, "No data available for deserialization"};
    }

    // Route based on the current front category
    if (this->types.front().category == DataCategory::Execution) {
        if (this->tasks.empty()) {
            sp_log("NodeData::deserializeImpl - execution category but no tasks present", "NodeData");
            return Error{Error::Code::NoObjectFound, "No task available"};
        }
        return this->deserializeExecution(obj, inputMetadata, doPop);
    } else if (this->types.front().category == DataCategory::UniquePtr) {
        return Error{Error::Code::NotSupported, "Nested PathSpaces must be accessed via out/take helpers"};
    } else {
        return this->deserializeData(obj, inputMetadata, doPop);
    }
}

auto NodeData::validateInputs(const InputMetadata& inputMetadata) -> std::optional<Error> {
    sp_log("NodeData::validateInputs", "Function Called");

    if (this->types.empty()) {
        sp_log("NodeData::validateInputs - queue is empty (no types present)", "NodeData");
        return Error{Error::Code::NoObjectFound, "No data available for deserialization"};
    }

    if (!this->types.empty() && this->types.front().typeInfo != inputMetadata.typeInfo) {
        auto have = this->types.front().typeInfo ? this->types.front().typeInfo->name() : "nullptr";
        auto want = inputMetadata.typeInfo ? inputMetadata.typeInfo->name() : "nullptr";
        sp_log(std::string("NodeData::validateInputs - type mismatch: have=") + have + " want=" + want, "NodeData");
        return Error{Error::Code::InvalidType, "Type mismatch during deserialization"};
    }

    return std::nullopt;
}

auto NodeData::deserializeExecution(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    sp_log("NodeData::deserializeExecution entry", "NodeData");
    sp_log(" - tasks.size()=" + std::to_string(this->tasks.size())
           + " futures.size()=" + std::to_string(this->futures.size()), "NodeData");
    if (!this->types.empty()) {
        auto have = this->types.front().typeInfo ? this->types.front().typeInfo->name() : "nullptr";
        sp_log(std::string(" - front type category=")
                   + std::to_string(static_cast<int>(this->types.front().category))
                   + " type=" + have,
               "NodeData");
    } else {
        sp_log(" - types queue is empty", "NodeData");
    }

    if (this->tasks.empty())
        return Error{Error::Code::NoObjectFound, "No task available"};

    // Use the aligned future to handle readiness non-blockingly
    auto const& task = this->tasks.front();
    sp_log(std::string(" - task state: started=") + (task->hasStarted() ? "true" : "false")
           + " completed=" + (task->isCompleted() ? "true" : "false")
           + " category=" + std::to_string(static_cast<int>(task->category())),
           "NodeData");

    if (!task->hasStarted()) {
        ExecutionCategory const taskExecutionCategory = task->category();
        bool const              isLazyExecution       = taskExecutionCategory == ExecutionCategory::Lazy;

        if (isLazyExecution) {
            // Require a preferred executor for lazy submission
            if (task->executor) {
                if (auto ret = task->executor->submit(task); ret) {
                    sp_log("Lazy submission refused by executor", "NodeData");
                    return ret;
                }
                sp_log("Lazy submission accepted by executor", "NodeData");
                // Removed busy-wait for lazy tasks; rely on external wait/notify to observe readiness.
            } else {
                sp_log("Lazy submission failed: task has no preferred executor", "NodeData");
                return Error{Error::Code::UnknownError, "No executor available for lazy task submission"};
            }
        }
    }

    // If we have a future and the task is ready, copy the result. Otherwise report not completed.
    if (!this->futures.empty() && this->futures.front().ready()) {
        sp_log("Future indicates task is ready; attempting to copy result", "NodeData");
        if (auto locked = this->futures.front().weak_task().lock()) {
            locked->resultCopy(obj);
            sp_log("Result copied from completed task", "NodeData");
            if (doPop) {
                sp_log("Pop requested; removing front task and aligned future", "NodeData");
                this->tasks.pop_front();
                this->futures.pop_front();
                popType();
            }
            return std::nullopt;
        }
        // Task expired unexpectedly
        sp_log("Future ready but task expired before result copy", "NodeData");
        return Error{Error::Code::UnknownError, "Task expired before result could be copied"};
    }

    if (task->isCompleted()) {
        sp_log("Task reports completed; copying result", "NodeData");
        task->resultCopy(obj);
        // Only pop on explicit extract (doPop == true)
        if (doPop) {
            this->tasks.pop_front();
            if (!this->futures.empty())
                this->futures.pop_front();
            popType();
        }
        return std::nullopt;
    }

    // Removed busy-wait for immediate tasks; rely on Future readiness and PathSpace::out blocking loop.

    sp_log("Task not yet completed; returning non-ready status to caller", "NodeData");
    return Error{Error::Code::UnknownError, "Task is not completed"};
}

auto NodeData::deserializeData(void* obj, const InputMetadata& inputMetadata, bool doPop) -> std::optional<Error> {
    sp_log("NodeData::deserializeData", "Function Called");
    sp_log("Deserializing data of type: " + std::string(inputMetadata.typeInfo->name()), "NodeData");
    sp_log("Current buffer size: " + std::to_string(data.size()), "NodeData");

    if (doPop) {
        if (!inputMetadata.deserializePop)
            return Error{Error::Code::UnserializableType, "No pop deserialization function provided"};
        inputMetadata.deserializePop(obj, data);
        if (!valueSizes.empty()) {
            valueSizes.pop_front();
        }
        sp_log("After pop, buffer size: " + std::to_string(data.size()), "NodeData");
        popType();
    } else {
        if (!inputMetadata.deserialize)
            return Error{Error::Code::UnserializableType, "No deserialization function provided"};
        inputMetadata.deserialize(obj, data);
    }
    return std::nullopt;
}

auto NodeData::empty() const -> bool {
    sp_log("NodeData::empty", "Function Called");
    return this->types.empty();
}

auto NodeData::pushType(InputMetadata const& meta) -> void {
    sp_log("NodeData::pushType", "Function Called");
    if (!types.empty()) {
        // Treat both type and dataCategory as part of the grouping key to avoid
        // merging Executions with serialized Data of the same type.
        if (types.back().typeInfo == meta.typeInfo && types.back().category == meta.dataCategory)
            types.back().elements++;
        else
            types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
    } else {
        types.emplace_back(meta.typeInfo, 1, meta.dataCategory);
    }
}

auto NodeData::popType() -> void {
    sp_log("NodeData::popType", "Function Called");
    if (this->types.empty())
        return;

    if (this->types.front().category == DataCategory::UniquePtr) {
        // Discard the front nested space in lockstep with the type queue.
        (void)this->takeNestedAt(0);
        return;
    }

    if (--this->types.front().elements == 0) {
        this->types.erase(this->types.begin());
    }
}

void NodeData::dropNestedTypes() {
    types.erase(std::remove_if(types.begin(),
                               types.end(),
                               [](ElementType const& t) {
                                   return t.category == DataCategory::UniquePtr;
                               }),
                types.end());
}

auto NodeData::append(NodeData const& other) -> std::optional<Error> {
    sp_log("NodeData::append", "Function Called");
    if (other.hasExecutionPayload()) {
        return Error{Error::Code::NotSupported,
                     "Execution payloads cannot be serialized across mounts"};
    }
    if (other.hasNestedSpaces()) {
        return Error{Error::Code::NotSupported,
                     "Nested PathSpaces cannot be serialized across mounts"};
    }

    auto sourceRaw   = other.rawBuffer();
    auto frontOffset = other.rawBufferFrontOffset();
    if (frontOffset > sourceRaw.size()) {
        return Error{Error::Code::MalformedInput, "Invalid serialized buffer"};
    }
    auto payload = sourceRaw.subspan(frontOffset);
    if (!payload.empty()) {
        data.append(payload.data(), payload.size());
    }

    for (auto const& type : other.types) {
        if (!types.empty() && types.back().typeInfo == type.typeInfo
            && types.back().category == type.category) {
            types.back().elements += type.elements;
        } else {
            types.push_back(type);
        }
    }

    if (!other.valueSizes.empty()) {
        valueSizes.insert(valueSizes.end(), other.valueSizes.begin(), other.valueSizes.end());
    }

    return std::nullopt;
}

auto NodeData::valueCount() const -> std::size_t {
    std::size_t count = 0;
    for (auto const& type : types) {
        if (type.category != DataCategory::Execution) {
            count += type.elements;
        }
    }
    return count;
}

auto NodeData::peekAnyFuture() const -> std::optional<FutureAny> {
    if (this->types.empty() || this->types.front().category != DataCategory::Execution)
        return std::nullopt;
    if (this->anyFutures.empty())
        return std::nullopt;
    return this->anyFutures.front();
}

auto NodeData::peekFuture() const -> std::optional<Future> {
    if (this->types.empty() || this->types.front().category != DataCategory::Execution)
        return std::nullopt;
   if (this->futures.empty())
       return std::nullopt;
   return this->futures.front();
}

auto NodeData::hasNestedSpaces() const noexcept -> bool {
    return this->nestedCount() > 0;
}

auto NodeData::nestedCount() const noexcept -> std::size_t {
    return nestedSlots.size();
}

auto NodeData::nestedAt(std::size_t index) const -> PathSpaceBase* {
    if (index >= nestedSlots.size())
        return nullptr;
    auto const& slot = nestedSlots[index];
    if (!slot)
        return nullptr;
    return slot->ptr.get();
}

auto NodeData::takeNestedAt(std::size_t index) -> std::unique_ptr<PathSpaceBase> {
    if (!hasNestedSpaces())
        return {};

    if (index >= nestedSlots.size())
        return {};

    auto slot = nestedSlots[index];
    if (!slot || !slot->ptr) {
        nestedSlots.erase(nestedSlots.begin() + static_cast<std::ptrdiff_t>(index));
        removeNestedTypeAt(index);
        return {};
    }

    {
        std::unique_lock<std::mutex> lk(slot->m);
        if (slot->borrows.load() > 0 && g_borrowWaitHook) {
            g_borrowWaitHook();
        }
        slot->cv.wait(lk, [&]() { return slot->borrows.load() == 0; });
    }

    auto ptr = std::move(slot->ptr);
    nestedSlots.erase(nestedSlots.begin() + static_cast<std::ptrdiff_t>(index));

    removeNestedTypeAt(index);
    return ptr;
}

auto NodeData::appendNested(std::unique_ptr<PathSpaceBase> space) -> void {
    auto slot = std::make_shared<NestedSlot>();
    slot->ptr = std::move(space);
    slot->self = slot;
    nestedSlots.push_back(std::move(slot));
}

auto NodeData::emplaceNestedAt(std::size_t index, std::unique_ptr<PathSpaceBase>& space) -> std::optional<Error> {
    if (auto err = validateNested(space)) {
        return err;
    }
    if (index >= nestedSlots.size()) {
        return Error{Error::Code::NoSuchPath, "Nested slot out of range"};
    }
    auto& slot = nestedSlots[index];
    if (!slot) {
        slot = std::make_shared<NestedSlot>();
        slot->self = slot;
    }
    if (slot->ptr != nullptr) {
        return Error{Error::Code::InvalidType, "Nested slot already occupied"};
    }
    slot->ptr = std::move(space);
    return std::nullopt;
}

auto NodeData::borrowNestedShared(std::size_t index) const -> std::shared_ptr<PathSpaceBase> {
    if (index >= nestedSlots.size()) {
        return {};
    }
    auto slot = nestedSlots[index];
    if (!slot || !slot->ptr) {
        return {};
    }
    slot->borrows.fetch_add(1);
    std::weak_ptr<NestedSlot> weakSlot = slot;
    auto control = slot; // keep slot alive for the holder lifetime
    std::shared_ptr<PathSpaceBase> holder(slot->ptr.get(), [weakSlot, control](PathSpaceBase*) {
        if (auto ctrl = weakSlot.lock()) {
            auto prev = ctrl->borrows.fetch_sub(1);
            if (prev == 1) {
                ctrl->cv.notify_all();
            }
        }
    });
    return holder;
}

void NodeData::retargetTasks(std::weak_ptr<NotificationSink> sink, Executor* exec) {
    for (auto const& task : tasks) {
        if (!task) {
            continue;
        }
        task->notifier = sink;
        task->setExecutor(exec);
    }
}

void NodeData::removeNestedTypeAt(std::size_t nestedIndex) {
    std::size_t remaining = nestedIndex;
    for (auto it = types.begin(); it != types.end(); ++it) {
        if (it->category != DataCategory::UniquePtr)
            continue;

        if (remaining >= it->elements) {
            remaining -= it->elements;
            continue;
        }

        // Target nested resides within this run
        if (--it->elements == 0) {
            types.erase(it);
        }
        return;
    }
}

std::optional<std::vector<std::byte>> NodeData::serializeSnapshot() const {
    sp_log("NodeData::serializeSnapshot", "Function Called");
    // Nested PathSpaces are copied separately by the caller (PathSpace::copyNodeRecursive).
    // Preserve their metadata to maintain queue ordering; the actual nested payloads are
    // re-attached after deserialization. Execution payloads cannot be serialized, so drop
    // them while retaining any adjacent values.
    std::deque<ElementType> typesForSnapshot = this->types;
    if (!this->tasks.empty() || !this->futures.empty() || !this->anyFutures.empty()) {
        typesForSnapshot.erase(std::remove_if(typesForSnapshot.begin(),
                                              typesForSnapshot.end(),
                                              [](ElementType const& t) {
                                                  return t.category == DataCategory::Execution;
                                              }),
                               typesForSnapshot.end());
    }

    if (typesForSnapshot.empty()) {
        sp_log("History payload has no serializable entries after filtering", "NodeData");
        return std::nullopt;
    }

    std::vector<std::byte> bytes;
    bytes.reserve(32 + this->data.rawSize());

    appendScalar<std::uint32_t>(bytes, HISTORY_PAYLOAD_VERSION);
    appendScalar<std::uint32_t>(bytes, static_cast<std::uint32_t>(typesForSnapshot.size()));

    for (auto const& type : typesForSnapshot) {
        auto ptrValue = reinterpret_cast<std::uintptr_t>(type.typeInfo);
        appendScalar<std::uintptr_t>(bytes, ptrValue);
        appendScalar<std::uint32_t>(bytes, type.elements);
        bytes.push_back(static_cast<std::byte>(type.category));
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
    }

    appendScalar<std::uint32_t>(bytes, static_cast<std::uint32_t>(this->valueSizes.size()));
    for (auto length : this->valueSizes) {
        appendScalar<std::uint32_t>(bytes, static_cast<std::uint32_t>(length));
    }

    appendScalar<std::uint32_t>(bytes, static_cast<std::uint32_t>(this->data.rawSize()));
    appendScalar<std::uint32_t>(bytes, static_cast<std::uint32_t>(this->data.virtualFront()));

    auto rawSpan = this->data.rawData();
    auto rawSize = rawSpan.size();
    auto oldSize = bytes.size();
    bytes.resize(oldSize + rawSize);
    std::memcpy(bytes.data() + oldSize, rawSpan.data(), rawSize);
    return bytes;
}

std::optional<NodeData> NodeData::deserializeSnapshot(std::span<const std::byte> bytes) {
    sp_log("NodeData::deserializeSnapshot", "Function Called");
    auto version = readScalar<std::uint32_t>(bytes);
    if (!version.has_value()) {
        sp_log("Missing history payload version", "NodeData");
        return std::nullopt;
    }
    if (*version == 0 || *version > HISTORY_PAYLOAD_VERSION) {
        sp_log("Unsupported history payload version", "NodeData");
        return std::nullopt;
    }
    bool const hasValueSizes = (*version) >= 2;

    auto countOpt = readScalar<std::uint32_t>(bytes);
    if (!countOpt.has_value())
        return std::nullopt;
    std::uint32_t count = *countOpt;

    std::deque<ElementType> restoredTypes;
    restoredTypes.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto typePtrOpt   = readScalar<std::uintptr_t>(bytes);
        auto elementsOpt  = readScalar<std::uint32_t>(bytes);
        if (!typePtrOpt.has_value() || !elementsOpt.has_value() || bytes.size() < 4)
            return std::nullopt;
        auto categoryByte = static_cast<std::uint8_t>(bytes[0]);
        bytes = bytes.subspan(4);

        ElementType element{};
        element.typeInfo = reinterpret_cast<std::type_info const*>(*typePtrOpt);
        element.elements = *elementsOpt;
        element.category = static_cast<DataCategory>(categoryByte);
        restoredTypes[i] = element;
    }

    std::vector<std::size_t> restoredValueSizes;
    if (hasValueSizes) {
        auto countOpt = readScalar<std::uint32_t>(bytes);
        if (!countOpt.has_value())
            return std::nullopt;
        restoredValueSizes.resize(*countOpt);
        for (std::uint32_t i = 0; i < *countOpt; ++i) {
            auto lengthOpt = readScalar<std::uint32_t>(bytes);
            if (!lengthOpt.has_value())
                return std::nullopt;
            restoredValueSizes[i] = *lengthOpt;
        }
    }

    auto rawSizeOpt = readScalar<std::uint32_t>(bytes);
    auto frontOpt   = readScalar<std::uint32_t>(bytes);
    if (!rawSizeOpt.has_value() || !frontOpt.has_value())
        return std::nullopt;
    std::uint32_t rawSize = *rawSizeOpt;
    std::uint32_t front   = *frontOpt;
    if (bytes.size() < rawSize || front > rawSize)
        return std::nullopt;

    std::vector<std::uint8_t> raw(rawSize);
    std::memcpy(raw.data(), bytes.data(), rawSize);
    bytes = bytes.subspan(rawSize);

    NodeData node;
    node.data.assignRaw(std::move(raw), front);
    node.types = std::move(restoredTypes);
    if (hasValueSizes) {
        node.valueSizes.assign(restoredValueSizes.begin(), restoredValueSizes.end());
    }
    // Recreate placeholder entries for nested payloads so queue ordering is preserved
    // even when the actual nested spaces are copied separately or unavailable.
    for (auto const& type : node.types) {
        if (type.category != DataCategory::UniquePtr)
            continue;
        for (std::size_t i = 0; i < type.elements; ++i) {
            auto slot = std::make_shared<NestedSlot>();
            slot->self = slot;
            node.nestedSlots.push_back(std::move(slot));
        }
    }
    return node;
}

void NodeDataTestHelper::setBorrowWaitHook(std::function<void()> hook) {
    g_borrowWaitHook = std::move(hook);
}

void NodeDataTestHelper::setNestedSerializeHook(std::function<std::optional<Error>()> hook) {
    g_nestedSerializeHook = std::move(hook);
}

auto NodeData::fromSerializedValue(InputMetadata const& metadata,
                                   std::span<const std::byte> bytes) -> Expected<NodeData> {
    if (metadata.typeInfo == nullptr) {
        return std::unexpected(Error{Error::Code::InvalidType, "Missing type metadata"});
    }
    NodeData node;
    if (!bytes.empty()) {
        auto raw = std::span<const std::byte>(bytes.data(), bytes.size());
        auto as_u8 = std::span<const std::uint8_t>(reinterpret_cast<std::uint8_t const*>(raw.data()),
                                                   raw.size());
        node.data.append(as_u8);
    }
    node.valueSizes.push_back(bytes.size());
    node.pushType(metadata);
    return node;
}

auto NodeData::frontSerializedValueBytes() const -> std::optional<std::span<const std::byte>> {
    if (valueSizes.empty()) {
        return std::nullopt;
    }
    auto const length = valueSizes.front();
    auto const raw    = std::as_bytes(data.rawData());
    auto const front  = data.virtualFront();
    if (front + length > raw.size()) {
        return std::nullopt;
    }
    return raw.subspan(front, length);
}

auto NodeData::hasExecutionPayload() const noexcept -> bool {
    return !tasks.empty() || !futures.empty() || !anyFutures.empty();
}

} // namespace SP
