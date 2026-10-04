#include "anomaly/ue5_nte_adapter.hpp"
#include "anomaly/nte_damage_capture.hpp"
#include "anomaly/nte_monster_names.hpp"
#include "anomaly/nte_ui_buttons.hpp"
#include "anomaly/ue5_ftext.hpp"
#include "anomaly/ue5_streaming_source_override.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

ThreadLocalScalar<const void*> g_active_tick_callback_state;
ThreadLocalScalar<const void*> g_active_ahud_callback_state;
ThreadLocalScalar<const void*> g_active_ahud_subscription_state;

class ActiveTickCallbackScope final {
public:
    explicit ActiveTickCallbackScope(const void* state) noexcept
        : previous_(g_active_tick_callback_state.Get()) {
        g_active_tick_callback_state.Set(state);
    }

    ActiveTickCallbackScope(const ActiveTickCallbackScope&) = delete;
    ActiveTickCallbackScope& operator=(const ActiveTickCallbackScope&) = delete;

    ~ActiveTickCallbackScope() {
        g_active_tick_callback_state.Set(previous_);
    }

private:
    const void* previous_{};
};

class ActiveAhudCallbackScope final {
public:
    ActiveAhudCallbackScope(const void* state, const void* subscription) noexcept
        : previous_state_(g_active_ahud_callback_state.Get()),
          previous_subscription_(g_active_ahud_subscription_state.Get()) {
        g_active_ahud_callback_state.Set(state);
        g_active_ahud_subscription_state.Set(subscription);
    }

    ActiveAhudCallbackScope(const ActiveAhudCallbackScope&) = delete;
    ActiveAhudCallbackScope& operator=(const ActiveAhudCallbackScope&) = delete;

    ~ActiveAhudCallbackScope() {
        g_active_ahud_subscription_state.Set(previous_subscription_);
        g_active_ahud_callback_state.Set(previous_state_);
    }

private:
    const void* previous_state_{};
    const void* previous_subscription_{};
};

class AddressWaitApi final {
public:
    using WaitOnAddressFunction = BOOL(WINAPI*)(
        volatile VOID*, PVOID, SIZE_T, DWORD);
    using WakeByAddressAllFunction = VOID(WINAPI*)(PVOID);

    [[nodiscard]] static const AddressWaitApi& Instance() noexcept {
        static const AddressWaitApi api;
        return api;
    }

    [[nodiscard]] WaitOnAddressFunction Wait() const noexcept {
        return wait_;
    }

    [[nodiscard]] WakeByAddressAllFunction WakeAll() const noexcept {
        return wake_all_;
    }

private:
    AddressWaitApi() noexcept {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        if (module == nullptr) module = GetModuleHandleW(L"kernel32.dll");
        if (module == nullptr) return;
        wait_ = reinterpret_cast<WaitOnAddressFunction>(
            GetProcAddress(module, "WaitOnAddress"));
        wake_all_ = reinterpret_cast<WakeByAddressAllFunction>(
            GetProcAddress(module, "WakeByAddressAll"));
    }

    WaitOnAddressFunction wait_{};
    WakeByAddressAllFunction wake_all_{};
};

class AdmissionGate final {
public:
    [[nodiscard]] bool TryEnter() noexcept {
        std::uint64_t observed = Load();
        for (;;) {
            if ((observed & kClosedBit) != 0 ||
                (observed & kActiveMask) == kActiveMask) {
                return false;
            }
            const std::uint64_t desired = observed + 1U;
            const auto previous = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &value_, static_cast<LONG64>(desired), static_cast<LONG64>(observed)));
            if (previous == observed) return true;
            observed = previous;
        }
    }

    void Close() noexcept {
        static_cast<void>(InterlockedOr64(&value_, static_cast<LONG64>(kClosedBit)));
        WakeAll(&value_);
    }

    void Leave() noexcept {
        const auto remaining =
            static_cast<std::uint64_t>(InterlockedDecrement64(&value_)) & kActiveMask;
        if (remaining == 0) {
            WakeAll(&value_);
        }
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return (Load() & kActiveMask) == 0;
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            const std::uint64_t observed = Load();
            if ((observed & kActiveMask) == 0) return true;
            if (deadline != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }

            const DWORD timeout = RemainingMilliseconds(deadline);
            LONG64 expected = static_cast<LONG64>(observed);
            Wait(&value_, expected, timeout);
        }
    }

private:
    static void Wait(volatile LONG64* address, LONG64 expected, DWORD timeout) noexcept {
        if (const auto wait = AddressWaitApi::Instance().Wait()) {
            static_cast<void>(wait(address, &expected, sizeof(expected), timeout));
            return;
        }
        if (timeout != 0) {
            Sleep(timeout == INFINITE
                ? static_cast<DWORD>(1)
                : (std::min)(timeout, static_cast<DWORD>(1)));
        }
    }

    static void WakeAll(volatile LONG64* address) noexcept {
        if (const auto wake = AddressWaitApi::Instance().WakeAll()) {
            wake(const_cast<void*>(static_cast<const volatile void*>(address)));
        }
    }

    [[nodiscard]] std::uint64_t Load() const noexcept {
        return static_cast<std::uint64_t>(InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&value_), 0, 0));
    }

    [[nodiscard]] static DWORD RemainingMilliseconds(
        std::chrono::steady_clock::time_point deadline) noexcept {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            return INFINITE;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(milliseconds) <
            remaining) {
            ++milliseconds;
        }
        if (milliseconds <= std::chrono::milliseconds::zero()) return 0;
        return static_cast<DWORD>((std::min)(
            milliseconds.count(), static_cast<std::int64_t>(INFINITE - 1U)));
    }

    static constexpr std::uint64_t kClosedBit = std::uint64_t{1} << 63U;
    static constexpr std::uint64_t kActiveMask = ~kClosedBit;
    volatile LONG64 value_{};
};

class RetiredTickCallbackQueue final {
public:
    RetiredTickCallbackQueue() {
        std::thread([this] { Run(); }).detach();
    }

    void Retire(const Ue5NteAdapter::TickCallback* callback) noexcept {
        if (callback == nullptr) return;
        try {
            {
                std::scoped_lock lock(mutex_);
                callbacks_.push_back(callback);
            }
            ready_.notify_one();
        } catch (...) {
            // A callback target can own arbitrary code. Preserve it rather
            // than running that destructor on a bounded lifecycle path.
        }
    }

private:
    void Run() noexcept {
        for (;;) {
            const Ue5NteAdapter::TickCallback* callback{};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !callbacks_.empty(); });
                callback = callbacks_.front();
                callbacks_.pop_front();
            }
            delete callback;
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<const Ue5NteAdapter::TickCallback*> callbacks_;
};

RetiredTickCallbackQueue* ProcessRetiredTickCallbacks() noexcept {
    // This queue intentionally outlives Runtime teardown: a user callback
    // destructor may block or reenter after the bounded Adapter stop path.
    static auto* queue = []() noexcept -> RetiredTickCallbackQueue* {
        try {
            return new RetiredTickCallbackQueue();
        } catch (...) {
            return nullptr;
        }
    }();
    return queue;
}

void RetireTickCallback(const Ue5NteAdapter::TickCallback* callback) noexcept {
    if (auto* queue = ProcessRetiredTickCallbacks()) {
        queue->Retire(callback);
    }
    // If the process queue cannot be initialized, intentionally retain the
    // callback object rather than destroying arbitrary code during Stop.
}

std::shared_ptr<const Ue5NteAdapter::TickCallback> MakeTickCallback(
    Ue5NteAdapter::TickCallback callback) {
    if (!callback) return {};
    return std::shared_ptr<const Ue5NteAdapter::TickCallback>(
        new Ue5NteAdapter::TickCallback(std::move(callback)), RetireTickCallback);
}

template <typename Mutex>
[[nodiscard]] bool LockUntil(
    std::unique_lock<Mutex>& lock,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        lock.lock();
        return true;
    }
    return lock.try_lock_until(deadline);
}

AnomalyStatusV1 Status(std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

std::uint32_t SnapshotFlags(
    bool partial,
    std::uint64_t sample_sequence,
    std::uint64_t current_sequence) noexcept {
    std::uint32_t flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
    if (sample_sequence < current_sequence) flags |= ANOMALY_NTE_SNAPSHOT_V1_STALE;
    if (partial) flags |= ANOMALY_NTE_SNAPSHOT_V1_PARTIAL;
    return flags;
}

AnomalyStatusV1 CopyString(
    std::string_view value,
    char* destination,
    std::size_t* inout_size) noexcept {
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const std::size_t required = value.size() + 1;
    if (destination == nullptr || *inout_size < required) {
        *inout_size = required;
        return destination == nullptr
            ? Status(ANOMALY_STATUS_V1_OK)
            : Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    *inout_size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

std::int64_t Layout(
    const BuildProfile& profile,
    std::string_view key,
    std::int64_t fallback = -1) noexcept {
    const auto found = profile.layout.find(key);
    return found == profile.layout.end() ? fallback : found->second;
}

template <typename T>
bool ReadValue(const SymbolMemory& memory, std::uintptr_t address, T& value) noexcept {
    return address != 0 && memory.Read(address, &value, sizeof(value));
}

bool AddAddress(std::uintptr_t base, std::int64_t offset, std::uintptr_t& result) noexcept {
    if (base == 0 || offset < 0 ||
        static_cast<std::uint64_t>(offset) >
            (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

[[nodiscard]] bool ReadableRange(
    const SymbolMemory& memory,
    const std::uintptr_t address,
    const std::size_t size) noexcept {
    if (address < 0x10000U || size == 0 ||
        size > (std::numeric_limits<std::uintptr_t>::max)() - address) {
        return false;
    }
    const auto region = memory.Query(address);
    if (!region || region->state != MEM_COMMIT ||
        (region->protection & PAGE_GUARD) != 0 ||
        (region->protection & 0xFFU) == PAGE_NOACCESS) {
        return false;
    }
    const auto region_end = region->base > (std::numeric_limits<std::uintptr_t>::max)() -
            region->size
        ? (std::numeric_limits<std::uintptr_t>::max)()
        : region->base + region->size;
    return address >= region->base && size <= region_end - address;
}

[[nodiscard]] bool InvokeProcessEventGuarded(
    const Ue5NteAdapter::ProcessEventInvoker& invoker,
    const std::uintptr_t receiver,
    const std::uintptr_t function,
    void* const parameters,
    const std::size_t parameter_size,
    std::uint32_t* const fault_code = nullptr) noexcept {
    // Code in the active process is above the 4 GiB boundary, so the function pointer keeps that
    // floor. Objects are not: the local player controller is allocated low (measured 0x377F2070,
    // about 931 MB on the current game build). Rejecting it made every pickup-service
    // TriggerInteract fail while BPCanTryInteract on the same actor - which passes the actor as
    // the receiver, and that one is above 4 GiB - succeeded, so every pickup-service item
    // (food, wallet, random items) was skipped. The receiver only needs a sanity floor; the SEH
    // wrapper below still catches a bad target.
    if (!invoker || receiver < 0x10000ULL || function < 0x100000000ULL) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        return invoker(receiver, function, parameters, parameter_size);
    } __except ((fault_code != nullptr ? *fault_code = GetExceptionCode() : 0),
                EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    try {
        return invoker(receiver, function, parameters, parameter_size);
    } catch (...) {
        return false;
    }
#endif
}

bool AddLayoutOffset(
    std::int64_t offset,
    std::int64_t additional,
    std::int64_t& result) noexcept {
    if (offset < 0 || additional < 0 ||
        offset > (std::numeric_limits<std::int64_t>::max)() - additional) {
        return false;
    }
    result = offset + additional;
    return true;
}

bool ReadPointerAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::int64_t offset,
    std::uintptr_t& result) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(memory, address, result) && result != 0;
}

bool ReadNullablePointerAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::int64_t offset,
    std::uintptr_t& result) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(memory, address, result);
}

bool AddUnsignedAddress(
    std::uintptr_t base,
    std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

struct ObjectRegistryState {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    std::uint32_t chunk_size{};
    std::uint32_t item_stride{};
    std::uint32_t object_offset{};
    std::uint32_t serial_offset{};
    std::uint64_t chunk_signature{};
};

bool ReadObjectChunk(
    const SymbolMemory& memory,
    const ObjectRegistryState& registry,
    std::uint32_t page,
    std::uintptr_t& chunk) noexcept {
    if (registry.items == 0 || page >= registry.num_chunks ||
        page > (std::numeric_limits<std::uint64_t>::max)() / sizeof(std::uintptr_t)) {
        return false;
    }
    std::uintptr_t entry{};
    return AddUnsignedAddress(
               registry.items,
               static_cast<std::uint64_t>(page) * sizeof(std::uintptr_t), entry) &&
        ReadValue(memory, entry, chunk) && chunk != 0 &&
        (chunk & (alignof(std::uintptr_t) - 1U)) == 0;
}

bool ReadObjectSlot(
    const SymbolMemory& memory,
    const ObjectRegistryState& registry,
    std::uint32_t index,
    std::uintptr_t& object,
    std::uint32_t& serial) noexcept {
    if (registry.items == 0 || registry.chunk_size == 0 || registry.item_stride == 0 ||
        index >= registry.count || registry.count > registry.max_count ||
        registry.num_chunks > registry.max_chunks) {
        return false;
    }
    const std::uint32_t page = index / registry.chunk_size;
    const std::uint32_t slot = index % registry.chunk_size;
    std::uintptr_t chunk{};
    if (!ReadObjectChunk(memory, registry, page, chunk) ||
        slot > (std::numeric_limits<std::uint64_t>::max)() / registry.item_stride) {
        return false;
    }
    std::uintptr_t item{};
    std::uintptr_t object_address{};
    std::uintptr_t serial_address{};
    return AddUnsignedAddress(
               chunk, static_cast<std::uint64_t>(slot) * registry.item_stride, item) &&
        AddUnsignedAddress(item, registry.object_offset, object_address) &&
        AddUnsignedAddress(item, registry.serial_offset, serial_address) &&
        ReadValue(memory, object_address, object) && ReadValue(memory, serial_address, serial);
}

bool LoadObjectRegistry(
    const BuildProfile& profile,
    const SymbolMemory& memory,
    std::uintptr_t address,
    ObjectRegistryState& registry) noexcept {
    const auto items_offset = Layout(profile, "objects.itemsOffset");
    const auto count_offset = Layout(profile, "objects.countOffset");
    const auto max_count_offset = Layout(profile, "objects.maxCountOffset");
    const auto max_chunks_offset = Layout(profile, "objects.maxChunksOffset");
    const auto num_chunks_offset = Layout(profile, "objects.numChunksOffset");
    const auto chunk_count_size = Layout(
        profile, "objects.chunkCountSize", sizeof(std::uint32_t));
    const auto chunk_size = Layout(profile, "objects.chunkSize");
    const auto item_stride = Layout(profile, "objects.itemStride");
    const auto object_offset = Layout(profile, "objects.objectOffset");
    const auto serial_offset = Layout(profile, "objects.serialOffset");
    constexpr std::int64_t kMaximumHeaderOffset = 4096;
    constexpr std::int64_t kMaximumObjects = 16LL * 1024LL * 1024LL;
    constexpr std::int64_t kMaximumChunks = 4096;
    if (items_offset < 0 || count_offset < 0 || max_count_offset < 0 ||
        max_chunks_offset < 0 || num_chunks_offset < 0 ||
        items_offset > kMaximumHeaderOffset || count_offset > kMaximumHeaderOffset ||
        max_count_offset > kMaximumHeaderOffset ||
        max_chunks_offset > kMaximumHeaderOffset ||
        num_chunks_offset > kMaximumHeaderOffset ||
        (chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint16_t)) &&
         chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint32_t))) ||
        chunk_size <= 0 ||
        chunk_size > kMaximumObjects ||
        (chunk_size & (chunk_size - 1)) != 0 || item_stride <
            static_cast<std::int64_t>(sizeof(std::uintptr_t)) || item_stride > 4096 ||
        object_offset < 0 || object_offset > item_stride -
            static_cast<std::int64_t>(sizeof(std::uintptr_t)) || serial_offset < 0 ||
        serial_offset > item_stride - static_cast<std::int64_t>(sizeof(std::uint32_t))) {
        return false;
    }

    ObjectRegistryState next;
    next.chunk_size = static_cast<std::uint32_t>(chunk_size);
    next.item_stride = static_cast<std::uint32_t>(item_stride);
    next.object_offset = static_cast<std::uint32_t>(object_offset);
    next.serial_offset = static_cast<std::uint32_t>(serial_offset);
    std::uintptr_t count_address{};
    std::uintptr_t max_count_address{};
    std::uintptr_t max_chunks_address{};
    std::uintptr_t num_chunks_address{};
    const auto read_chunk_count = [&](std::uintptr_t field, std::uint32_t& value) {
        if (chunk_count_size == static_cast<std::int64_t>(sizeof(std::uint16_t))) {
            std::uint16_t packed{};
            if (!ReadValue(memory, field, packed)) return false;
            value = packed;
            return true;
        }
        return ReadValue(memory, field, value);
    };
    if (!ReadPointerAt(memory, address, items_offset, next.items) ||
        !AddAddress(address, count_offset, count_address) ||
        !AddAddress(address, max_count_offset, max_count_address) ||
        !AddAddress(address, max_chunks_offset, max_chunks_address) ||
        !AddAddress(address, num_chunks_offset, num_chunks_address) ||
        !ReadValue(memory, count_address, next.count) ||
        !ReadValue(memory, max_count_address, next.max_count) ||
        !read_chunk_count(max_chunks_address, next.max_chunks) ||
        !read_chunk_count(num_chunks_address, next.num_chunks) || next.max_count == 0 ||
        next.max_count > kMaximumObjects || next.count > next.max_count ||
        next.max_chunks == 0 || next.max_chunks > kMaximumChunks ||
        next.num_chunks > next.max_chunks ||
        static_cast<std::uint64_t>(next.max_count) >
            static_cast<std::uint64_t>(next.max_chunks) * next.chunk_size) {
        return false;
    }
    const std::uint64_t required_chunks = next.count == 0 ? 0 :
        (static_cast<std::uint64_t>(next.count) + next.chunk_size - 1U) / next.chunk_size;
    if (required_chunks > next.num_chunks) return false;

    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    next.chunk_signature = kFnvOffset;
    for (std::uint32_t page = 0; page < required_chunks; ++page) {
        std::uintptr_t chunk{};
        if (!ReadObjectChunk(memory, next, page, chunk)) return false;
        next.chunk_signature ^= static_cast<std::uint64_t>(chunk);
        next.chunk_signature *= kFnvPrime;
    }
    if (next.count != 0) {
        std::uintptr_t ignored_object{};
        std::uint32_t ignored_serial{};
        if (!ReadObjectSlot(memory, next, 0, ignored_object, ignored_serial) ||
            !ReadObjectSlot(
                memory, next, next.count - 1U, ignored_object, ignored_serial)) {
            return false;
        }
    }
    registry = next;
    return true;
}

std::uint64_t EncodeObjectHandle(std::uint32_t index, std::uint32_t serial) noexcept {
    return (static_cast<std::uint64_t>(serial) << 32U) |
        (static_cast<std::uint64_t>(index) + 1U);
}

bool DecodeExactObjectPath(
    const AnomalyStringViewV1 path,
    std::wstring& decoded) {
    constexpr std::size_t kMaximumPathBytes = 16U * 1024U;
    decoded.clear();
    if (path.data == nullptr || path.size == 0 || path.size > kMaximumPathBytes ||
        path.size > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        std::memchr(path.data, '\0', path.size) != nullptr) {
        return false;
    }
    const int source_size = static_cast<int>(path.size);
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data, source_size, nullptr, 0);
    if (count <= 0) return false;
    decoded.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               path.data,
               source_size,
               decoded.data(),
               count) == count;
}

}  // namespace

struct Ue5NteAdapter::State {
    struct SemanticServiceEndpoint;
    struct CallbackEndpoint;
    struct AhudServiceEndpoint;
    struct ProcessEventServiceEndpoint;

    BuildFingerprint fingerprint;
    BuildProfile profile;
    ProfileResolutionSnapshot resolution;
    std::shared_ptr<const SymbolMemory> memory;
    FeatureLayoutValidatorRegistry feature_layout_validators;
    AdapterServiceRegistry* services{};
    Ue5NteAdapter::ProcessEventInvoker process_event_invoker;
    Ue5NteAdapter::ObjectLookup object_lookup;
    mutable std::timed_mutex mutex;
    std::timed_mutex lifecycle_mutex;
    mutable std::recursive_timed_mutex publication_mutex;
    std::atomic<std::shared_ptr<const TickCallback>> configured_tick_callback;
    std::atomic_bool started{};
    std::atomic_bool stopping{};
    std::atomic<std::uint64_t> lifecycle_epoch{};
    std::atomic<DWORD> game_thread_id{};
    std::atomic<std::uint64_t> tick_sequence{};
    std::atomic<std::uint64_t> rejected_thread_ticks{};
    NteSnapshotSamplingOptions sampling;
    std::atomic_bool player_demand{};
    std::atomic_bool entity_demand{};
    std::atomic_bool navigation_demand{};
    std::atomic_bool pickup_demand{};
    static constexpr std::size_t kSessionEventCapacity = 64;
    static constexpr std::uint32_t kEntityPageCapacity = 256;
    static constexpr std::size_t kMaximumPickupParameterSize = 4096;
    static constexpr std::size_t kMaximumPickupChoices = 128;
    struct SessionEvent {
        std::uint32_t kind{};
        std::uint64_t sequence{};
        std::uint64_t tick_sequence{};
        AnomalyGenerationHandleV1 previous_world{};
        AnomalyGenerationHandleV1 world{};
    };
    std::array<SessionEvent, kSessionEventCapacity> session_events{};
    std::size_t session_event_start{};
    std::size_t session_event_count{};
    std::uint64_t session_event_sequence{};
    std::uintptr_t world_pointer{};
    std::uint64_t world_generation{};
    std::uint64_t world_change_sequence{};
    std::uint32_t world_name_id{};
    bool world_name_layout_available{};
    bool world_name_readable{};
    ObjectRegistryState object_registry{};
    std::uint64_t object_generation{};
    struct ReflectedBoolParameter {
        std::uint16_t byte_offset{};
        std::uint8_t field_mask{};
        std::uint8_t byte_mask{};
    };
    enum class NteFunctionKind : std::size_t {
        GetAbilitySystemComponent,
        GetMainCharacterId,
        GetNpcMainCharacterId,
        GetHp,
        GetHpMax,
        GetIsDead,
        GetAttackTarget,
        GetShieldHealth,
        GetActiveEffectTimeRemainingAndDuration,
        ActivateAbilityByClass,
        ShowDamageFloaties,
        MulticastShowMonsterDamageInfo,
        ClientShowPlayerDamageInfo,
        SetDamageInfo,
        OnActiveGameplayEffectAdded,
        OnAnyGameplayEffectRemoved,
        AddBuffControl,
        RemoveFromBuffControl,
        AddBufferManagerBuffControl,
        AddHeadUpBattleMarkBuffControl,
        RemoveHeadUpBattleMarkBuffControl,
        AddMonsterBufferControl,
        GetMonsterStaticData,
        CurrentDamageIsCrit,
        Count,
    };
    static constexpr std::size_t kNteFunctionCount =
        static_cast<std::size_t>(NteFunctionKind::Count);
    static constexpr std::size_t kMaximumNteFunctionParameters = 6;
    struct NteFunctionParameterSpec {
        std::string_view name;
        std::string_view type;
        std::int32_t element_size{};
        bool return_value{};
    };
    struct NteFunctionSpec {
        NteFunctionKind kind{};
        std::string_view name;
        std::string_view outer;
        std::uint16_t parms_size{};
        std::span<const NteFunctionParameterSpec> parameters;
    };
    struct NteFunctionBinding {
        std::uintptr_t function{};
        std::uintptr_t outer_class{};
        std::uintptr_t meta_class{};
        std::uint16_t parms_size{};
        std::array<std::uint16_t, kMaximumNteFunctionParameters> offsets{};
        std::array<ReflectedBoolParameter, kMaximumNteFunctionParameters> bool_parameters{};
    };
    struct CombatSkillDiscovery {
        std::array<std::optional<NteFunctionBinding>, kNteFunctionCount> functions{};
        std::uintptr_t ability_system_class{};
        std::uintptr_t gameplay_ability_class{};
        std::uintptr_t ability_spawn_actor_class{};
        std::uint64_t object_generation{};
        bool direct_lookup_succeeded{};
        std::uint64_t direct_lookup_next_sequence{};
        std::uint32_t direct_lookup_retry_interval{300};
        bool damage_event_layout_valid{};
        bool skill_layout_valid{};
        bool cooldown_layout_valid{};
        bool combat_event_bindings_attempted{};
    } combat_skill_discovery;

    struct VehicleFunctionBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t parameter_offset{};
        std::uint16_t return_offset{};
        bool has_return{};
    };
    struct VehicleBindings {
        VehicleFunctionBinding current_vehicle{};
        VehicleFunctionBinding speed_kmh{};
        VehicleFunctionBinding set_top_speed_ratio{};
        VehicleFunctionBinding summon_vehicle{};
        VehicleFunctionBinding set_wheel_friction{};
        std::uint64_t object_generation{};
        bool attempted{};
    } vehicle_bindings;
    struct VehicleCatalogEntry {
        std::string id;
        std::uintptr_t row{};
        std::uintptr_t table{};
    };
    std::vector<VehicleCatalogEntry> vehicle_catalog;
    std::string selected_vehicle_id;
    std::uint64_t vehicle_catalog_generation{};
    std::uint64_t vehicle_catalog_sequence{};
    std::uintptr_t current_vehicle_object{};
    // Host-only state for Tokky's validated SetMaxEngineTorque mutation path.
    std::uintptr_t vehicle_base_movement_component{};
    float vehicle_base_engine_torque{};
    bool vehicle_base_engine_torque_valid{};
    float vehicle_top_speed_ratio{1.0F};
    bool vehicle_wheel_friction_enabled{true};
    float vehicle_speed_kmh{};
    bool vehicle_valid{};

    static constexpr std::size_t kDamageEventCapacity = 512;
    static constexpr std::uint64_t kDamageSourceBase = 0x8000000000000000ULL;
    struct DamageRecord {
        AnomalyNteDamageEventV1 event{};
    };
    std::array<DamageRecord, kDamageEventCapacity> damage_events{};
    std::size_t damage_event_start{};
    std::size_t damage_event_count{};
    std::uint64_t damage_event_sequence{};
    std::uint64_t damage_world_sequence_base{};
    std::uint64_t damage_dropped_count{};
    std::atomic<std::uint64_t> damage_native_call_count{};
    mutable std::atomic<std::uint64_t> reflection_fault_count{};
    mutable std::atomic<std::uintptr_t> last_reflection_fault_function{};
    mutable std::atomic<std::uint32_t> last_reflection_fault_code{};
    std::uint64_t damage_captured_event_count{};
    std::atomic<std::uint64_t> damage_capture_drop_count{};
    std::uint64_t damage_attacker_resolution_failure_count{};
    std::uint64_t damage_victim_resolution_failure_count{};
    std::uint64_t damage_source_resolution_failure_count{};
    std::uint64_t damage_next_source_id{kDamageSourceBase};
    std::unordered_map<std::uint64_t, std::uint64_t> damage_source_object_ids;
    std::unordered_map<std::uint64_t, std::uintptr_t> damage_source_objects;
    std::unordered_map<std::uint64_t, std::string> damage_source_names;
    struct DamageSourceAbilityMapping {
        std::uintptr_t class_pointer{};
        AnomalyGenerationHandleV1 class_handle{};
        bool name_pending{};
        std::uint8_t name_attempts{};
        std::uint64_t next_name_sequence{};
    };
    std::unordered_map<std::uint64_t, DamageSourceAbilityMapping>
        damage_source_ability_classes;
    struct PendingDamageSourceMapping {
        std::uint64_t source_id{};
        std::uintptr_t saved_skill_cdo{};
        std::int32_t active_spec_handle{};
        bool attacker_is_player{};
        std::uint8_t attempts{};
    };
    std::deque<PendingDamageSourceMapping> pending_damage_source_mappings;
    std::unordered_set<std::uint64_t> observed_damage_source_mappings;
    std::uint64_t saved_trigger_skill_mapping_count{};
    std::uint64_t trigger_ability_handle_mapping_count{};
    std::uint64_t damage_source_mapping_failure_count{};
    std::uint64_t delayed_damage_name_completion_count{};
    std::unordered_map<std::uint64_t, bool> damage_critical_tag_cache;
    std::unordered_map<std::uint64_t, std::string> damage_participant_paths;
    static constexpr std::size_t kCombatEventCapacity = 512;
    struct CombatEventRecord {
        AnomalyNteCombatEventV1 event{};
    };
    std::array<CombatEventRecord, kCombatEventCapacity> combat_events{};
    std::size_t combat_event_start{};
    std::size_t combat_event_count{};
    std::uint64_t combat_event_sequence{};
    std::unordered_map<std::uint64_t, std::string> combat_event_names;
    std::unordered_map<std::uint64_t, std::uintptr_t> combat_event_objects;
    std::deque<std::uint64_t> pending_combat_event_names;
    std::unordered_set<std::uint64_t> queued_combat_event_names;
    std::unordered_set<std::uint64_t> failed_combat_event_names;
    std::unordered_map<std::uint64_t, std::string> combat_participant_names;
    struct PendingCombatParticipantName {
        AnomalyGenerationHandleV1 participant{};
        std::uintptr_t object{};
        std::array<std::uint64_t, 3> keys{};
        std::array<std::string, 3> key_texts{};
        std::uint8_t key_count{};
        std::uint8_t attempts{};
        std::uint8_t scene_retry_attempts{};
        bool player_participant{};
        std::uint64_t config_id_key{};
        bool has_config_id{};
        std::uint64_t next_retry_sequence{};
    };
    std::deque<PendingCombatParticipantName> pending_combat_participant_names;
    std::unordered_set<std::uint64_t> queued_combat_participant_names;
    std::unordered_set<std::uint64_t> failed_combat_participant_names;
    std::uint64_t monster_static_data_resolution_calls{};
    std::uint64_t monster_static_data_resolution_successes{};
    mutable std::uint64_t string_table_binding_attempts{};
    mutable std::uint64_t string_table_binding_failures{};
    mutable std::uint64_t string_table_call_count{};
    mutable std::uint64_t string_table_success_count{};
    mutable std::uint64_t string_table_thread_rejections{};
    mutable std::uint8_t string_table_binding_failure_code{};
    mutable std::string string_table_last_key;
    mutable std::string string_table_last_value;
    std::uint64_t participant_last_handle{};
    bool participant_last_player{};
    std::string participant_last_class_text;
    std::string participant_last_config_text;
    std::unordered_map<std::uint64_t, std::string> ability_display_names;
    std::unordered_map<std::uint64_t, std::uint8_t> ability_display_name_attempts;
    // FName -> localized FText values decoded only for keys requested by a
    // combat event, participant, or skill.
    std::unordered_map<std::uint64_t, std::string> localized_names_by_fname;
    std::unordered_map<std::string, std::string> localized_names_by_key;
    std::uint64_t display_table_generation{};
    bool display_table_scan_complete{};
    std::uint8_t display_table_loaded_mask{};
    std::uintptr_t display_game_data{};
    std::uint64_t display_game_data_generation{};
    struct SparseMapView {
        std::uintptr_t map{};
        std::uintptr_t data{};
        std::uintptr_t flags_data{};
        std::int32_t num{};
        std::int32_t num_free{};
        std::int32_t max{};
        std::int32_t flags_num{};
        std::int32_t flags_max{};
        std::int64_t stride{};
        std::int64_t row_offset{};
        std::vector<std::uint32_t> flags;
    };
    struct DisplayTableIndex {
        std::uintptr_t table{};
        std::int64_t text_offset{-1};
        std::unordered_map<std::uint64_t, std::uintptr_t> rows_by_fname;
        std::unordered_map<std::string, std::uintptr_t> rows_by_key;
    };
    struct DamageSkillIndex {
        std::uintptr_t table{};
        std::unordered_map<std::uint64_t, std::uint64_t> ability_by_effect_fname;
        std::unordered_map<std::string, std::uint64_t> ability_by_effect_key;
    } damage_skill_index;

    struct StringTableEntryBinding {
        std::uintptr_t function{};
        std::uintptr_t receiver{};
        std::uint64_t table_id{};
        std::uint16_t parms_size{};
        std::uint16_t table_offset{};
        std::uint16_t key_offset{};
        std::uint16_t return_offset{};
        std::uint64_t object_generation{};
        bool attempted{};
        std::array<std::uint8_t, 64> parameters{};
    };
    mutable StringTableEntryBinding string_table_entry;
    mutable StringTableEntryBinding abyss_string_table_entry;
    struct RegisteredStringTablesBinding {
        std::uintptr_t function{};
        std::uintptr_t receiver{};
        std::uint16_t parms_size{};
        std::uint16_t return_offset{};
        std::uint64_t object_generation{};
        bool attempted{};
        std::array<std::uint8_t, 64> parameters{};
    };
    mutable RegisteredStringTablesBinding registered_string_tables;
    std::array<DisplayTableIndex, 4> display_table_indexes{};
    static constexpr std::size_t kSceneMonsterTableCapacity = 5;
    std::array<DisplayTableIndex, kSceneMonsterTableCapacity>
        scene_monster_table_indexes{};
    std::size_t scene_monster_table_count{};
    std::uint64_t scene_monster_table_generation{};
    bool scene_monster_table_scan_complete{};
    static constexpr std::size_t kCombatCaptureQueueCapacity = 128;
    // The ProcessEvent tap only needs the reflected fields below. Keeping the
    // queue payload bounded to the largest useful value avoids copying the
    // 0x298-byte FGameplayEffectSpec for every buff notification.
    static constexpr std::size_t kCombatCapturePayloadBytes =
        (std::max)(std::size_t{96}, sizeof(NteCharacterDamageCapture));
    enum class CombatCaptureKind : std::uint8_t {
        CharacterDamage,
        Damage,
        BuffAdd,
        BuffRemove,
    };
    struct PendingCombatCapture {
        CombatCaptureKind kind{};
        std::uint16_t payload_size{};
        std::uint16_t info_offset{};
        std::uint64_t tick_sequence{};
        std::array<std::uint8_t, kCombatCapturePayloadBytes> payload{};
    };
    std::array<PendingCombatCapture, kCombatCaptureQueueCapacity> combat_capture_queue{};
    std::atomic<std::uint32_t> combat_capture_write{};
    std::atomic<std::uint32_t> combat_capture_read{};
    std::atomic<std::uint64_t> combat_capture_drop_count{};
    struct CombatCaptureBindings {
        std::uintptr_t damage{};
        std::uintptr_t monster_damage{};
        std::uintptr_t player_damage_queue{};
        std::uint16_t player_damage_queue_offset{};
        std::uintptr_t damage_widget{};
        std::uint16_t damage_widget_info_offset{};
        struct Buff {
            std::uintptr_t function{};
            std::uint16_t parms_size{};
            std::uint16_t object_offset{0xFFFFU};
            std::uint16_t definition_offset{0xFFFFU};
            std::uint16_t duration_offset{0xFFFFU};
            std::uint16_t stack_offset{0xFFFFU};
            std::uint16_t is_add_offset{0xFFFFU};
        };
        std::array<Buff, 8> buffs{};
        std::uint16_t damage_info_offset{};
        std::uint16_t damage_value_offset{0xFFFFU};
        std::uint16_t damage_source_offset{0xFFFFU};
        std::uint16_t damage_tags_offset{0xFFFFU};
    };
    // Slots are generation-local and never freed. Publish a fully populated
    // slot with one acquire/release pointer load in the ProcessEvent hook.
    std::array<CombatCaptureBindings, 2> combat_capture_binding_slots{};
    std::atomic<const CombatCaptureBindings*> combat_capture_bindings{};
    std::uint8_t combat_capture_binding_slot{};
    std::atomic<std::uint64_t> damage_floaties_call_count{};
    std::atomic<std::uint64_t> monster_damage_call_count{};
    std::atomic<std::uint64_t> player_damage_queue_call_count{};
    std::atomic<std::uint64_t> damage_widget_call_count{};
    std::atomic<std::uint64_t> buff_call_count{};
    std::atomic<std::uint64_t> crit_query_call_count{};
    std::atomic<std::uint64_t> crit_query_success_count{};
    std::atomic<std::uint64_t> crit_true_count{};
    std::atomic_bool combat_demand{};
    std::atomic_bool display_name_demand{};
    std::uint64_t combat_attempt_sequence{};
    std::uint64_t combat_sample_sequence{};
    AnomalyGenerationHandleV1 combat_character{};
    AnomalyGenerationHandleV1 combat_target{};
    double combat_hp{};
    double combat_max_hp{};
    double combat_shield{};
    bool combat_dead{};
    bool combat_available{};
    bool combat_partial{};
    std::uint32_t combat_refresh_failure{};
    struct ActiveEffectRecord {
        std::int32_t replication_id{};
        std::int32_t replication_key{};
        std::uintptr_t definition{};
        std::uint64_t name_id{};
        float duration_seconds{};
        std::int32_t stack_count{};
    };
    std::vector<ActiveEffectRecord> active_effects;
    std::uintptr_t active_effect_ability_system{};
    std::int32_t active_effect_array_key{};
    bool active_effects_initialized{};
    struct SkillRecord {
        AnomalyGenerationHandleV1 handle{};
        AnomalyGenerationHandleV1 character{};
        AnomalyGenerationHandleV1 ability_class{};
        std::uint64_t sequence{};
        std::uint32_t flags{};
        std::int32_t level{};
        std::int32_t input_id{};
        std::int32_t spec_handle{};
        float cooldown_remaining_seconds{};
        float cooldown_duration_seconds{};
        std::uintptr_t ability{};
        std::uintptr_t ability_class_pointer{};
        std::string ability_path;
    };
    std::vector<SkillRecord> skills;
    std::atomic_bool skill_demand{};
    std::uint64_t skill_generation{};
    std::uint64_t skill_next_id{1};
    std::uint64_t skill_attempt_sequence{};
    std::uint64_t skill_sample_sequence{};
    std::uintptr_t skill_ability_system{};
    AnomalyGenerationHandleV1 skill_character{};
    bool skills_available{};
    bool skills_partial{};
    struct TeleportBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t new_location_offset{};
        std::uint16_t sweep_hit_result_offset{};
        std::uint16_t sweep_hit_result_size{};
        ReflectedBoolParameter b_sweep{};
        ReflectedBoolParameter b_teleport{};
        ReflectedBoolParameter return_value{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool available{};
        bool discovery_complete{};
    } teleport;
    struct MapLandmarkBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t teleport_id_offset{};
        std::uint16_t transfer_mode_offset{};
        std::uint64_t object_generation{};
        bool available{};
    } map_landmark_binding;
    struct MapLandmarkRecord {
        std::string teleport_id;
        std::string world;
        std::array<double, 3> world_position{};
        std::array<double, 3> destination{};
        std::uint32_t point_type{};
        std::int32_t floor{};
        bool destination_overridden{};
    };
    struct MapLandmarkCatalog {
        std::vector<MapLandmarkRecord> entries;
        std::uint64_t sequence{};
        std::uint64_t object_generation{};
    };
    std::shared_ptr<const MapLandmarkCatalog> map_landmark_catalog;
    std::uint64_t map_landmark_catalog_sequence{};
    std::uint64_t map_landmark_next_refresh_sequence{};
    struct NavigationBinding {
        std::uintptr_t move_to_point_by_transform{};
        std::uintptr_t util_class{};
        std::uint32_t move_object_index{};
        std::uint32_t move_object_serial{};
        std::uint16_t move_parms_size{};
        std::uint16_t world_context_object_offset{};
        std::uint16_t move_location_offset{};
        std::uint16_t move_rotator_offset{};
        ReflectedBoolParameter force_walk{};
        ReflectedBoolParameter auto_control{};
        ReflectedBoolParameter hide_ui{};
        std::uint16_t protect_time_offset{};
        ReflectedBoolParameter use_pathfinding{};
        std::uintptr_t stop_movement{};
        std::uint32_t stop_object_index{};
        std::uint32_t stop_object_serial{};
        std::uint16_t stop_parms_size{};
        std::uintptr_t registry_items{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool available{};
        bool discovery_complete{};
    } navigation;
    struct PendingPickupRequest {
        double radius{};
        std::uint32_t maximum_items{};
        std::uint32_t attempts{};
        bool queued{};
    } pickup_request;
    struct PickupConfirmationCandidate {
        std::uintptr_t actor{};
        std::uintptr_t controller{};
        std::uintptr_t can_try_function{};
        std::uint32_t object_index{};
        std::uint32_t object_serial{};
        std::uint64_t entity_sequence{};
        std::int32_t interact_index{};
        std::uint8_t baseline{};
    };
    struct PickupConfirmation {
        std::vector<PickupConfirmationCandidate> candidates;
        std::chrono::steady_clock::time_point next_check{};
        std::chrono::steady_clock::time_point deadline{};
    } pickup_confirmation;
    AnomalyNtePickupSnapshotV1 pickup_snapshot{sizeof(AnomalyNtePickupSnapshotV1)};
    std::uint64_t pickup_sequence{};
    // Created with the Profile. Ticked after the locked sampling pass because a click runs
    // arbitrary game code; it takes this State's mutex only for name and object lookups.
    std::unique_ptr<NteUiButtons> ui_buttons;
    enum class AhudFunctionKind : std::size_t {
        ReceiveDrawHud,
        Project,
        DrawText,
        DrawLine,
        DrawRect,
        GetTextSize,
        Count,
    };
    static constexpr std::size_t kAhudFunctionCount =
        static_cast<std::size_t>(AhudFunctionKind::Count);
    static constexpr std::size_t kMaximumAhudParameters = 7;
    struct AhudFunctionBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::array<std::uint16_t, kMaximumAhudParameters> offsets{};
        std::array<ReflectedBoolParameter, kMaximumAhudParameters> bool_parameters{};
    };
    struct AhudBinding {
        std::array<AhudFunctionBinding, kAhudFunctionCount> functions{};
        std::uint64_t object_generation{};
    };
    struct AhudDiscovery {
        std::array<std::optional<AhudFunctionBinding>, kAhudFunctionCount> functions{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool discovery_complete{};
    } ahud_discovery;
    struct AhudParameterSpec {
        std::string_view name;
        std::string_view type;
        std::string_view structure;
        std::int32_t element_size{};
        bool return_value{};
    };
    struct AhudFunctionSpec {
        AhudFunctionKind kind{};
        std::string_view name;
        std::uint16_t parms_size{};
        std::span<const AhudParameterSpec> parameters;
    };
    struct AhudFrameCallContext {
        std::uintptr_t hud{};
        const AhudBinding* binding{};
        const ProcessEventInvoker* invoker{};
        std::atomic_uint64_t* process_event_call_count{};
    };
    struct NativeUtf16StringHeader {
        wchar_t* data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };
    static_assert(sizeof(NativeUtf16StringHeader) == 16);
    std::atomic<std::shared_ptr<const AhudBinding>> ahud_binding;
    std::atomic_bool ahud_demand{};
    std::atomic_uint64_t ahud_frame_count{};
    std::atomic_uint64_t ahud_process_event_call_count{};
    std::uintptr_t player_pawn{};
    std::uintptr_t player_controller{};
    std::uintptr_t player_root{};
    // Default teleport mode moves the character first and then pins it at the destination while
    // the engine streams the cells in around it. Holding the character at the *origin* and
    // teleporting it afterwards was measurably fatal: the fall the game settles is anchored where
    // the character was standing, so the whole origin-to-destination height difference was charged
    // on landing. Never being anywhere but the destination is what keeps that difference out of it.
    struct ArrivalHold {
        std::chrono::steady_clock::time_point started{};
        std::chrono::milliseconds window{};
        bool active{};
    };

    // Framework-owned streaming override shared by every consumer of
    // anomaly.ue5.streaming-source and by the teleport preload.
    std::unique_ptr<Ue5StreamingSourceOverride> streaming_source_override;
    ArrivalHold arrival_hold;
    std::uint64_t player_generation{};
    std::uint64_t player_attempt_sequence{};
    std::uint64_t player_sample_sequence{};
    std::array<double, 3> player_position{};
    std::array<double, 3> player_bounds_center{};
    std::array<double, 3> player_bounds_extent{};
    std::array<double, 3> camera_position{};
    std::array<double, 3> camera_rotation{};
    float camera_horizontal_fov{};
    bool player_available{};
    bool player_esp_available{};
    bool player_partial{};
    struct EntityRecord {
        std::uintptr_t actor{};
        std::uintptr_t class_object{};
        std::uint32_t object_index{};
        std::uint32_t object_serial{};
        std::uint32_t flags{};
        bool object_identity_available{};
        std::uint64_t entity_id{};
        std::uint64_t class_id{};
        std::uint32_t entity_name_id{};
        std::uint32_t class_name_id{};
        std::array<double, 3> bounds_center{};
        std::array<double, 3> bounds_extent{};
    };
    struct EntityFrameCache {
        std::vector<EntityRecord> entities;
        std::unordered_map<std::uint64_t, std::string> class_names;
        std::unordered_map<std::uint64_t, std::string> entity_names;
        std::uint64_t generation{};
        std::uint64_t sequence{};
        std::array<double, 3> camera_position{};
        std::array<double, 3> camera_rotation{};
        float camera_horizontal_fov{};
        bool partial{};
    };
    // FName 的 comparison index 在进程内稳定，同一 name_id 永远对应同一个字符串。
    // 全关卡 actor 扫描会为每个 actor 解析一次实体名（数千次），逐次解码宽字符名
    // 的代价要一秒以上；记忆化后只有首次扫描需要真正解码。失败结果不缓存，
    // 因为那通常意味着布局尚未就绪，之后的扫描应当重试。
    mutable std::unordered_map<std::uint32_t, std::string> name_snapshot_cache;
    std::shared_ptr<const EntityFrameCache> entity_frame_cache;
    std::shared_ptr<const EntityFrameCache> previous_entity_frame_cache;
    std::uint64_t entity_generation{};
    std::uint64_t entity_attempt_sequence{};
    std::shared_ptr<const EntityFrameCache> actor_frame_cache;
    std::uint64_t actor_generation{};
    std::uint64_t actor_world_generation{};
    // 全部关卡 actor 快照按 tick 节流重扫：只在 World 变化时刷新会让快照永久冻结，
    // 死掉的 actor 留在列表里、新生成的 actor 永远不可见（大世界全程是同一个 World）。
    std::uint64_t actor_attempt_sequence{};
    std::uint64_t snapshot_tick_count{};
    std::uint64_t latest_snapshot_cost_micros{};
    std::uint64_t total_snapshot_cost_micros{};
    std::uint64_t max_snapshot_cost_micros{};
    std::uint64_t player_refresh_count{};
    std::uint64_t player_cache_hit_count{};
    std::uint64_t entity_refresh_count{};
    std::uint64_t entity_cache_hit_count{};
    std::uint64_t entity_page_request_count{};
    std::uint64_t entity_page_cache_hit_count{};
    bool framework_hook_ready{};
    bool ahud_hook_ready{};
    bool process_event_hook_ready{};
    std::shared_ptr<NteNavigationInputPolicy> navigation_input_policy;
    std::uint64_t deferred_resolution_retry_sequence{1};
    std::vector<std::pair<std::string, const void*>> published;
    std::vector<std::pair<std::string, const void*>> pending_revocations;
    std::optional<std::pair<std::string, const void*>> revocation_in_flight;
    std::atomic_bool revocation_call_active{};
    std::atomic<std::shared_ptr<SemanticServiceEndpoint>> semantic_endpoint;
    std::shared_ptr<SemanticServiceEndpoint> draining_semantic_endpoint;
    std::atomic<std::shared_ptr<CallbackEndpoint>> callback_endpoint;
    std::shared_ptr<CallbackEndpoint> draining_callback_endpoint;
    std::atomic<std::shared_ptr<AhudServiceEndpoint>> ahud_endpoint;
    std::shared_ptr<AhudServiceEndpoint> draining_ahud_endpoint;
    std::atomic<std::shared_ptr<ProcessEventServiceEndpoint>> process_event_endpoint;
    std::shared_ptr<ProcessEventServiceEndpoint> draining_process_event_endpoint;

    const ResolvedSymbol* Symbol(std::string_view id) const noexcept {
        return resolution.FindSymbol(id);
    }

    [[nodiscard]] static constexpr std::size_t AhudIndex(
        const AhudFunctionKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    [[nodiscard]] static constexpr std::size_t NteIndex(
        const NteFunctionKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    [[nodiscard]] static NteFunctionSpec NteSpec(
        const NteFunctionKind kind) noexcept {
        static constexpr std::array get_ability_system{
            NteFunctionParameterSpec{"ReturnValue", "ObjectProperty", 8, true}};
        static constexpr std::array get_name{
            NteFunctionParameterSpec{"ReturnValue", "NameProperty", 8, true}};
        static constexpr std::array get_float{
            NteFunctionParameterSpec{"ReturnValue", "FloatProperty", 4, true}};
        static constexpr std::array get_hp_max{
            NteFunctionParameterSpec{"bIsFixHPMax", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"ReturnValue", "FloatProperty", 4, true}};
        static constexpr std::array get_bool{
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        static constexpr std::array get_object{
            NteFunctionParameterSpec{"ReturnValue", "ObjectProperty", 8, true}};
        static constexpr std::array activate{
            NteFunctionParameterSpec{
                "InAbilityToActivate", "ClassProperty", 8, false},
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        static constexpr std::array cooldown{
            NteFunctionParameterSpec{
                "GameplayEffect", "ClassProperty", 8, false},
            NteFunctionParameterSpec{"TimeRemaining", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"CooldownDuration", "FloatProperty", 4, false}};
        static constexpr std::array damage_text{
            NteFunctionParameterSpec{"InDamageInfo", "StructProperty", 72, false}};
        static constexpr std::array monster_damage_text{
            NteFunctionParameterSpec{"InDamageTextInfo", "StructProperty", 72, false}};
        static constexpr std::array damage_text_queue{
            NteFunctionParameterSpec{
                "InDamageTextInfoQueue", "StructProperty", 16, false}};
        static constexpr std::array damage_widget{
            NteFunctionParameterSpec{"InDamageFloatiesForm", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"InDamageInfo", "StructProperty", 72, false},
            NteFunctionParameterSpec{"bNeedSetTransform", "BoolProperty", 1, false}};
        static constexpr std::array active_effect_added{
            NteFunctionParameterSpec{"Source", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false}};
        static constexpr std::array active_effect_removed{
            NteFunctionParameterSpec{"ActiveEffect", "StructProperty", 864, false}};
        static constexpr std::array buff_info_add{
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false}};
        static constexpr std::array buff_info_remove{
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false}};
        static constexpr std::array buffer_manager_add{
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false},
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"bIsAdd", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false},
            NteFunctionParameterSpec{"bInPlayOpenAnim", "BoolProperty", 1, false}};
        static constexpr std::array head_up_add{
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false}};
        static constexpr std::array head_up_remove{
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false}};
        static constexpr std::array monster_buffer_add{
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false},
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"bIsAdd", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false},
        };
        static constexpr std::array get_monster_static_data{
            NteFunctionParameterSpec{"WorldContextObject", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"ConfigID", "NameProperty", 8, false},
            NteFunctionParameterSpec{"InOutMonsterStaticData", "StructProperty", 296, false},
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        switch (kind) {
        case NteFunctionKind::GetAbilitySystemComponent:
            return {kind, "K2_GetAbilitySystemComponent", "HTAbilityCharacter", 8,
                get_ability_system};
        case NteFunctionKind::GetMainCharacterId:
            return {kind, "GetMainCharacterID", "HTPlayerCharacter", 8, get_name};
        case NteFunctionKind::GetNpcMainCharacterId:
            return {kind, "GetMainCharacterID", "HTPlayerNPCCharacter", 8, get_name};
        case NteFunctionKind::GetHp:
            return {kind, "GetHP", "HTAbilityCharacter", 4, get_float};
        case NteFunctionKind::GetHpMax:
            return {kind, "GetHPMax", "HTAbilityCharacter", 8, get_hp_max};
        case NteFunctionKind::GetIsDead:
            return {kind, "GetIsDead", "HTAbilityCharacter", 1, get_bool};
        case NteFunctionKind::GetAttackTarget:
            return {kind, "GetAttackTarget", "HTAbilityCharacter", 8, get_object};
        case NteFunctionKind::GetShieldHealth:
            return {kind, "GetShieldHealth", "HTAttributeComponent", 4, get_float};
        case NteFunctionKind::GetActiveEffectTimeRemainingAndDuration:
            return {kind, "GetActiveEffectTimeRemainingAndDuration",
                "HTAbilitySystemComponent", 16, cooldown};
        case NteFunctionKind::ActivateAbilityByClass:
            return {kind, "HTTryActivateAbilityByClass", "HTAbilitySystemComponent", 9,
                activate};
        case NteFunctionKind::ShowDamageFloaties:
            return {kind, "ShowDamageFloaties", "HTUI_DamageFloatiesForm", 72, damage_text};
        case NteFunctionKind::MulticastShowMonsterDamageInfo:
            return {kind, "MulticastShowMonsterDamageInfo", "HTMonsterCharacter", 72,
                monster_damage_text};
        case NteFunctionKind::ClientShowPlayerDamageInfo:
            return {kind, "ClientShowPlayerDamageInfo", "HTPlayerCharacter", 16,
                damage_text_queue};
        case NteFunctionKind::SetDamageInfo:
            return {kind, "SetDamageInfo", "HTUI_DamageFloatiesWidget", 81,
                damage_widget};
        case NteFunctionKind::OnActiveGameplayEffectAdded:
            return {kind, "BP_OnActiveGameplayEffectAdded", "HTUI_AbilityCustomBase", 680,
                active_effect_added};
        case NteFunctionKind::OnAnyGameplayEffectRemoved:
            return {kind, "BP_OnAnyGameplayEffectRemoved", "HTUI_AbilityCustomBase", 864,
                active_effect_removed};
        case NteFunctionKind::AddBuffControl:
            return {kind, "AddBuffControl", "HTUI_BuffInfoManager", 672, buff_info_add};
        case NteFunctionKind::RemoveFromBuffControl:
            return {kind, "RemoveFromBuffControl", "HTUI_BuffInfoManager", 664, buff_info_remove};
        case NteFunctionKind::AddBufferManagerBuffControl:
            return {kind, "AddBuffControl", "HTUI_BufferManager", 685,
                buffer_manager_add};
        case NteFunctionKind::AddHeadUpBattleMarkBuffControl:
            return {kind, "AddBuffControl", "HTUI_HeadUpBattleMark", 16, head_up_add};
        case NteFunctionKind::RemoveHeadUpBattleMarkBuffControl:
            return {kind, "RemoveFromBuffControl", "HTUI_HeadUpBattleMark", 8, head_up_remove};
        case NteFunctionKind::AddMonsterBufferControl:
            return {kind, "AddBuffControl", "HTUI_MonsterBufferManager", 28,
                monster_buffer_add};
        case NteFunctionKind::GetMonsterStaticData:
            return {kind, "K2_GetMonsterStaticData", "HTSceneSolelyDataAsset", 313,
                get_monster_static_data};
        case NteFunctionKind::CurrentDamageIsCrit:
            return {kind, "CurrentDamageIsCrit", "HTAttributeComponent", 1, get_bool};
        case NteFunctionKind::Count: break;
        }
        return {};
    }

    [[nodiscard]] static AhudFunctionSpec AhudSpec(
        const AhudFunctionKind kind) noexcept {
        static constexpr std::array receive{
            AhudParameterSpec{"SizeX", "IntProperty", {}, 4, false},
            AhudParameterSpec{"SizeY", "IntProperty", {}, 4, false},
        };
        static constexpr std::array project{
            AhudParameterSpec{"Location", "StructProperty", "Vector", 24, false},
            AhudParameterSpec{"bClampToZeroPlane", "BoolProperty", {}, 1, false},
            AhudParameterSpec{"ReturnValue", "StructProperty", "Vector", 24, true},
        };
        static constexpr std::array draw_text{
            AhudParameterSpec{"Text", "StrProperty", {}, 16, false},
            AhudParameterSpec{"TextColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"ScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"Font", "ObjectProperty", {}, 8, false},
            AhudParameterSpec{"Scale", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"bScalePosition", "BoolProperty", {}, 1, false},
        };
        static constexpr std::array draw_line{
            AhudParameterSpec{"StartScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"StartScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"EndScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"EndScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"LineColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"LineThickness", "FloatProperty", {}, 4, false},
        };
        static constexpr std::array draw_rect{
            AhudParameterSpec{"RectColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"ScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenW", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenH", "FloatProperty", {}, 4, false},
        };
        static constexpr std::array get_text_size{
            AhudParameterSpec{"Text", "StrProperty", {}, 16, false},
            AhudParameterSpec{"OutWidth", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"OutHeight", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"Font", "ObjectProperty", {}, 8, false},
            AhudParameterSpec{"Scale", "FloatProperty", {}, 4, false},
        };
        switch (kind) {
        case AhudFunctionKind::ReceiveDrawHud:
            return {kind, "ReceiveDrawHUD", 8, receive};
        case AhudFunctionKind::Project:
            return {kind, "Project", 56, project};
        case AhudFunctionKind::DrawText:
            return {kind, "DrawText", 53, draw_text};
        case AhudFunctionKind::DrawLine:
            return {kind, "DrawLine", 36, draw_line};
        case AhudFunctionKind::DrawRect:
            return {kind, "DrawRect", 32, draw_rect};
        case AhudFunctionKind::GetTextSize:
            return {kind, "GetTextSize", 36, get_text_size};
        case AhudFunctionKind::Count: break;
        }
        return {};
    }

    bool Publish(
        std::string id,
        std::uint32_t version,
        const void* table,
        AdapterServiceRegistry::QueryObserver query_observer = {},
        std::shared_ptr<const void> lifetime = {}) {
        std::scoped_lock publication_lock(publication_mutex);
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!services->Publish(
                id, version, table, std::move(query_observer), std::move(lifetime))) {
            return false;
        }
        published.emplace_back(std::move(id), table);
        return true;
    }

    bool IsPublished(std::string_view id) const noexcept {
        std::scoped_lock publication_lock(publication_mutex);
        return std::ranges::any_of(published, [&](const auto& entry) {
            return entry.first == id;
        });
    }

    bool PublishIfMissing(
        std::string_view id,
        std::uint32_t version,
        const void* table,
        AdapterServiceRegistry::QueryObserver query_observer = {},
        std::shared_ptr<const void> lifetime = {}) {
        return IsPublished(id) || Publish(
            std::string(id), version, table, std::move(query_observer), std::move(lifetime));
    }

    [[nodiscard]] std::size_t PublishedCount() const noexcept {
        std::scoped_lock publication_lock(publication_mutex);
        return published.size();
    }

    [[nodiscard]] bool SemanticServicesAvailable() const noexcept {
        return resolution.state != ProfileResolutionState::NoProfile &&
            resolution.profile_hash == profile.source_hash;
    }

    [[nodiscard]] bool SemanticServicesRunning() const noexcept {
        return started.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool SemanticFeatureRunning(std::string_view feature) const noexcept {
        return SemanticServicesRunning() && SemanticFeatureAvailable(feature);
    }

    [[nodiscard]] static bool LayoutKeysAvailable(
        const BuildProfile& profile,
        std::initializer_list<std::string_view> keys) noexcept {
        constexpr std::int64_t kMaximumFieldOffset = 64LL * 1024LL * 1024LL;
        return std::ranges::all_of(keys, [&profile](const std::string_view key) {
            const auto value = Layout(profile, key);
            return value >= 0 && value <= kMaximumFieldOffset;
        });
    }

    [[nodiscard]] static bool FeatureDeclaresLayoutValidator(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view validator) noexcept {
        const auto validators = profile.feature_layout_validators.find(feature);
        return validators != profile.feature_layout_validators.end() && std::ranges::any_of(
            validators->second, [validator](const std::string& candidate) {
                return candidate == validator;
            });
    }

    [[nodiscard]] static bool FeatureDeclaresSymbol(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view symbol) noexcept {
        const auto symbols = profile.features.find(feature);
        return symbols != profile.features.end() && std::ranges::any_of(
            symbols->second, [symbol](const std::string& candidate) {
                return candidate == symbol;
            });
    }

    [[nodiscard]] static bool FeatureDeclaresDependency(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view dependency) noexcept {
        const auto dependencies = profile.feature_dependencies.find(feature);
        return dependencies != profile.feature_dependencies.end() && std::ranges::any_of(
            dependencies->second, [dependency](const std::string& candidate) {
                return candidate == dependency;
            });
    }

    [[nodiscard]] bool NtePlayerLayoutAvailable() const noexcept {
        return LayoutKeysAvailable(profile, {
            "world.gameInstance",
            "gameInstance.localPlayers",
            "localPlayer.controller",
            "controller.pawn",
            "actor.rootComponent",
            "sceneComponent.location"});
    }

    [[nodiscard]] bool NtePlayerEspLayoutAvailable() const noexcept {
        return NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
            "controller.cameraManager",
            "sceneComponent.boundsOrigin",
            "sceneComponent.boundsExtent",
            "cameraManager.location",
            "cameraManager.rotation",
            "cameraManager.fov"});
    }

    [[nodiscard]] bool NteVehicleProfileAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) && framework_hook_ready &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.outer",
                "ustruct.propertyLink", "ufunction.numParms", "ufunction.parmsSize",
                "ufunction.returnValueOffset", "ffield.name", "ffield.class",
                "ffieldClass.name", "fproperty.arrayDim", "fproperty.elementSize",
                "fproperty.offsetInternal", "fproperty.propertyLinkNext",
                "fobjectProperty.propertyClass", "fstructProperty.struct",
                "vehicle.movementComponent", "vehicle.maxEngineTorque"}) &&
            FeatureDeclaresDependency(profile, "nte.vehicle", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.vehicle", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.vehicle", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.vehicle", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(profile, "nte.vehicle", "nte-vehicle-layout-v1");
    }

    [[nodiscard]] bool NteCombatProfileAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) && framework_hook_ready &&
            resolution.FeatureAvailable("nte.combat") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.outer",
                "uclass.classDefaultObject",
                "ustruct.superStruct", "ustruct.propertyLink", "ufunction.numParms",
                "ufunction.parmsSize", "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name", "ffieldClass.name", "fproperty.arrayDim",
                "fproperty.elementSize", "fproperty.offsetInternal",
                "fproperty.propertyLinkNext", "fstructProperty.struct",
                "fobjectProperty.propertyClass", "farrayProperty.inner",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset", "fboolProperty.byteMask",
                "fboolProperty.fieldMask", "damageEvent.size", "damageEvent.damage",
                "damageEvent.damageGEDef", "damageEvent.damageTags",
                "controller.playerState", "playerState.roleName",
                "abilityCharacter.characterConfigId",
                "weakObject.index", "weakObject.serial",
                "abilitySpawnActor.triggerAbilityHandle",
                 "abilitySpawnActor.savedTriggerSkillCDO",
                 "abilitySystem.tryActivateAbilityStack", "gameplayEffectSpec.size",
                 "gameplayEffectSpec.def", "gameplayEffectSpec.duration",
                 "gameplayEffectSpec.stackCount", "activeGameplayEffect.size",
                 "activeGameplayEffect.spec", "activeGameplayEffect.replicationId",
                 "activeGameplayEffect.replicationKey", "abilitySystem.activeGameplayEffects",
                 "activeGameplayEffects.size", "activeGameplayEffects.arrayReplicationKey",
                 "activeGameplayEffects.items", "buffs.maxCount",
                "damageTextInfo.displayDamage", "damageTextInfo.damageType",
                "damageTextInfo.critical", "damageTextInfo.headHit",
                "damageTextInfo.weakUnbalance", "damageTextInfo.attacker",
                "damageTextInfo.victim", "damageTextInfo.combatStatistics",
                "damageTextInfo.basicDamage", "damageTextInfo.finalDamage",
                 "damageTextInfo.displayType", "damageTextInfo.reactionType",
                 "damageTextInfo.reactionDisplayType",
                 "gameplayEffect.uiData", "gameplayEffectUIData.description",
                 "buff.specDef",
                 "buff.duration", "buff.stackCount", "ftext.textData",
                 "ftextData.textSource", "fstring.data", "fstring.count",
                 "fstring.capacity", "abilityCharacter.abilitySystemComponent",
                 "gameData.abilityDataAsset",
                 "abilityData.skillDamageDataTable", "skillDamage.gaName",
                 "gameData.characterDataTable", "gameData.gameplayAbilityTipsDataTable",
                 "gameData.gameplayEffectTipsDataTable", "gameplayAbilityTips.name",
                 "gameplayAbilityTips.gameplayAbility", "gameplayEffectTips.name",
                 "gameplayEffectTips.geParamName",
                 "monsterData.textName", "dataTable.rowMap", "dataTable.rowMapData",
                 "dataTable.rowMapNum", "dataTable.rowMapNumFree", "dataTable.rowMapMax",
                 "dataTable.rowMapElementStride", "dataTable.rowMapRowOffset",
                 "dataTable.rowMapInlineFlags", "dataTable.rowMapFlagsData",
                 "dataTable.rowMapFlagsNum", "dataTable.rowMapFlagsMax"}) &&
            FeatureDeclaresDependency(profile, "nte.combat", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.combat", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.combat", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.combat", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.combat", "nte-combat-reflection-v1");
    }

    [[nodiscard]] bool NteSkillsProfileAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) && framework_hook_ready &&
            resolution.FeatureAvailable("nte.skills") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.outer",
                "ustruct.superStruct", "ustruct.propertyLink", "ufunction.numParms",
                "ufunction.parmsSize", "ufunction.returnValueOffset", "ffield.class",
                "ffield.name", "ffieldClass.name", "fproperty.arrayDim",
                "fproperty.elementSize", "fproperty.offsetInternal",
                "fproperty.propertyLinkNext", "fstructProperty.struct",
                "fobjectProperty.propertyClass", "farrayProperty.inner",
                "fclassProperty.metaClass",
                "fboolProperty.fieldSize", "fboolProperty.byteOffset",
                "fboolProperty.byteMask", "fboolProperty.fieldMask", "tarray.data",
                "tarray.num", "tarray.max", "abilitySystem.activatableAbilities",
                "abilitySpecContainer.items", "abilitySpec.stride",
                "abilitySpec.handle", "abilitySpec.ability", "abilitySpec.level",
                "abilitySpec.inputId", "abilitySpec.activeCount",
                "abilitySpec.stateBits", "ability.cooldownGameplayEffectClass",
                "skills.maxCount"}) &&
            FeatureDeclaresDependency(profile, "nte.skills", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.skills", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.skills", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.skills", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.skills", "nte-skills-layout-v1");
    }

    [[nodiscard]] bool NteSkillInvocationProfileAvailable() const noexcept {
        return NteSkillsProfileAvailable() &&
            resolution.FeatureAvailable("nte.skill-invocation") &&
            FeatureDeclaresDependency(
                profile, "nte.skill-invocation", "nte.skills") &&
            FeatureDeclaresDependency(
                profile, "nte.skill-invocation", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.skill-invocation", "nte-skill-invocation-v1");
    }

    [[nodiscard]] bool NteFunctionReady(const NteFunctionKind kind) const noexcept {
        return combat_skill_discovery.functions[NteIndex(kind)].has_value();
    }

    [[nodiscard]] bool NteCombatReflectionReady() const noexcept {
        return combat_skill_discovery.damage_event_layout_valid &&
            NteFunctionReady(NteFunctionKind::GetAbilitySystemComponent) &&
            NteFunctionReady(NteFunctionKind::GetHp) &&
            NteFunctionReady(NteFunctionKind::GetHpMax) &&
            NteFunctionReady(NteFunctionKind::GetIsDead) &&
            NteFunctionReady(NteFunctionKind::GetAttackTarget) &&
            NteFunctionReady(NteFunctionKind::GetShieldHealth);
    }

    [[nodiscard]] bool NteSkillsReflectionReady() const noexcept {
        return combat_skill_discovery.skill_layout_valid &&
            NteFunctionReady(NteFunctionKind::GetAbilitySystemComponent) &&
            combat_skill_discovery.gameplay_ability_class != 0;
    }

    [[nodiscard]] bool NteSkillInvocationReflectionReady() const noexcept {
        return NteSkillsReflectionReady() &&
            NteFunctionReady(NteFunctionKind::ActivateAbilityByClass) &&
            combat_skill_discovery.gameplay_ability_class != 0;
    }

    // The streaming source is what the world streams around, so redirecting it is how a
    // destination is loaded before anything is moved there. The framework owns the single
    // hook; plugins only consume the published override service.
    [[nodiscard]] bool Ue5StreamingSourceAvailable() const noexcept {
        return resolution.FeatureAvailable("ue5.streaming-source") &&
            resolution.FeatureAvailable("nte.player") &&
            LayoutKeysAvailable(profile, {
                "world.gameInstance",
                "gameInstance.localPlayers",
                "localPlayer.controller",
                "controller.streamingSourceVtableOffset"}) &&
            FeatureDeclaresDependency(
                profile, "ue5.streaming-source", "nte.player") &&
            FeatureDeclaresLayoutValidator(
                profile, "ue5.streaming-source", "ue5-streaming-source-layout-v1");
    }

    [[nodiscard]] bool NtePlayerTeleportAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        return static_cast<bool>(process_event_invoker) &&
            process_event != nullptr && process_event->Available() &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.player-teleport") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
            "object.class",
            "object.nameOffset",
            "object.outer",
            "ustruct.propertyLink",
            "ufunction.numParms",
            "ufunction.parmsSize",
            "ufunction.returnValueOffset",
            "ffield.name",
            "fproperty.arrayDim",
            "fproperty.elementSize",
            "fproperty.offsetInternal",
            "fproperty.propertyLinkNext",
            "fstructProperty.struct",
            "fboolProperty.fieldSize",
            "fboolProperty.byteOffset",
            "fboolProperty.byteMask",
            "fboolProperty.fieldMask"}) &&
            FeatureDeclaresSymbol(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventAbiValidator) &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "nte.player") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "ue5.names") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile,
                "nte.player-teleport",
                "nte-player-teleport-layout-v1");
    }

    [[nodiscard]] bool NteMapLandmarksAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        return static_cast<bool>(process_event_invoker) && ObjectFindAvailable() &&
            process_event != nullptr && process_event->Available() &&
            resolution.FeatureAvailable("nte.map-landmarks") &&
            SemanticFeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) && LayoutKeysAvailable(profile, {
                "object.internalIndex",
                "object.class",
                "object.nameOffset",
                "object.outer",
                "controller.playerState",
                "uclass.classDefaultObject",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "gameData.teleportPointDataTable",
                "dataTable.rowStruct",
                "dataTable.rowMap",
                "dataTable.rowMapData",
                "dataTable.rowMapNum",
                "dataTable.rowMapNumFree",
                "dataTable.rowMapMax",
                "dataTable.rowMapElementStride",
                "dataTable.rowMapRowOffset",
                "dataTable.rowMapInlineFlags",
                "dataTable.rowMapFlagsData",
                "dataTable.rowMapFlagsNum",
                "dataTable.rowMapFlagsMax",
                "dataTable.maxRows",
                "teleportPoint.belongsLevel",
                "teleportPoint.floor",
                "teleportPoint.transformTranslation",
                "teleportPoint.type",
                "teleportPoint.canTeleport",
                "teleportPoint.overrideTransform",
                "teleportPoint.overrideTranslation"}) &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.object-find") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.map-landmarks", "nte-map-landmarks-layout-v1");
    }

    [[nodiscard]] bool NteNavigationReflectionAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        const auto* const input_policy = resolution.FindSymbol(
            "nte.ClientIgnoreGameAndUiInput");
        return static_cast<bool>(process_event_invoker) &&
            process_event != nullptr && process_event->Available() &&
            input_policy != nullptr && input_policy->Available() &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.navigation") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.class",
                "object.nameOffset",
                "object.outer",
                "uclass.classDefaultObject",
                "ustruct.superStruct",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name",
                "ffieldClass.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fstructProperty.struct",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask",
                "controller.controlRotation",
                "controller.getPlayerCharacterVtableOffset",
                "character.setCustomIgnoreMoveInputVtableOffset",
                "character.setCustomLimitInputVtableOffset"}) &&
            FeatureDeclaresSymbol(
                profile, "nte.navigation", "nte.ClientIgnoreGameAndUiInput") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.navigation", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.navigation", "nte-navigation-layout-v1") &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.navigation", "nte-navigation-input-abi-v1");
    }

    [[nodiscard]] bool NteNavigationAvailable() const noexcept {
        return NteNavigationReflectionAvailable() && navigation_input_policy != nullptr &&
            navigation_input_policy->Started();
    }

    [[nodiscard]] bool NteUiButtonsAvailable() const noexcept {
        if (ui_buttons == nullptr || !process_event_invoker || !ObjectFindAvailable() ||
            !resolution.FeatureAvailable("nte.ui-buttons") ||
            !resolution.FeatureAvailable("ue5.names") ||
            !resolution.FeatureAvailable("ue5.objects") ||
            !resolution.FeatureAvailable(kUe5ProcessEventFeature)) {
            return false;
        }
        for (const std::string_view key : NteUiButtonsLayoutKeys()) {
            if (Layout(profile, key) < 0) return false;
        }
        return FeatureDeclaresDependency(profile, "nte.ui-buttons", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.ui-buttons", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.ui-buttons", kUe5ObjectFindFeature) &&
            FeatureDeclaresDependency(profile, "nte.ui-buttons", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.ui-buttons", kNteUiButtonsLayoutValidator);
    }

    [[nodiscard]] bool NtePickupAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol(kUe5ProcessEventSymbol);
        return static_cast<bool>(process_event_invoker) && process_event != nullptr &&
            process_event->Available() && resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.pickup") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("nte.entities") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && NteEntityReflectionLayoutAvailable() &&
            LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.nameOffset", "object.outer",
                "ustruct.superStruct", "ustruct.children", "ufield.next",
                "ufunction.flags", "ufunction.nativeFlag", "ufunction.numParms",
                "ufunction.parmsSize", "pickup.actor.interactFinish",
                "pickup.trigger.numParms", "pickup.trigger.parmsSize",
                "pickup.trigger.actor", "pickup.trigger.index",
                "pickup.trigger.onlyClientSide", "pickup.canTry.controller",
                "pickup.canTry.numParms", "pickup.canTry.parmsSize",
                "pickup.canTry.index", "pickup.canTry.returnValue",
                "pickup.entries.numParms", "pickup.entries.parmsSize",
                "pickup.entries.controller", "pickup.entries.array",
                "pickup.entries.maximumChoices",
                "pickup.interactEntryStride", "pickup.interactEntryIndex"}) &&
            FeatureDeclaresSymbol(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventAbiValidator) &&
            FeatureDeclaresDependency(profile, "nte.pickup", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "nte.entities") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.pickup", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.pickup", "nte-pickup-layout-v1");
    }

    [[nodiscard]] bool EnsureNavigationInputPolicyLocked() noexcept {
        if (navigation_input_policy == nullptr) return false;
        if (navigation_input_policy->Started()) return true;
        if (!NteNavigationReflectionAvailable()) return false;
        const auto* const target = resolution.FindSymbol("nte.ClientIgnoreGameAndUiInput");
        if (target == nullptr || !target->Available()) return false;
        return navigation_input_policy->Start(reinterpret_cast<void*>(target->address));
    }

    [[nodiscard]] bool ObjectFindAvailable() const noexcept {
        return static_cast<bool>(object_lookup) &&
            resolution.FeatureAvailable(kUe5ObjectFindFeature) &&
            resolution.FeatureAvailable("ue5.objects") &&
            FeatureDeclaresSymbol(
                profile, kUe5ObjectFindFeature, kUe5StaticFindObjectSymbol) &&
            FeatureDeclaresDependency(
                profile, kUe5ObjectFindFeature, "ue5.objects") &&
            FeatureDeclaresLayoutValidator(
                profile,
                kUe5ObjectFindFeature,
                kUe5StaticFindObjectAbiValidator);
    }

    [[nodiscard]] bool AhudFeatureAvailable() const noexcept {
        return framework_hook_ready && ahud_hook_ready &&
            resolution.FeatureAvailable("ue5.ahud") &&
            resolution.FeatureAvailable("ue5.functions") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            LayoutKeysAvailable(profile, {
                "object.class",
                "object.nameOffset",
                "object.outer",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name",
                "ffieldClass.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fstructProperty.struct",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask"}) &&
            FeatureDeclaresDependency(profile, "ue5.ahud", "ue5.functions") &&
            FeatureDeclaresDependency(
                profile, "ue5.ahud", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "ue5.ahud", "ue5-ahud-reflection-v1");
    }

    [[nodiscard]] bool NteEntitiesLayoutAvailable() const noexcept {
        return LayoutKeysAvailable(profile, {
            "world.persistentLevel",
            "level.actors",
            "actor.rootComponent",
            "sceneComponent.boundsOrigin",
            "sceneComponent.boundsExtent"});
    }

    [[nodiscard]] bool NteEntityReflectionLayoutAvailable() const noexcept {
        return NteEntitiesLayoutAvailable() &&
            resolution.FeatureAvailable("ue5.names") && LayoutKeysAvailable(profile, {
                "object.class",
                "ustruct.propertyLink",
                "ffield.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask"});
    }

    [[nodiscard]] bool NteActorsLayoutAvailable() const noexcept {
        return NteEntityReflectionLayoutAvailable() &&
            SemanticFeatureAvailable("nte.entities") && LayoutKeysAvailable(profile, {
                "world.levels",
                "entities.maxLevels"});
    }

    [[nodiscard]] bool SemanticFeatureAvailable(std::string_view feature) const noexcept {
        if (!SemanticServicesAvailable() || !resolution.FeatureAvailable(feature)) return false;
        if (feature == "nte.player") return NtePlayerLayoutAvailable();
        if (feature == "nte.player-esp") {
            return resolution.FeatureAvailable("nte.player") && NtePlayerEspLayoutAvailable();
        }
        if (feature == "nte.player-teleport") {
            return resolution.FeatureAvailable("nte.player") &&
                NtePlayerTeleportAvailable();
        }
        if (feature == "ue5.streaming-source") return Ue5StreamingSourceAvailable();
        if (feature == "nte.map-landmarks") return NteMapLandmarksAvailable();
        if (feature == "nte.navigation") return NteNavigationAvailable();
        if (feature == "nte.vehicle") return NteVehicleProfileAvailable();
        if (feature == "nte.pickup") return NtePickupAvailable();
        if (feature == "nte.ui-buttons") return NteUiButtonsAvailable();
        if (feature == "nte.entities") {
            return NteEntitiesLayoutAvailable();
        }
        if (feature == "nte.combat") {
            return NteCombatProfileAvailable() && NteCombatReflectionReady();
        }
        if (feature == "nte.skills") {
            return NteSkillsProfileAvailable() && NteSkillsReflectionReady();
        }
        if (feature == "nte.skill-invocation") {
            return NteSkillInvocationProfileAvailable() &&
                NteSkillInvocationReflectionReady();
        }
        return feature == "nte.session";
    }

    [[nodiscard]] bool MetricsFeatureAvailable() const noexcept {
        return SemanticFeatureAvailable("nte.session") ||
            SemanticFeatureAvailable("nte.player") ||
            SemanticFeatureAvailable("nte.entities") ||
            SemanticFeatureAvailable("nte.combat") ||
            SemanticFeatureAvailable("nte.skills");
    }

    bool PublishAvailableServices(const std::weak_ptr<State>& self);

    void RevokePublishedFrom(std::size_t first) noexcept {
        static_cast<void>(RevokePublishedFromUntil(
            first, std::chrono::steady_clock::time_point::max()));
    }

    [[nodiscard]] bool RevokePublishedFromUntil(
        std::size_t first,
        std::chrono::steady_clock::time_point deadline) noexcept {
        {
            std::unique_lock publication_lock(publication_mutex, std::defer_lock);
            if (!LockUntil(publication_lock, deadline)) return false;
            const std::size_t begin = (std::min)(first, published.size());
            try {
                pending_revocations.reserve(
                    pending_revocations.size() + published.size() - begin);
            } catch (...) {
                return false;
            }
            for (std::size_t index = begin; index < published.size(); ++index) {
                pending_revocations.emplace_back(std::move(published[index]));
            }
            published.resize(begin);
        }
        return RevokePendingUntil(deadline);
    }

    [[nodiscard]] bool RevokePendingUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            std::string_view id;
            const void* table{};
            {
                std::unique_lock publication_lock(publication_mutex, std::defer_lock);
                if (!LockUntil(publication_lock, deadline)) return false;
                // The in-flight entry owns the string behind id. Exactly one
                // revoker may borrow it until that owner has finalized the
                // registry call, otherwise another Stop could move it away.
                if (revocation_call_active.load(std::memory_order_acquire)) return false;
                if (!revocation_in_flight) {
                    if (pending_revocations.empty()) return true;
                    revocation_in_flight.emplace(std::move(pending_revocations.back()));
                    pending_revocations.pop_back();
                }
                revocation_call_active.store(true, std::memory_order_release);
                id = revocation_in_flight->first;
                table = revocation_in_flight->second;
            }

            const auto result = services->RevokeUntil(id, table, deadline);
            std::optional<std::pair<std::string, const void*>> completed;
            {
                std::unique_lock publication_lock(publication_mutex, std::defer_lock);
                if (!LockUntil(publication_lock, deadline)) {
                    revocation_call_active.store(false, std::memory_order_release);
                    return false;
                }
                if (result == AdapterServiceRegistry::RevokeResult::TimedOut) {
                    revocation_call_active.store(false, std::memory_order_release);
                    return false;
                }
                if (revocation_in_flight &&
                    revocation_in_flight->first == id &&
                    revocation_in_flight->second == table) {
                    completed.emplace(std::move(*revocation_in_flight));
                    revocation_in_flight.reset();
                }
                revocation_call_active.store(false, std::memory_order_release);
            }
            // The retired identifier is released after the publication lock.
        }
    }

    [[nodiscard]] bool ServiceAvailableForPublication(std::string_view id) const noexcept {
        if (id == ANOMALY_UE5_BUILD_SERVICE_V1_ID || id == ANOMALY_NTE_BUILD_SERVICE_V1_ID) {
            return true;
        }
        if (id == ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.framework");
        }
        if (id == ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID) {
            return framework_hook_ready && process_event_hook_ready &&
                resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
                process_event_endpoint.load(std::memory_order_acquire) != nullptr;
        }
        if (id == ANOMALY_UE5_AHUD_SERVICE_V1_ID) {
            return AhudFeatureAvailable();
        }
        if (id == ANOMALY_UE5_NAMES_SERVICE_V1_ID) {
            return resolution.FeatureAvailable("ue5.names");
        }
        if (id == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.objects");
        }
        if (id == ANOMALY_UE5_WORLD_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.world");
        }
        if (id == ANOMALY_NTE_SESSION_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.session");
        }
        if (id == ANOMALY_NTE_METRICS_SERVICE_V1_ID) {
            return framework_hook_ready && MetricsFeatureAvailable();
        }
        if (id == ANOMALY_NTE_PLAYER_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.player");
        }
        if (id == ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID) {
            return framework_hook_ready &&
                SemanticFeatureAvailable("nte.player-teleport");
        }
        if (id == ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.map-landmarks");
        }
        if (id == ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.navigation");
        }
        if (id == ANOMALY_NTE_VEHICLE_SERVICE_V1_ID) {
            return framework_hook_ready && NteVehicleProfileAvailable();
        }
        if (id == ANOMALY_NTE_PICKUP_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.pickup");
        }
        if (id == ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.ui-buttons");
        }
        if (id == ANOMALY_NTE_ENTITIES_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.entities");
        }
        if (id == ANOMALY_NTE_ACTORS_SERVICE_V1_ID) {
            return framework_hook_ready && NteActorsLayoutAvailable();
        }
        if (id == ANOMALY_NTE_COMBAT_SERVICE_V1_ID) {
            return framework_hook_ready && NteCombatProfileAvailable();
        }
        if (id == ANOMALY_NTE_SKILLS_SERVICE_V1_ID) {
            return framework_hook_ready && NteSkillsProfileAvailable();
        }
        if (id == ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID) {
            return framework_hook_ready &&
                SemanticFeatureAvailable("nte.skill-invocation");
        }
        return false;
    }

    void RevokePublishedUnavailable() {
        {
            std::scoped_lock publication_lock(publication_mutex);
            const auto unavailable = std::count_if(
                published.begin(), published.end(), [this](const auto& service) {
                    return !ServiceAvailableForPublication(service.first);
                });
            pending_revocations.reserve(
                pending_revocations.size() + static_cast<std::size_t>(unavailable));
            auto service = published.begin();
            while (service != published.end()) {
                if (ServiceAvailableForPublication(service->first)) {
                    ++service;
                    continue;
                }
                pending_revocations.emplace_back(std::move(*service));
                service = published.erase(service);
            }
        }
        static_cast<void>(RevokePendingUntil(std::chrono::steady_clock::time_point::max()));
    }

    void RefreshDeferredResolution(
        std::uint64_t sequence,
        const std::shared_ptr<State>& self) noexcept {
        constexpr std::uint64_t kRetryIntervalTicks = 60;
        if (!started.load(std::memory_order_acquire) || !framework_hook_ready ||
            sequence < deferred_resolution_retry_sequence) {
            return;
        }
        deferred_resolution_retry_sequence = sequence + kRetryIntervalTicks;
        try {
            ProfileResolutionSnapshot refreshed = resolution;
            SymbolResolver resolver(
                memory, {}, {}, feature_layout_validators);
            if (!resolver.RevalidateDeferredCandidates(profile, refreshed)) return;

            if (!started.load(std::memory_order_acquire)) return;
            const auto first_new_service = PublishedCount();
            const ProfileResolutionSnapshot previous = std::move(resolution);
            resolution = std::move(refreshed);
            static_cast<void>(EnsureNavigationInputPolicyLocked());
            if (!PublishAvailableServices(self)) {
                RevokePublishedFrom(first_new_service);
                resolution = previous;
            } else {
                RevokePublishedUnavailable();
            }
        } catch (...) {
        }
    }

    std::uint32_t FeatureState(AnomalyStringViewV1 id) const noexcept {
        if (id.data == nullptr) return ANOMALY_FEATURE_V1_UNAVAILABLE;
        std::scoped_lock lock(mutex);
        const std::string_view feature(id.data, id.size);
        if (feature == "nte.metrics") {
            return MetricsFeatureAvailable()
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        if (feature == "ue5.ahud") {
            return AhudFeatureAvailable()
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        if (feature == "nte.session" || feature == "nte.player" ||
            feature == "nte.player-esp" ||
            feature == "nte.player-teleport" || feature == "nte.navigation" ||
            feature == "nte.vehicle" || feature == "nte.pickup" || feature == "nte.ui-buttons" ||
            feature == "nte.entities" ||
            feature == "nte.combat" || feature == "nte.skills" ||
            feature == "nte.skill-invocation") {
            return SemanticFeatureAvailable(feature)
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        return resolution.FeatureAvailable(feature)
            ? ANOMALY_FEATURE_V1_AVAILABLE
            : ANOMALY_FEATURE_V1_UNAVAILABLE;
    }

    static bool SamplingDue(
        std::uint64_t sequence,
        std::uint64_t previous_attempt,
        std::uint32_t interval) noexcept {
        return previous_attempt == 0 || sequence - previous_attempt >= interval;
    }

    void InvalidateEntities() noexcept {
        if (entity_frame_cache || previous_entity_frame_cache) ++entity_generation;
        entity_frame_cache.reset();
        previous_entity_frame_cache.reset();
    }

    void InvalidateActors() noexcept {
        if (actor_frame_cache) ++actor_generation;
        actor_frame_cache.reset();
        actor_world_generation = 0;
        actor_attempt_sequence = 0;
    }

    void InvalidateCombatSnapshot() noexcept {
        combat_sample_sequence = 0;
        combat_character = {};
        combat_target = {};
        combat_hp = 0.0;
        combat_max_hp = 0.0;
        combat_shield = 0.0;
        combat_dead = false;
        combat_available = false;
        combat_partial = false;
    }

    void ResetDamageEvents() noexcept {
        if (damage_event_sequence != 0) ++damage_event_sequence;
        damage_world_sequence_base = damage_event_sequence;
        damage_event_start = 0;
        damage_event_count = 0;
        damage_events = {};
        damage_dropped_count = 0;
        damage_next_source_id = kDamageSourceBase;
        damage_source_object_ids.clear();
        damage_source_objects.clear();
        damage_source_names.clear();
        damage_source_ability_classes.clear();
        pending_damage_source_mappings.clear();
        observed_damage_source_mappings.clear();
        damage_critical_tag_cache.clear();
        damage_participant_paths.clear();
        if (combat_event_sequence != 0) ++combat_event_sequence;
        combat_event_start = 0;
        combat_event_count = 0;
        combat_events = {};
        combat_event_names.clear();
        combat_event_objects.clear();
        pending_combat_event_names.clear();
        queued_combat_event_names.clear();
        failed_combat_event_names.clear();
        combat_participant_names.clear();
        pending_combat_participant_names.clear();
        queued_combat_participant_names.clear();
        failed_combat_participant_names.clear();
        localized_names_by_fname.clear();
        localized_names_by_key.clear();
        display_table_generation = 0;
        display_table_scan_complete = false;
        display_table_loaded_mask = 0;
        display_game_data = 0;
        display_game_data_generation = 0;
        display_table_indexes = {};
        damage_skill_index = {};
        active_effects.clear();
        active_effect_ability_system = 0;
        active_effect_array_key = 0;
        active_effects_initialized = false;
        combat_capture_read.store(0, std::memory_order_release);
        combat_capture_write.store(0, std::memory_order_release);
        display_name_demand.store(false, std::memory_order_release);
    }

    void InvalidateSkills() noexcept {
        if (skills_available || !skills.empty()) ++skill_generation;
        skills.clear();
        ability_display_names.clear();
        ability_display_name_attempts.clear();
        skill_sample_sequence = 0;
        skill_ability_system = 0;
        skill_character = {};
        skills_available = false;
        skills_partial = false;
    }

    void InvalidateCombatSkillDiscoveryLocked() noexcept {
        combat_skill_discovery = {};
        combat_skill_discovery.object_generation = object_generation;
        combat_capture_bindings.store(nullptr, std::memory_order_release);
        InvalidateCombatSnapshot();
        InvalidateSkills();
    }

    void InvalidatePlayer() noexcept {
        const bool had_identity = player_pawn != 0 || player_controller != 0 || player_root != 0 ||
            player_available || player_esp_available;
        if (had_identity) ++player_generation;
        player_pawn = 0;
        player_controller = 0;
        player_root = 0;
        player_sample_sequence = 0;
        player_position = {};
        player_bounds_center = {};
        player_bounds_extent = {};
        camera_position = {};
        camera_rotation = {};
        camera_horizontal_fov = 0.0F;
        player_available = false;
        player_esp_available = false;
        player_partial = false;
    }

    void InvalidateAhudBindingLocked() noexcept {
        ahud_binding.store({}, std::memory_order_release);
        ahud_discovery = {};
        ahud_discovery.object_generation = object_generation;
    }

    void InvalidatePickupLocked(const std::uint32_t status) noexcept {
        pickup_demand.store(false, std::memory_order_release);
        pickup_request = {};
        pickup_confirmation = {};
        pickup_snapshot = {sizeof(pickup_snapshot)};
        pickup_snapshot.sequence = pickup_sequence;
        pickup_snapshot.state = pickup_sequence == 0
            ? ANOMALY_NTE_PICKUP_V1_IDLE
            : ANOMALY_NTE_PICKUP_V1_COMPLETE;
        pickup_snapshot.status = status;
        if (pickup_sequence != 0) pickup_snapshot.flags = ANOMALY_NTE_PICKUP_V1_VALID;
    }

    void ResetForStartLocked() noexcept {
        session_event_start = 0;
        session_event_count = 0;
        // Keep session cursors monotonic across Host lifecycles. Reserving one
        // value before a restarted stream makes every prior non-zero cursor
        // older than the new retained range, even after a new event is added.
        if (session_event_sequence != 0) ++session_event_sequence;
        session_events = {};
        if (world_pointer != 0) ++world_generation;
        world_pointer = 0;
        world_name_id = 0;
        world_name_layout_available = false;
        world_name_readable = false;
        if (object_registry.items != 0) ++object_generation;
        object_registry = {};
        teleport = {};
        map_landmark_binding = {};
        map_landmark_catalog.reset();
        map_landmark_next_refresh_sequence = 0;
        navigation = {};
        pickup_sequence = 0;
        InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        if (ui_buttons != nullptr) ui_buttons->Invalidate(ANOMALY_STATUS_V1_UNAVAILABLE);
        InvalidateAhudBindingLocked();
        InvalidateCombatSkillDiscoveryLocked();
        ResetDamageEvents();
        damage_native_call_count = 0;
        reflection_fault_count = 0;
        last_reflection_fault_function = 0;
        last_reflection_fault_code = 0;
        damage_captured_event_count = 0;
        damage_capture_drop_count = 0;
        damage_attacker_resolution_failure_count = 0;
        damage_victim_resolution_failure_count = 0;
        damage_source_resolution_failure_count = 0;
        saved_trigger_skill_mapping_count = 0;
        trigger_ability_handle_mapping_count = 0;
        damage_source_mapping_failure_count = 0;
        delayed_damage_name_completion_count = 0;
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        entity_attempt_sequence = 0;
        player_attempt_sequence = 0;
        combat_attempt_sequence = 0;
        skill_attempt_sequence = 0;
        player_demand.store(false, std::memory_order_release);
        entity_demand.store(false, std::memory_order_release);
        navigation_demand.store(false, std::memory_order_release);
        pickup_demand.store(false, std::memory_order_release);
        ahud_demand.store(false, std::memory_order_release);
        combat_demand.store(false, std::memory_order_release);
        skill_demand.store(false, std::memory_order_release);
        combat_capture_read.store(0, std::memory_order_release);
        combat_capture_write.store(0, std::memory_order_release);
        combat_capture_drop_count.store(0, std::memory_order_release);
        ahud_frame_count.store(0, std::memory_order_release);
        ahud_process_event_call_count.store(0, std::memory_order_release);
        game_thread_id.store(0, std::memory_order_release);
        tick_sequence.store(0, std::memory_order_release);
        rejected_thread_ticks.store(0, std::memory_order_release);
        snapshot_tick_count = 0;
        latest_snapshot_cost_micros = 0;
        total_snapshot_cost_micros = 0;
        max_snapshot_cost_micros = 0;
        player_refresh_count = 0;
        player_cache_hit_count = 0;
        entity_refresh_count = 0;
        entity_cache_hit_count = 0;
        entity_page_request_count = 0;
        entity_page_cache_hit_count = 0;
        deferred_resolution_retry_sequence = 1;
    }

    void ClearSemanticStateForStopLocked() noexcept {
        player_demand.store(false, std::memory_order_release);
        entity_demand.store(false, std::memory_order_release);
        navigation_demand.store(false, std::memory_order_release);
        InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        if (ui_buttons != nullptr) ui_buttons->Invalidate(ANOMALY_STATUS_V1_UNAVAILABLE);
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        InvalidateCombatSnapshot();
        InvalidateSkills();
        ResetDamageEvents();
        if (world_pointer != 0) {
            world_pointer = 0;
            ++world_generation;
            ++world_change_sequence;
        }
        world_name_id = 0;
        world_name_layout_available = false;
        world_name_readable = false;
        teleport = {};
        arrival_hold = {};
        ReleaseMovementHoldForStopLocked();
        RemoveStreamingOverride();
        map_landmark_binding = {};
        map_landmark_catalog.reset();
        map_landmark_next_refresh_sequence = 0;
        navigation = {};
        framework_hook_ready = false;
        ahud_hook_ready = false;
        process_event_hook_ready = false;
        InvalidateAhudBindingLocked();
        InvalidateCombatSkillDiscoveryLocked();
    }

    void RecordSessionEvent(
        const std::uint32_t kind,
        const std::uint64_t tick,
        const AnomalyGenerationHandleV1 previous_world,
        const AnomalyGenerationHandleV1 world) noexcept {
        const SessionEvent event{kind, ++session_event_sequence, tick, previous_world, world};
        if (session_event_count == kSessionEventCapacity) {
            session_events[session_event_start] = event;
            session_event_start = (session_event_start + 1U) % kSessionEventCapacity;
            return;
        }
        const std::size_t slot =
            (session_event_start + session_event_count) % kSessionEventCapacity;
        session_events[slot] = event;
        ++session_event_count;
    }

    void RefreshWorld(const std::uint64_t tick) noexcept {
        const auto* world_symbol = Symbol("ue5.GWorld");
        std::uintptr_t next{};
        if (world_symbol != nullptr && world_symbol->Available()) {
            static_cast<void>(ReadValue(*memory, world_symbol->address, next));
        }
        if (next != world_pointer) {
            const AnomalyGenerationHandleV1 previous_world = world_pointer == 0
                ? AnomalyGenerationHandleV1{}
                : AnomalyGenerationHandleV1{1, world_generation};
            InvalidatePlayer();
            InvalidateEntities();
            InvalidateActors();
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            InvalidateCombatSnapshot();
            InvalidateSkills();
            ResetDamageEvents();
            world_pointer = next;
            ++world_generation;
            ++world_change_sequence;
            const AnomalyGenerationHandleV1 world = world_pointer == 0
                ? AnomalyGenerationHandleV1{}
                : AnomalyGenerationHandleV1{1, world_generation};
            const std::uint32_t event_kind = previous_world.id == 0
                ? ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY
                : world.id == 0
                    ? ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE
                    : ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED;
            RecordSessionEvent(event_kind, tick, previous_world, world);
        }
        world_name_id = 0;
        auto name_offset = Layout(profile, "world.nameOffset");
        if (name_offset < 0) name_offset = Layout(profile, "object.nameOffset");
        world_name_layout_available = name_offset >= 0;
        world_name_readable = false;
        std::uintptr_t address{};
        if (world_pointer != 0 && world_name_layout_available &&
            AddAddress(world_pointer, name_offset, address)) {
            world_name_readable = ReadValue(*memory, address, world_name_id);
        }
    }

    void RefreshObjects() noexcept {
        const std::uint64_t previous_generation = object_generation;
        const auto* objects = Symbol("ue5.GObjects");
        ObjectRegistryState next;
        const bool available = objects != nullptr && objects->Available() &&
            LoadObjectRegistry(profile, *memory, objects->address, next);
        if (!available) {
            if (object_registry.items != 0) ++object_generation;
            object_registry = {};
            teleport = {};
            map_landmark_binding = {};
            map_landmark_catalog.reset();
            map_landmark_next_refresh_sequence = 0;
            navigation = {};
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            if (object_generation != previous_generation) {
                InvalidateAhudBindingLocked();
                InvalidateCombatSkillDiscoveryLocked();
                ResetDamageEvents();
            }
            return;
        }
        if (object_registry.items == 0 || next.items != object_registry.items ||
            next.count < object_registry.count || next.max_count != object_registry.max_count ||
            next.max_chunks != object_registry.max_chunks ||
            next.num_chunks < object_registry.num_chunks ||
            next.chunk_signature != object_registry.chunk_signature) {
            ++object_generation;
            teleport = {};
            map_landmark_binding = {};
            map_landmark_catalog.reset();
            map_landmark_next_refresh_sequence = 0;
            navigation = {};
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        }
        object_registry = next;
        if (object_generation != previous_generation) {
            InvalidateAhudBindingLocked();
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            InvalidateCombatSkillDiscoveryLocked();
            ResetDamageEvents();
        }
    }

    struct PlayerLocationSample {
        std::uintptr_t controller{};
        std::uintptr_t pawn{};
        std::uintptr_t root{};
        std::uintptr_t location{};
        std::array<double, 3> position{};
    };

    [[nodiscard]] bool ReadCurrentPlayerLocation(PlayerLocationSample& sample) const noexcept {
        if (!SemanticFeatureAvailable("nte.player") || world_pointer == 0) return false;
        std::uintptr_t game_instance{};
        std::uintptr_t players_array{};
        std::int32_t players_count{};
        std::uintptr_t local_player{};
        if (!ReadPointerAt(*memory, world_pointer, Layout(profile, "world.gameInstance"), game_instance) ||
            !ReadPointerAt(*memory, game_instance, Layout(profile, "gameInstance.localPlayers"), players_array)) {
            return false;
        }
        std::uintptr_t count_address{};
        std::int64_t local_players_count_offset{};
        if (!AddLayoutOffset(
                Layout(profile, "gameInstance.localPlayers"),
                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                local_players_count_offset) ||
            !AddAddress(game_instance, local_players_count_offset, count_address) ||
            !ReadValue(*memory, count_address, players_count) || players_count < 1 ||
            !ReadValue(*memory, players_array, local_player) || local_player == 0 ||
            !ReadPointerAt(
                *memory, local_player, Layout(profile, "localPlayer.controller"), sample.controller) ||
            !ReadPointerAt(*memory, sample.controller, Layout(profile, "controller.pawn"), sample.pawn) ||
            !ReadPointerAt(*memory, sample.pawn, Layout(profile, "actor.rootComponent"), sample.root)) {
            return false;
        }
        if (!AddAddress(sample.root, Layout(profile, "sceneComponent.location"), sample.location) ||
            !memory->Read(sample.location, sample.position.data(), sizeof(sample.position)) ||
            !std::ranges::all_of(
                sample.position, [](double value) { return std::isfinite(value); })) {
            return false;
        }
        return true;
    }

    void RefreshPlayer(std::uint64_t sequence) noexcept {
        player_attempt_sequence = sequence;
        PlayerLocationSample sample;
        if (!ReadCurrentPlayerLocation(sample)) {
            InvalidatePlayer();
            return;
        }
        if (sample.pawn != player_pawn || sample.controller != player_controller ||
            sample.root != player_root) {
            player_pawn = sample.pawn;
            player_controller = sample.controller;
            player_root = sample.root;
            ++player_generation;
        }
        player_position = sample.position;
        player_available = true;
        player_esp_available = false;
        player_partial = false;
        player_sample_sequence = sequence;
        player_bounds_center = {};
        player_bounds_extent = {};
        camera_position = {};
        camera_rotation = {};
        camera_horizontal_fov = 0.0F;

        std::uintptr_t camera_manager{};
        std::uintptr_t bounds_center_address{};
        std::uintptr_t bounds_extent_address{};
        std::uintptr_t camera_position_address{};
        std::uintptr_t camera_rotation_address{};
        std::uintptr_t camera_fov_address{};
        std::array<double, 3> bounds_center{};
        std::array<double, 3> bounds_extent{};
        std::array<double, 3> next_camera_position{};
        std::array<double, 3> next_camera_rotation{};
        float horizontal_fov{};
        if (!SemanticFeatureAvailable("nte.player-esp") ||
            !ReadPointerAt(
                *memory, sample.controller, Layout(profile, "controller.cameraManager"), camera_manager) ||
            !AddAddress(sample.root, Layout(profile, "sceneComponent.boundsOrigin"), bounds_center_address) ||
            !AddAddress(sample.root, Layout(profile, "sceneComponent.boundsExtent"), bounds_extent_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.location"), camera_position_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.rotation"), camera_rotation_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.fov"), camera_fov_address) ||
            !memory->Read(bounds_center_address, bounds_center.data(), sizeof(bounds_center)) ||
            !memory->Read(bounds_extent_address, bounds_extent.data(), sizeof(bounds_extent)) ||
            !memory->Read(
                camera_position_address, next_camera_position.data(), sizeof(next_camera_position)) ||
            !memory->Read(
                camera_rotation_address, next_camera_rotation.data(), sizeof(next_camera_rotation)) ||
            !ReadValue(*memory, camera_fov_address, horizontal_fov)) {
            player_partial = SemanticFeatureAvailable("nte.player-esp");
            return;
        }
        const auto finite = [](const std::array<double, 3>& values) {
            return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
        };
        if (!finite(bounds_center) || !finite(bounds_extent) || !finite(next_camera_position) ||
            !finite(next_camera_rotation) || !std::isfinite(horizontal_fov) ||
            std::ranges::any_of(bounds_extent, [](double value) { return value <= 0.0; }) ||
            horizontal_fov <= 5.0F || horizontal_fov >= 175.0F) {
            player_partial = true;
            return;
        }
        player_bounds_center = bounds_center;
        player_bounds_extent = bounds_extent;
        camera_position = next_camera_position;
        camera_rotation = next_camera_rotation;
        camera_horizontal_fov = horizontal_fov;
        player_esp_available = true;
    }

    [[nodiscard]] bool InvokeNteFunctionLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        std::span<std::uint8_t> parameters) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        return binding && object != 0 && process_event_invoker &&
            ReadableRange(*memory, object, 0x20U) &&
            ReadableRange(*memory, binding->function, 0x20U) &&
            parameters.size() >= binding->parms_size &&
            InvokeNativeProcessEventLocked(
                object, binding->function, parameters.data(), binding->parms_size);
    }

    [[nodiscard]] bool InvokeNativeProcessEventLocked(
        const std::uintptr_t receiver,
        const std::uintptr_t function,
        void* const parameters,
        const std::size_t parameter_size) const noexcept {
        std::uintptr_t flags_address{};
        std::uint32_t original_flags{};
        const auto native_flag = Layout(profile, "ufunction.nativeFlag", -1);
        if (native_flag <= 0 || !process_event_invoker ||
            !AddAddress(function, Layout(profile, "ufunction.flags"), flags_address) ||
            !ReadValue(*memory, flags_address, original_flags)) {
            return false;
        }
        const auto invocation_flags = original_flags |
            static_cast<std::uint32_t>(native_flag);
        if (!memory->Write(
                flags_address, &invocation_flags, sizeof(invocation_flags))) {
            return false;
        }
        std::uint32_t exception_code{};
        const bool invoked = InvokeProcessEventGuarded(
            process_event_invoker, receiver, function, parameters, parameter_size,
            &exception_code);
        if (exception_code != 0) {
            ++reflection_fault_count;
            last_reflection_fault_function = function;
            last_reflection_fault_code = exception_code;
        }
        const bool restored = memory->Write(
            flags_address, &original_flags, sizeof(original_flags));
        return invoked && restored;
    }

    template <typename Value>
    [[nodiscard]] bool InvokeNteReturnLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        Value& value) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        if (!binding || binding->parms_size > 64U) return false;
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!InvokeNteFunctionLocked(kind, object, parameters)) return false;
        const std::size_t return_index = NteSpec(kind).parameters.size() - 1U;
        const std::size_t offset = binding->offsets[return_index];
        if (offset > binding->parms_size || sizeof(Value) > binding->parms_size - offset) {
            return false;
        }
        std::memcpy(&value, parameters.data() + offset, sizeof(Value));
        return true;
    }

    [[nodiscard]] bool InvokeNteBoolReturnLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        bool& value) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        if (!binding || binding->parms_size > 64U) return false;
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!InvokeNteFunctionLocked(kind, object, parameters)) return false;
        const std::size_t index = NteSpec(kind).parameters.size() - 1U;
        const ReflectedBoolParameter& reflected = binding->bool_parameters[index];
        if (reflected.byte_offset >= binding->parms_size) return false;
        value = (parameters[reflected.byte_offset] & reflected.field_mask) != 0;
        return true;
    }

    [[nodiscard]] bool EnsureSceneMonsterTableIndexLocked() noexcept {
        if (scene_monster_table_generation == object_generation &&
            scene_monster_table_scan_complete) {
            return scene_monster_table_count != 0;
        }
        if (scene_monster_table_generation != object_generation) {
            scene_monster_table_generation = object_generation;
            scene_monster_table_count = 0;
            scene_monster_table_scan_complete = false;
            for (auto& index : scene_monster_table_indexes) {
                index = DisplayTableIndex{};
            }
        }
        if (world_pointer == 0) return false;
        const auto persistent_level_offset =
            Layout(profile, "world.persistentLevel", -1);
        const auto level_settings_offset =
            Layout(profile, "level.worldSettings", -1);
        const auto scene_asset_offset =
            Layout(profile, "worldSettings.htSceneSolelyDataAsset", -1);
        const auto array_offset =
            Layout(profile, "sceneSolelyDataAsset.monsterArrayDataTable", -1);
        const auto count_offset = Layout(
            profile, "sceneSolelyDataAsset.monsterArrayDataTableCount", -1);
        const auto text_offset = Layout(profile, "monsterData.textName", -1);
        if (persistent_level_offset < 0 || level_settings_offset < 0 ||
            scene_asset_offset < 0 || array_offset < 0 || count_offset < 0 ||
            text_offset < 0) {
            return false;
        }
        std::uintptr_t persistent_level{};
        std::uintptr_t world_settings{};
        std::uintptr_t scene_asset{};
        if (!ReadPointerAt(
                *memory, world_pointer, persistent_level_offset, persistent_level) ||
            !ReadPointerAt(
                *memory, persistent_level, level_settings_offset, world_settings) ||
            !ReadPointerAt(
                *memory, world_settings, scene_asset_offset, scene_asset)) {
            return false;
        }
        std::uintptr_t array_data{};
        std::int32_t count{};
        if (!ReadPointerAt(*memory, scene_asset, array_offset, array_data) ||
            !ReadValue(*memory,
                scene_asset + static_cast<std::uintptr_t>(count_offset), count) ||
            count <= 0 ||
            static_cast<std::size_t>(count) > scene_monster_table_indexes.size()) {
            return false;
        }
        std::size_t built{};
        for (std::int32_t index = 0; index < count; ++index) {
            std::uintptr_t table{};
            if (!ReadValue(*memory,
                    array_data +
                        static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t),
                    table) || table == 0) {
                continue;
            }
            if (BuildDisplayTableIndexLocked(
                    table, "TextName", text_offset, -1, true,
                    scene_monster_table_indexes[built])) {
                ++built;
            }
        }
        scene_monster_table_count = built;
        scene_monster_table_scan_complete = true;
        return built != 0;
    }

    [[nodiscard]] bool ResolveMonsterNameFromSceneTablesLocked(
        const std::uint64_t key,
        const std::string_view supplied_key_text,
        std::string& value,
        const bool base_only = false) noexcept {
        value.clear();
        if (!EnsureSceneMonsterTableIndexLocked()) return false;
        std::string normalized(supplied_key_text);
        if (normalized.starts_with("Default__")) {
            normalized.erase(0, std::string_view{"Default__"}.size());
        }
        if (normalized.ends_with("_C")) {
            normalized.resize(normalized.size() - 2U);
        }
        const auto identity = MonsterDisplayIdentity(normalized);
        const auto try_row = [this, &value](
                                 const DisplayTableIndex& index,
                                 const std::uintptr_t row) {
            std::uintptr_t ftext_address{};
            return row != 0 && index.text_offset >= 0 &&
                AddAddress(row, index.text_offset, ftext_address) &&
                ResolveFTextLocked(ftext_address, value) && !value.empty();
        };
        for (std::size_t index{}; !base_only && index < scene_monster_table_count; ++index) {
            const auto& table_index = scene_monster_table_indexes[index];
            if (table_index.rows_by_fname.empty()) continue;
            std::uintptr_t row{};
            if (key != 0) {
                const auto found = table_index.rows_by_fname.find(key);
                if (found != table_index.rows_by_fname.end()) row = found->second;
            }
            if (row == 0 && !normalized.empty()) {
                const auto found = table_index.rows_by_key.find(normalized);
                if (found != table_index.rows_by_key.end()) row = found->second;
            }
            if (try_row(table_index, row)) return true;
            if (!identity.empty()) {
                const auto found = table_index.rows_by_key.find(identity);
                if (found != table_index.rows_by_key.end() &&
                    try_row(table_index, found->second)) {
                    return true;
                }
                for (const auto& [candidate, candidate_row] :
                         table_index.rows_by_key) {
                    if (!MonsterIdentityMatches(candidate, identity)) continue;
                    if (try_row(table_index, candidate_row)) return true;
                }
            }
        }
        if (!base_only) return false;
        const auto base_identity = NteMonsterBaseIdentity(identity);
        if (!base_identity.empty() && base_identity != identity) {
            std::string fallback;
            std::unordered_set<std::uintptr_t> visited;
            for (std::size_t index{}; index < scene_monster_table_count; ++index) {
                const auto& table_index = scene_monster_table_indexes[index];
                for (const auto& [candidate, row] : table_index.rows_by_key) {
                    if (MonsterDisplayIdentity(candidate) != base_identity ||
                        !visited.insert(row).second || !try_row(table_index, row)) continue;
                    if (!MergeNteMonsterFallbackName(fallback, value)) {
                        value.clear();
                        return false;
                    }
                }
            }
            value = std::move(fallback);
            return !value.empty();
        }
        value.clear();
        return false;
    }

    [[nodiscard]] bool ResolveMonsterNameWithFallbackLocked(
        const std::uint64_t key, const std::string_view key_text, std::string& value) {
        if (ResolveMonsterNameFromSceneTablesLocked(key, key_text, value) ||
            ResolveActorMonsterNameLocked(key_text, value)) return true;
        const auto abyss_key = AbyssStringTableMonsterKey(key_text);
        if (!abyss_key.empty() && ResolveAbyssStringTableEntryLocked(abyss_key, value)) return true;
        // A specific text-table entry takes precedence over a broad scene alias.
        return ResolveMonsterNameFromSceneTablesLocked(key, key_text, value, true) ||
            ResolveActorMonsterNameLocked(key_text, value, true);
    }

    [[nodiscard]] bool ResolveMonsterStaticDataNameLocked(
        const std::uint64_t config_id_key, std::string& value) noexcept {
        value.clear();
        ++monster_static_data_resolution_calls;
        if (config_id_key != 0) {
            // GetDataTableRowFromName is a custom thunk with a wildcard inline
            // OutRow, not an output pointer. Use the validated table index.
            std::string config_text;
            static_cast<void>(ResolveFNameLocked(
                static_cast<std::uint32_t>(config_id_key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(config_id_key >> 32U), config_text));
            if (ResolveMonsterNameFromSceneTablesLocked(
                    config_id_key, config_text, value) &&
                !value.empty()) {
                ++monster_static_data_resolution_successes;
                return true;
            }
        }
        const auto& binding = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetMonsterStaticData)];
        constexpr std::uint16_t kMonsterStaticDataSize = 296;
        constexpr std::size_t kMonsterStaticDataParameterCapacity = 320;
        const auto text_offset = Layout(profile, "monsterData.textName", -1);
        if (!binding || binding->parms_size == 0 ||
            binding->parms_size > kMonsterStaticDataParameterCapacity ||
            config_id_key == 0 || world_pointer == 0 || !process_event_invoker ||
            text_offset < 0 ||
            text_offset + 16 > kMonsterStaticDataSize) {
            return false;
        }
        const auto world_offset = binding->offsets[0];
        const auto config_offset = binding->offsets[1];
        const auto output_offset = binding->offsets[2];
        if (world_offset > binding->parms_size ||
            sizeof(std::uintptr_t) > binding->parms_size - world_offset ||
            config_offset > binding->parms_size ||
            sizeof(std::uint64_t) > binding->parms_size - config_offset ||
            output_offset > binding->parms_size ||
            kMonsterStaticDataSize > binding->parms_size - output_offset) {
            return false;
        }
        const auto persistent_level_offset =
            Layout(profile, "world.persistentLevel", -1);
        const auto level_settings_offset =
            Layout(profile, "level.worldSettings", -1);
        const auto scene_asset_offset =
            Layout(profile, "worldSettings.htSceneSolelyDataAsset", -1);
        std::uintptr_t persistent_level{};
        std::uintptr_t world_settings{};
        std::uintptr_t receiver{};
        std::uintptr_t receiver_class{};
        if (persistent_level_offset < 0 || level_settings_offset < 0 ||
            scene_asset_offset < 0 ||
            !ReadPointerAt(
                *memory, world_pointer, persistent_level_offset, persistent_level) ||
            persistent_level == 0 ||
            !ReadPointerAt(
                *memory, persistent_level, level_settings_offset, world_settings) ||
            world_settings == 0 ||
            !ReadPointerAt(
                *memory, world_settings, scene_asset_offset, receiver) ||
            receiver == 0 ||
            !ReadPointerAt(
                *memory, receiver, Layout(profile, "object.class"), receiver_class) ||
            !IsClassDerivedFromLocked(receiver_class, binding->outer_class)) {
            return false;
        }
        alignas(std::uint64_t)
            std::array<std::uint8_t, kMonsterStaticDataParameterCapacity> parameters{};
        std::memcpy(parameters.data() + world_offset,
            &world_pointer, sizeof(world_pointer));
        const auto comparison_index = static_cast<std::uint32_t>(
            config_id_key & 0xFFFFFFFFU);
        const auto number = static_cast<std::uint32_t>(config_id_key >> 32U);
        std::memcpy(parameters.data() + config_offset,
            &comparison_index, sizeof(comparison_index));
        std::memcpy(parameters.data() + config_offset + sizeof(comparison_index),
            &number, sizeof(number));
        if (!InvokeNteFunctionLocked(
                NteFunctionKind::GetMonsterStaticData, receiver, parameters)) {
            return false;
        }
        const auto return_index =
            NteSpec(NteFunctionKind::GetMonsterStaticData).parameters.size() - 1U;
        const ReflectedBoolParameter& reflected = binding->bool_parameters[return_index];
        if (reflected.byte_offset >= binding->parms_size ||
            (parameters[reflected.byte_offset] & reflected.field_mask) == 0) {
            return false;
        }
        const auto ftext_offset =
            static_cast<std::size_t>(output_offset) + static_cast<std::size_t>(text_offset);
        std::array<std::uint8_t, 16> ftext_bytes{};
        if (ftext_offset > binding->parms_size ||
            ftext_bytes.size() > binding->parms_size - ftext_offset) {
            return false;
        }
        std::memcpy(ftext_bytes.data(),
            parameters.data() + ftext_offset, ftext_bytes.size());
        if (!ResolveFTextBytesLocked(ftext_bytes, value)) {
            return false;
        }
        ++monster_static_data_resolution_successes;
        return true;
    }

    [[nodiscard]] bool CurrentAbilitySystemLocked(
        const std::uintptr_t pawn,
        std::uintptr_t& ability_system) const noexcept {
        ability_system = 0;
        if (!InvokeNteReturnLocked(
                NteFunctionKind::GetAbilitySystemComponent, pawn, ability_system) ||
            ability_system == 0 ||
            combat_skill_discovery.ability_system_class == 0) {
            return false;
        }
        std::uintptr_t ability_system_class{};
        return ReadPointerAt(
                   *memory, ability_system, Layout(profile, "object.class"),
                   ability_system_class) &&
            IsClassDerivedFromLocked(
                ability_system_class,
                combat_skill_discovery.ability_system_class);
    }

    void RefreshCombat(std::uint64_t sequence) noexcept {
        combat_attempt_sequence = sequence;
        combat_refresh_failure = 0;
        if (!SemanticFeatureAvailable("nte.combat") || !player_available ||
            player_pawn == 0 || world_pointer == 0) {
            combat_refresh_failure = 1;
            InvalidateCombatSnapshot();
            return;
        }
        AnomalyGenerationHandleV1 character{};
        if (!ObjectHandleLocked(player_pawn, character)) {
            combat_refresh_failure = 2;
            InvalidateCombatSnapshot();
            return;
        }
        std::uintptr_t pawn_class{};
        const auto& get_hp = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetHp)];
        if (!get_hp ||
            !ReadPointerAt(
                *memory, player_pawn, Layout(profile, "object.class"), pawn_class) ||
            !IsClassDerivedFromLocked(pawn_class, get_hp->outer_class)) {
            combat_refresh_failure = 3;
            InvalidateCombatSnapshot();
            return;
        }

        float hp{};
        float max_hp{};
        float shield{};
        bool dead{};
        std::uintptr_t target{};
        std::uintptr_t ability_system{};
        const auto& hp_max_binding = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetHpMax)];
        alignas(std::uint64_t) std::array<std::uint8_t, 8> hp_max_parameters{};
        if (!InvokeNteReturnLocked(NteFunctionKind::GetHp, player_pawn, hp) ||
            !hp_max_binding ||
            !InvokeNteFunctionLocked(
                NteFunctionKind::GetHpMax, player_pawn, hp_max_parameters) ||
            !InvokeNteBoolReturnLocked(
                NteFunctionKind::GetIsDead, player_pawn, dead)) {
            combat_refresh_failure = 4;
            InvalidateCombatSnapshot();
            return;
        }
        bool partial = false;
        if (!InvokeNteReturnLocked(
                NteFunctionKind::GetAttackTarget, player_pawn, target)) {
            target = 0;
            combat_refresh_failure = 5;
            partial = true;
        }
        if (!CurrentAbilitySystemLocked(player_pawn, ability_system)) {
            ability_system = 0;
            combat_refresh_failure = 6;
            partial = true;
        } else if (!InvokeNteReturnLocked(
                NteFunctionKind::GetShieldHealth, ability_system, shield)) {
            shield = 0.0F;
            combat_refresh_failure = 7;
            partial = true;
        }
        const std::size_t max_hp_return = hp_max_binding->offsets[1];
        if (max_hp_return > hp_max_parameters.size() ||
            sizeof(max_hp) > hp_max_parameters.size() - max_hp_return) {
            combat_refresh_failure = 8;
            InvalidateCombatSnapshot();
            return;
        }
        std::memcpy(&max_hp, hp_max_parameters.data() + max_hp_return, sizeof(max_hp));
        if (!std::isfinite(hp) || !std::isfinite(max_hp) ||
            !std::isfinite(shield) || max_hp < 0.0F) {
            combat_refresh_failure = 9;
            InvalidateCombatSnapshot();
            return;
        }
        AnomalyGenerationHandleV1 target_handle{};
        const bool target_handle_failed =
            target != 0 && !ObjectHandleLocked(target, target_handle);
        partial = partial || target_handle_failed;
        if (target_handle_failed) combat_refresh_failure = 10;
        combat_character = character;
        combat_target = target_handle;
        combat_hp = hp;
        combat_max_hp = max_hp;
        combat_shield = shield;
        combat_dead = dead;
        combat_partial = partial;
        combat_available = true;
        combat_sample_sequence = sequence;
        if (!partial) combat_refresh_failure = 0;
        try {
            if (ability_system != 0) {
                RefreshActiveGameplayEffectsLocked(
                    ability_system, sequence, character);
            }
        } catch (...) {
            combat_partial = true;
            combat_refresh_failure = 11;
        }
    }

    struct NativeArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };

    [[nodiscard]] bool ReadNativeArrayHeaderLocked(
        const std::uintptr_t address,
        NativeArrayHeader& header,
        const std::int32_t maximum) const noexcept {
        NativeArrayHeader next;
        if (address == 0 || maximum <= 0 ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.data"), next.data) ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.num"), next.count) ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.max"), next.capacity) ||
            next.count < 0 || next.capacity < next.count || next.capacity > maximum ||
            (next.count != 0 && next.data == 0)) {
            return false;
        }
        header = next;
        return true;
    }

    [[nodiscard]] bool ReadSkillArrayLocked(
        const std::uintptr_t ability_system,
        NativeArrayHeader& header) const noexcept {
        std::uintptr_t container{};
        std::uintptr_t items{};
        return AddAddress(
                   ability_system,
                   Layout(profile, "abilitySystem.activatableAbilities"),
                   container) &&
            AddAddress(
                container, Layout(profile, "abilitySpecContainer.items"), items) &&
            ReadNativeArrayHeaderLocked(
                items, header,
                static_cast<std::int32_t>(Layout(profile, "skills.maxCount")));
    }

    [[nodiscard]] bool ReadCombatNameIdentityLocked(
        const std::uintptr_t object, std::uint64_t& name_id) noexcept {
        name_id = 0;
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        std::uint32_t comparison_index{};
        std::uint32_t number{};
        if (object == 0 || name_offset < 0 ||
            !ReadValue(*memory, object + static_cast<std::uintptr_t>(name_offset),
                comparison_index) ||
            !ReadValue(*memory,
                object + static_cast<std::uintptr_t>(name_offset) +
                    sizeof(comparison_index),
                number)) {
            return false;
        }
        name_id = static_cast<std::uint64_t>(comparison_index) |
            (static_cast<std::uint64_t>(number) << 32U);
        if (name_id == 0) return false;
        combat_event_objects.emplace(name_id, object);
        CacheCombatEventNameLocked(name_id, object);
        return true;
    }

    void PublishActiveEffectEventLocked(
        const ActiveEffectRecord& effect, const std::uint32_t kind,
        const std::uint64_t sequence,
        const AnomalyGenerationHandleV1 target) noexcept {
        AnomalyNteCombatEventV1 event{};
        event.kind = kind;
        event.tick_sequence = sequence;
        event.world = {1, world_generation};
        event.target = target;
        event.name_id = effect.name_id;
        event.duration_seconds = kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD
            ? effect.duration_seconds : 0.0F;
        event.stack_count = kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD
            ? effect.stack_count : 0;
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        RecordCombatEventLocked(event);
    }

    void RefreshActiveGameplayEffectsLocked(
        const std::uintptr_t ability_system, const std::uint64_t sequence,
        const AnomalyGenerationHandleV1 target) {
        std::uintptr_t container{};
        const auto container_offset = Layout(
            profile, "abilitySystem.activeGameplayEffects", -1);
        const auto key_offset = Layout(
            profile, "activeGameplayEffects.arrayReplicationKey", -1);
        const auto items_offset = Layout(profile, "activeGameplayEffects.items", -1);
        if (ability_system == 0 || container_offset < 0 || key_offset < 0 ||
            items_offset < 0 || !AddAddress(ability_system, container_offset, container)) {
            return;
        }
        std::int32_t key_before{};
        if (!ReadValue(*memory,
                container + static_cast<std::uintptr_t>(key_offset), key_before)) {
            return;
        }
        if (active_effects_initialized &&
            active_effect_ability_system == ability_system &&
            active_effect_array_key == key_before) {
            return;
        }

        NativeArrayHeader array;
        if (!ReadNativeArrayHeaderLocked(
                container + static_cast<std::uintptr_t>(items_offset), array,
                static_cast<std::int32_t>(Layout(profile, "buffs.maxCount")))) {
            return;
        }
        const auto stride = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.size"));
        const auto spec_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.spec"));
        const auto replication_id_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.replicationId"));
        const auto replication_key_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.replicationKey"));
        const auto definition_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.def"));
        const auto duration_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.duration"));
        const auto stack_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.stackCount"));
        if (stride == 0 || static_cast<std::size_t>(array.count) >
                (std::numeric_limits<std::size_t>::max)() / stride) {
            return;
        }
        const std::size_t byte_count = static_cast<std::size_t>(array.count) * stride;
        std::vector<std::uint8_t> bytes(byte_count);
        if (byte_count != 0 && !memory->Read(array.data, bytes.data(), byte_count)) return;

        const bool same_owner = active_effects_initialized &&
            active_effect_ability_system == ability_system;
        std::unordered_map<std::int32_t, std::size_t> previous_by_id;
        std::vector<bool> previous_seen;
        if (same_owner) {
            previous_by_id.reserve(active_effects.size());
            previous_seen.resize(active_effects.size());
            for (std::size_t index{}; index < active_effects.size(); ++index) {
                previous_by_id.emplace(active_effects[index].replication_id, index);
            }
        }

        std::vector<ActiveEffectRecord> next;
        next.reserve(static_cast<std::size_t>(array.count));
        for (std::int32_t index{}; index < array.count; ++index) {
            const auto* item = bytes.data() + static_cast<std::size_t>(index) * stride;
            ActiveEffectRecord effect;
            std::memcpy(&effect.replication_id, item + replication_id_offset,
                sizeof(effect.replication_id));
            std::memcpy(&effect.replication_key, item + replication_key_offset,
                sizeof(effect.replication_key));
            std::memcpy(&effect.definition, item + definition_offset,
                sizeof(effect.definition));
            std::memcpy(&effect.duration_seconds, item + duration_offset,
                sizeof(effect.duration_seconds));
            std::memcpy(&effect.stack_count, item + stack_offset,
                sizeof(effect.stack_count));
            if (effect.replication_id <= 0 || effect.definition == 0 ||
                !std::isfinite(effect.duration_seconds) || effect.stack_count < 0) {
                continue;
            }
            const auto previous = previous_by_id.find(effect.replication_id);
            if (previous != previous_by_id.end() &&
                active_effects[previous->second].definition == effect.definition) {
                effect.name_id = active_effects[previous->second].name_id;
                previous_seen[previous->second] = true;
            } else if (!ReadCombatNameIdentityLocked(effect.definition, effect.name_id)) {
                continue;
            }
            next.push_back(effect);
        }
        std::int32_t key_after{};
        if (!ReadValue(*memory,
                container + static_cast<std::uintptr_t>(key_offset), key_after) ||
            key_after != key_before) {
            return;
        }

        if (same_owner) {
            for (std::size_t index{}; index < active_effects.size(); ++index) {
                if (!previous_seen[index]) {
                    PublishActiveEffectEventLocked(active_effects[index],
                        ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE, sequence, target);
                }
            }
        }
        for (const auto& current : next) {
            const auto previous = previous_by_id.find(current.replication_id);
            if (previous == previous_by_id.end() ||
                active_effects[previous->second].definition != current.definition ||
                active_effects[previous->second].replication_key != current.replication_key ||
                active_effects[previous->second].stack_count != current.stack_count) {
                PublishActiveEffectEventLocked(current,
                    ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD, sequence, target);
            }
        }
        active_effects = std::move(next);
        active_effect_ability_system = ability_system;
        active_effect_array_key = key_after;
        active_effects_initialized = true;
    }

    [[nodiscard]] bool ReadSkillIdentityLocked(
        const NativeArrayHeader& array,
        const std::int32_t index,
        SkillRecord& record) const {
        if (index < 0 || index >= array.count) return false;
        const auto stride = static_cast<std::uint64_t>(Layout(profile, "abilitySpec.stride"));
        if (static_cast<std::uint64_t>(index) >
            (std::numeric_limits<std::uint64_t>::max)() / stride) {
            return false;
        }
        std::uintptr_t spec{};
        if (!AddUnsignedAddress(
                array.data, static_cast<std::uint64_t>(index) * stride, spec)) {
            return false;
        }
        std::uint8_t active_count{};
        std::uint8_t state_bits{};
        if (!ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.handle"),
                record.spec_handle) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.ability"), record.ability) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.level"), record.level) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.inputId"), record.input_id) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.activeCount"), active_count) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.stateBits"), state_bits) ||
            record.spec_handle == 0 || record.ability == 0 ||
            !ReadPointerAt(
                *memory, record.ability, Layout(profile, "object.class"),
                record.ability_class_pointer) ||
            combat_skill_discovery.gameplay_ability_class == 0 ||
            !IsClassDerivedFromLocked(
                record.ability_class_pointer,
                combat_skill_discovery.gameplay_ability_class) ||
            !ObjectHandleLocked(record.ability_class_pointer, record.ability_class)) {
            return false;
        }
        record.flags = ANOMALY_NTE_SKILL_V1_VALID |
            ANOMALY_NTE_SKILL_V1_PARTIAL;
        if (active_count != 0) record.flags |= ANOMALY_NTE_SKILL_V1_ACTIVE;
        if ((state_bits & 0x01U) != 0) record.flags |= ANOMALY_NTE_SKILL_V1_INPUT_PRESSED;
        if ((state_bits & 0x02U) != 0) {
            record.flags |= ANOMALY_NTE_SKILL_V1_REMOVE_AFTER_ACTIVATION;
        }
        if ((state_bits & 0x04U) != 0) record.flags |= ANOMALY_NTE_SKILL_V1_PENDING_REMOVE;
        return true;
    }

    [[nodiscard]] static bool SameSkillIdentity(
        const SkillRecord& left,
        const SkillRecord& right) noexcept {
        return left.spec_handle == right.spec_handle &&
            left.ability == right.ability &&
            left.ability_class_pointer == right.ability_class_pointer;
    }

    [[nodiscard]] bool RefreshSkillCooldownLocked(
        const std::uintptr_t ability_system,
        SkillRecord& record) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(
            NteFunctionKind::GetActiveEffectTimeRemainingAndDuration)];
        if (!combat_skill_discovery.cooldown_layout_valid || !binding ||
            binding->parms_size > 64U) {
            return false;
        }

        std::uintptr_t effect_address{};
        std::uintptr_t effect_class{};
        if (!AddAddress(
                record.ability,
                Layout(profile, "ability.cooldownGameplayEffectClass"),
                effect_address) ||
            !ReadValue(*memory, effect_address, effect_class)) {
            return false;
        }
        if (effect_class == 0) {
            record.flags &= ~ANOMALY_NTE_SKILL_V1_PARTIAL;
            record.flags |= ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID;
            return true;
        }
        AnomalyGenerationHandleV1 effect_handle{};
        if (!ObjectHandleLocked(effect_class, effect_handle) ||
            binding->meta_class == 0 ||
            !IsClassDerivedFromLocked(effect_class, binding->meta_class)) {
            return false;
        }

        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        const std::size_t class_offset = binding->offsets[0];
        if (class_offset > binding->parms_size ||
            sizeof(effect_class) > binding->parms_size - class_offset) {
            return false;
        }
        std::memcpy(
            parameters.data() + class_offset, &effect_class, sizeof(effect_class));
        if (!InvokeNteFunctionLocked(
                NteFunctionKind::GetActiveEffectTimeRemainingAndDuration,
                ability_system,
                parameters)) {
            return false;
        }

        const std::size_t remaining_offset = binding->offsets[1];
        const std::size_t duration_offset = binding->offsets[2];
        float remaining{};
        float duration{};
        if (remaining_offset > binding->parms_size ||
            sizeof(remaining) > binding->parms_size - remaining_offset ||
            duration_offset > binding->parms_size ||
            sizeof(duration) > binding->parms_size - duration_offset) {
            return false;
        }
        std::memcpy(
            &remaining, parameters.data() + remaining_offset, sizeof(remaining));
        std::memcpy(
            &duration, parameters.data() + duration_offset, sizeof(duration));
        if (!std::isfinite(remaining) || !std::isfinite(duration)) return false;
        record.cooldown_remaining_seconds = (std::max)(0.0F, remaining);
        record.cooldown_duration_seconds = (std::max)(0.0F, duration);
        record.flags &= ~ANOMALY_NTE_SKILL_V1_PARTIAL;
        record.flags |= ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID;
        return true;
    }

    void RefreshSkills(std::uint64_t sequence) noexcept {
        skill_attempt_sequence = sequence;
        if (!SemanticFeatureAvailable("nte.skills") || !player_available ||
            player_pawn == 0 || world_pointer == 0) {
            InvalidateSkills();
            return;
        }
        std::uintptr_t ability_system{};
        AnomalyGenerationHandleV1 character{};
        NativeArrayHeader array;
        if (!CurrentAbilitySystemLocked(player_pawn, ability_system) ||
            !ObjectHandleLocked(player_pawn, character) ||
            !ReadSkillArrayLocked(ability_system, array)) {
            InvalidateSkills();
            return;
        }
        try {
            std::vector<SkillRecord> next;
            next.reserve(static_cast<std::size_t>(array.count));
            bool partial{};
            for (std::int32_t index{}; index < array.count; ++index) {
                SkillRecord record;
                if (!ReadSkillIdentityLocked(array, index, record)) {
                    InvalidateSkills();
                    return;
                }
                const auto previous = std::ranges::find_if(
                    skills, [&](const SkillRecord& current) {
                        return SameSkillIdentity(current, record);
                    });
                if (previous != skills.end()) {
                    record.ability_path = previous->ability_path;
                } else {
                    record.ability_path = ObjectPathLocked(record.ability_class_pointer);
                }
                if (record.ability_path.empty()) {
                    InvalidateSkills();
                    return;
                }
                if (!RefreshSkillCooldownLocked(ability_system, record)) {
                    partial = true;
                }
                record.character = character;
                record.sequence = sequence;
                next.push_back(std::move(record));
            }

            bool same_identity = skills_available && skill_ability_system == ability_system &&
                skill_character.id == character.id &&
                skill_character.generation == character.generation &&
                skills.size() == next.size();
            if (same_identity) {
                std::vector<bool> matched(skills.size());
                for (SkillRecord& candidate : next) {
                    const auto found = std::find_if(
                        skills.begin(), skills.end(), [&](const SkillRecord& current) {
                            const std::size_t old_index = static_cast<std::size_t>(
                                &current - skills.data());
                            return !matched[old_index] &&
                                SameSkillIdentity(current, candidate);
                        });
                    if (found == skills.end()) {
                        same_identity = false;
                        break;
                    }
                    const std::size_t old_index = static_cast<std::size_t>(found - skills.begin());
                    matched[old_index] = true;
                    candidate.handle = found->handle;
                }
            }
            if (!same_identity) {
                ++skill_generation;
                for (SkillRecord& candidate : next) {
                    if (skill_next_id == 0) ++skill_next_id;
                    candidate.handle = {skill_next_id++, skill_generation};
                }
            }

            skills = std::move(next);
            skill_ability_system = ability_system;
            skill_character = character;
            skill_sample_sequence = sequence;
            skills_available = true;
            skills_partial = partial;
            if ((display_table_loaded_mask & 0x04U) == 0) {
                display_name_demand.store(true, std::memory_order_release);
            } else {
                for (const auto& record : skills) {
                    if (ability_display_names.contains(record.ability_class.id)) {
                        continue;
                    }
                    const auto attempts = ability_display_name_attempts.find(
                        record.ability_class.id);
                    if (attempts != ability_display_name_attempts.end() &&
                        attempts->second >= 2U) {
                        continue;
                    }
                    std::string display_name;
                    if (ResolveAbilityDisplayNameForClassLocked(
                            record.ability_class_pointer, display_name)) {
                        ability_display_name_attempts.erase(record.ability_class.id);
                        CompleteDamageSourceNamesForAbilityLocked(
                            record.ability_class.id, display_name);
                    } else {
                        auto& attempt_count = ability_display_name_attempts[record.ability_class.id];
                        if (attempt_count < 2U) ++attempt_count;
                    }
                    break;
                }
            }
        } catch (...) {
            InvalidateSkills();
        }
    }

    [[nodiscard]] bool WeakObjectHandleLocked(
        const std::int32_t index,
        const std::int32_t serial,
        AnomalyGenerationHandleV1& handle) const noexcept {
        handle = {};
        if (index < 0 || serial <= 0 ||
            static_cast<std::uint64_t>(index) >= object_registry.count) {
            return false;
        }
        std::uintptr_t object{};
        std::uint32_t observed_serial{};
        if (!ReadObjectSlot(
                *memory, object_registry, static_cast<std::uint32_t>(index),
                object, observed_serial) || object == 0 ||
            observed_serial != static_cast<std::uint32_t>(serial)) {
            return false;
        }
        handle = {
            EncodeObjectHandle(
                static_cast<std::uint32_t>(index), observed_serial),
            object_generation};
        return true;
    }

    [[nodiscard]] bool ReadWeakObjectPointerLocked(
        const std::uintptr_t address,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (address == 0) return false;
        std::int32_t index{};
        std::int32_t serial{};
        if (!ReadValue(*memory, address, index) ||
            !ReadValue(
                *memory,
                address + static_cast<std::uintptr_t>(sizeof(index)),
                serial)) {
            return false;
        }
        AnomalyGenerationHandleV1 handle{};
        if (!WeakObjectHandleLocked(index, serial, handle)) return false;
        return ResolveObjectHandleLocked(handle, object) && object != 0;
    }

    void RecordDamageEventLocked(AnomalyNteDamageEventV1 event) noexcept {
        event.struct_size = sizeof(event);
        event.sequence = ++damage_event_sequence;
        if (damage_event_count == kDamageEventCapacity) {
            damage_events[damage_event_start].event = event;
            damage_event_start = (damage_event_start + 1U) % kDamageEventCapacity;
            return;
        }
        const std::size_t slot =
            (damage_event_start + damage_event_count) % kDamageEventCapacity;
        damage_events[slot].event = event;
        ++damage_event_count;
    }

    void RecordCombatEventLocked(AnomalyNteCombatEventV1 event) noexcept {
        event.struct_size = sizeof(event);
        event.sequence = ++combat_event_sequence;
        QueueCombatParticipantNameLocked(event.source);
        QueueCombatParticipantNameLocked(event.target);
        if (combat_event_count == kCombatEventCapacity) {
            combat_events[combat_event_start].event = event;
            combat_event_start = (combat_event_start + 1U) % kCombatEventCapacity;
            return;
        }
        const std::size_t slot =
            (combat_event_start + combat_event_count) % kCombatEventCapacity;
        combat_events[slot].event = event;
        ++combat_event_count;
    }

    static void AppendParticipantNameCandidate(
        PendingCombatParticipantName& pending,
        const std::uint64_t key,
        std::string key_text) {
        if ((key == 0 && key_text.empty()) ||
            pending.key_count >= pending.keys.size()) {
            return;
        }
        for (std::size_t index{}; index < pending.key_count; ++index) {
            if ((key != 0 && pending.keys[index] == key) ||
                (!key_text.empty() && pending.key_texts[index] == key_text)) {
                return;
            }
        }
        const auto index = pending.key_count++;
        pending.keys[index] = key;
        pending.key_texts[index] = std::move(key_text);
    }

    [[nodiscard]] bool CaptureCombatParticipantIdentityLocked(
        const AnomalyGenerationHandleV1 participant,
        const std::uintptr_t object,
        PendingCombatParticipantName& pending) {
        pending = {};
        pending.participant = participant;
        pending.object = object;
        participant_last_handle = participant.id;
        participant_last_player = false;
        participant_last_class_text.clear();
        participant_last_config_text.clear();
        if (object == 0) return false;

        const auto append_fname = [this, &pending](
                                      const std::uint32_t comparison_index,
                                      const std::uint32_t number) {
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            if (key == 0) return;
            std::string key_text;
            static_cast<void>(ResolveFNameLocked(
                comparison_index, number, key_text));
            AppendParticipantNameCandidate(pending, key, std::move(key_text));
        };

        std::uintptr_t object_class{};
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        std::uint32_t class_index{};
        std::uint32_t class_number{};
        if (name_offset >= 0 &&
            ReadPointerAt(*memory, object, Layout(profile, "object.class"), object_class) &&
            object_class != 0 &&
            ReadValue(*memory,
                object_class + static_cast<std::uintptr_t>(name_offset), class_index) &&
            ReadValue(*memory,
                object_class + static_cast<std::uintptr_t>(name_offset) +
                    sizeof(class_index),
                class_number)) {
            std::string class_name;
            static_cast<void>(ResolveFNameLocked(
                class_index, class_number, class_name));
            participant_last_class_text = class_name;
            const auto class_key = static_cast<std::uint64_t>(class_index) |
                (static_cast<std::uint64_t>(class_number) << 32U);
            AppendParticipantNameCandidate(pending, class_key, class_name);
        }
        const bool is_player_participant = std::ranges::any_of(
            std::array{NteFunctionKind::GetMainCharacterId,
                       NteFunctionKind::GetNpcMainCharacterId},
            [this, object_class](const NteFunctionKind kind) {
                const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
                return binding && object_class != 0 &&
                    IsClassDerivedFromLocked(object_class, binding->outer_class);
        });
        pending.player_participant = is_player_participant;
        participant_last_player = is_player_participant;
        if (is_player_participant) {
            const auto read_character_id = [this, object, &pending](
                                               const std::int64_t offset) {
                if (offset < 0) return;
                std::uint32_t index{};
                std::uint32_t number{};
                if (ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(offset),
                        index) &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(offset) +
                            sizeof(index),
                        number)) {
                    const auto key = static_cast<std::uint64_t>(index) |
                        (static_cast<std::uint64_t>(number) << 32U);
                    AppendParticipantNameCandidate(pending, key, {});
                }
            };
            read_character_id(Layout(
                profile, "playerCharacter.defaultCharacterId", -1));
            read_character_id(Layout(
                profile, "playerCharacter.currentDisplayCharacterId", -1));
        }
        const auto config_id_offset = Layout(
            profile, "abilityCharacter.characterConfigId", -1);
        if (!is_player_participant && config_id_offset >= 0) {
            std::uint32_t config_index{};
            std::uint32_t config_number{};
            if (ReadValue(*memory,
                    object + static_cast<std::uintptr_t>(config_id_offset),
                    config_index) &&
                ReadValue(*memory,
                    object + static_cast<std::uintptr_t>(config_id_offset) +
                        sizeof(config_index),
                    config_number)) {
                pending.config_id_key = static_cast<std::uint64_t>(config_index) |
                    (static_cast<std::uint64_t>(config_number) << 32U);
                pending.has_config_id = pending.config_id_key != 0;
                static_cast<void>(ResolveFNameLocked(
                    config_index, config_number, participant_last_config_text));
                append_fname(config_index, config_number);
            }
        }
        // A player can expose a valid MainCharacterID even when its class FName
        // is temporarily unreadable. Keep the object for the deferred getter
        // instead of permanently failing the participant at capture time.
        return true;
    }

    void QueueCombatParticipantNameLocked(
        const AnomalyGenerationHandleV1 participant,
        const std::uintptr_t object) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            combat_participant_names.contains(participant.id) ||
            failed_combat_participant_names.contains(participant.id) ||
            queued_combat_participant_names.contains(participant.id)) {
            return;
        }
        try {
            PendingCombatParticipantName pending;
            if (!CaptureCombatParticipantIdentityLocked(
                    participant, object, pending)) {
                failed_combat_participant_names.insert(participant.id);
                return;
            }
            queued_combat_participant_names.insert(participant.id);
            pending_combat_participant_names.push_back(std::move(pending));
            if (!display_table_scan_complete) {
                display_name_demand.store(true, std::memory_order_release);
            }
        } catch (...) {
            queued_combat_participant_names.erase(participant.id);
        }
    }

    void QueueCombatParticipantNameLocked(
        const AnomalyGenerationHandleV1 participant) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            combat_participant_names.contains(participant.id) ||
            failed_combat_participant_names.contains(participant.id) ||
            queued_combat_participant_names.contains(participant.id)) {
            return;
        }
        std::uintptr_t object{};
        if (!ResolveObjectHandleLocked(participant, object)) {
            return;
        }
        QueueCombatParticipantNameLocked(participant, object);
    }

    void ResolveNextCombatParticipantNameLocked() noexcept {
        if ((display_table_loaded_mask & 0x01U) == 0 ||
            pending_combat_participant_names.empty()) {
            return;
        }
        auto pending = std::move(pending_combat_participant_names.front());
        pending_combat_participant_names.pop_front();
        queued_combat_participant_names.erase(pending.participant.id);
        const auto sequence = tick_sequence.load(std::memory_order_relaxed);
        if (pending.next_retry_sequence > sequence) {
            queued_combat_participant_names.insert(pending.participant.id);
            pending_combat_participant_names.push_back(std::move(pending));
            return;
        }
        const auto retry_or_fail = [&]() noexcept {
            constexpr std::uint8_t kMaximumAttempts = 32;
            constexpr std::uint8_t kMaximumSceneReadyRetries = 4;
            if (++pending.attempts < kMaximumAttempts) {
                pending.next_retry_sequence = sequence + 15U;
                queued_combat_participant_names.insert(pending.participant.id);
                pending_combat_participant_names.push_back(std::move(pending));
            } else if (scene_monster_table_scan_complete &&
                       pending.scene_retry_attempts < kMaximumSceneReadyRetries) {
                ++pending.scene_retry_attempts;
                pending.attempts = kMaximumAttempts - 1U;
                pending.next_retry_sequence = sequence + 120U;
                queued_combat_participant_names.insert(pending.participant.id);
                pending_combat_participant_names.push_back(std::move(pending));
            } else {
                failed_combat_participant_names.insert(pending.participant.id);
            }
        };
        try {
            if (!pending.player_participant) {
                for (std::size_t index{}; index < pending.key_count; ++index) {
                    std::string value;
                    if (ResolveMonsterNameWithFallbackLocked(
                            pending.keys[index], pending.key_texts[index], value) &&
                        !value.empty()) {
                        combat_participant_names.emplace(
                            pending.participant.id, std::move(value));
                        return;
                    }
                }
            }
            std::uintptr_t object{};
            if (!ResolveObjectHandleLocked(pending.participant, object)) {
                retry_or_fail();
                return;
            }
            std::uintptr_t object_class{};
            if (!ReadPointerAt(
                    *memory, object, Layout(profile, "object.class"), object_class)) {
                retry_or_fail();
                return;
            }
            bool player_participant{};
            const auto try_main_character_id = [&](const NteFunctionKind kind) {
                const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
                if (!binding || !IsClassDerivedFromLocked(object_class, binding->outer_class)) {
                    return std::string{};
                }
                player_participant = true;
                std::uint64_t character_id{};
                std::string value;
                if (InvokeNteReturnLocked(kind, object, character_id) &&
                    character_id != 0 &&
                    ResolveDisplayNameFromTableLocked(0, character_id, {}, value) &&
                    !value.empty()) {
                    return value;
                }
                return std::string{};
            };
            for (const auto kind : {NteFunctionKind::GetMainCharacterId,
                                    NteFunctionKind::GetNpcMainCharacterId}) {
                auto value = try_main_character_id(kind);
                if (!value.empty()) {
                    combat_participant_names.emplace(
                        pending.participant.id, std::move(value));
                    return;
                }
            }
            if (!player_participant) {
                const auto name_offset = Layout(profile, "object.nameOffset", -1);
                std::uint32_t class_index{};
                std::uint32_t class_number{};
                if (name_offset >= 0 &&
                    ReadValue(*memory,
                        object_class + static_cast<std::uintptr_t>(name_offset),
                        class_index) &&
                    ReadValue(*memory,
                        object_class + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(class_index),
                        class_number)) {
                    const auto class_key = static_cast<std::uint64_t>(class_index) |
                        (static_cast<std::uint64_t>(class_number) << 32U);
                    std::string class_text;
                    if (ResolveFNameLocked(
                            class_index, class_number, class_text) &&
                        !class_text.empty()) {
                        AppendParticipantNameCandidate(
                            pending, class_key, class_text);
                        std::string value;
                        if (ResolveMonsterNameWithFallbackLocked(
                                class_key, class_text, value) &&
                            !value.empty()) {
                            combat_participant_names.emplace(
                                pending.participant.id, std::move(value));
                            return;
                        }
                    }
                }
                const auto config_id_offset = Layout(
                    profile, "abilityCharacter.characterConfigId", -1);
                std::uint32_t config_index{};
                std::uint32_t config_number{};
                if (config_id_offset >= 0 &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(config_id_offset),
                        config_index) &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(config_id_offset) +
                            sizeof(config_index),
                        config_number)) {
                    const auto config_key = static_cast<std::uint64_t>(config_index) |
                        (static_cast<std::uint64_t>(config_number) << 32U);
                    if (config_key != 0) {
                        pending.config_id_key = config_key;
                        pending.has_config_id = true;
                        std::string config_text;
                        static_cast<void>(ResolveFNameLocked(
                            config_index, config_number, config_text));
                        AppendParticipantNameCandidate(
                            pending, config_key, config_text);
                        std::string value;
                        if (!config_text.empty()) {
                            if (ResolveMonsterNameWithFallbackLocked(
                                    config_key, config_text, value) &&
                                !value.empty()) {
                                combat_participant_names.emplace(
                                    pending.participant.id,
                                    std::move(value));
                                return;
                            }
                        }
                    }
                }
                if (pending.has_config_id && pending.config_id_key != 0) {
                    std::string value;
                    if (ResolveMonsterStaticDataNameLocked(
                            pending.config_id_key, value) &&
                        !value.empty()) {
                        combat_participant_names.emplace(
                            pending.participant.id, std::move(value));
                        return;
                    }
                }
            }
            const std::size_t table_index = player_participant ? 0U : 1U;
            for (std::size_t index{}; index < pending.key_count; ++index) {
                std::string value;
                if (ResolveDisplayNameFromTableLocked(
                        table_index, pending.keys[index],
                        pending.key_texts[index], value) &&
                    !value.empty()) {
                    combat_participant_names.emplace(
                        pending.participant.id, std::move(value));
                    return;
                }
            }
        } catch (...) {
        }
        retry_or_fail();
    }

    [[nodiscard]] bool ReadWeakHandleAtLocked(
        const std::uintptr_t address, AnomalyGenerationHandleV1& handle) const noexcept {
        std::int32_t index{-1};
        std::int32_t serial{};
        if (!ReadValue(*memory, address + static_cast<std::uintptr_t>(Layout(profile, "weakObject.index")), index) ||
            !ReadValue(*memory, address + static_cast<std::uintptr_t>(Layout(profile, "weakObject.serial")), serial)) {
            return false;
        }
        return WeakObjectHandleLocked(index, serial, handle);
    }

    void CacheCombatEventNameLocked(
        const std::uint64_t name_id, const std::uintptr_t object = 0) noexcept {
        if (name_id == 0) return;
        if (object != 0) {
            combat_event_objects.emplace(name_id, object);
        }
        if ((name_id & kDamageSourceBase) != 0 ||
            combat_event_names.contains(name_id) ||
            failed_combat_event_names.contains(name_id) ||
            queued_combat_event_names.contains(name_id)) {
            return;
        }
        try {
            queued_combat_event_names.insert(name_id);
            pending_combat_event_names.push_back(name_id);
            if (!display_table_scan_complete) {
                display_name_demand.store(true, std::memory_order_release);
            }
        } catch (...) {
            queued_combat_event_names.erase(name_id);
        }
    }

    void MarkCombatEventNameResolvedLocked(const std::uint64_t name_id) noexcept {
        for (std::size_t index{}; index < combat_event_count; ++index) {
            auto& event = combat_events[
                (combat_event_start + index) % kCombatEventCapacity].event;
            if (event.name_id == name_id) {
                event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
            }
        }
    }

    void ResolveNextCombatEventNameLocked() noexcept {
        if (!display_table_scan_complete || pending_combat_event_names.empty()) return;
        const std::uint64_t name_id = pending_combat_event_names.front();
        pending_combat_event_names.pop_front();
        queued_combat_event_names.erase(name_id);
        try {
            std::string value;
            const auto object = combat_event_objects.find(name_id);
            if (object != combat_event_objects.end() && object->second != 0) {
                std::uint32_t comparison_index{};
                std::uint32_t number{};
                const auto name_offset = Layout(profile, "object.nameOffset", -1);
                if (name_offset >= 0 &&
                    ReadValue(*memory,
                        object->second + static_cast<std::uintptr_t>(name_offset),
                        comparison_index) &&
                    ReadValue(*memory,
                        object->second + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(comparison_index),
                        number)) {
                    const auto object_key = static_cast<std::uint64_t>(comparison_index) |
                        (static_cast<std::uint64_t>(number) << 32U);
                    std::string object_name;
                    static_cast<void>(ResolveFNameLocked(
                        comparison_index, number, object_name));
                    static_cast<void>(ResolveIndexedDisplayNameLocked(
                        object_key, object_name, value));
                }
            } else {
                static_cast<void>(ResolveIndexedDisplayNameLocked(name_id, {}, value));
            }
            if (!value.empty()) {
                combat_event_names.emplace(name_id, std::move(value));
                MarkCombatEventNameResolvedLocked(name_id);
                return;
            }
        } catch (...) {
        }
        failed_combat_event_names.insert(name_id);
    }

    [[nodiscard]] bool ReadSparseMapViewLocked(
        const std::uintptr_t map,
        SparseMapView& view) const noexcept {
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        const auto data_offset = Layout(profile, "dataTable.rowMapData", 0);
        const auto num_offset = Layout(profile, "dataTable.rowMapNum", 8);
        const auto num_free_offset = Layout(profile, "dataTable.rowMapNumFree", 52);
        const auto max_offset = Layout(profile, "dataTable.rowMapMax", 12);
        const auto stride = Layout(profile, "dataTable.rowMapElementStride", 24);
        const auto row_offset = Layout(profile, "dataTable.rowMapRowOffset", 8);
        const auto flags_data_offset = Layout(profile, "dataTable.rowMapFlagsData", 32);
        const auto flags_num_offset = Layout(profile, "dataTable.rowMapFlagsNum", 40);
        const auto flags_max_offset = Layout(profile, "dataTable.rowMapFlagsMax", 44);
        const auto inline_flags_offset = Layout(profile, "dataTable.rowMapInlineFlags", 16);
        if (map == 0 || row_map_offset < 0 || data_offset < 0 || num_offset < 0 ||
            num_free_offset < 0 ||
            max_offset < 0 || flags_data_offset < 0 || flags_num_offset < 0 ||
            flags_max_offset < 0 || inline_flags_offset < 0 || stride < 16 ||
            stride > 128 || row_offset < 0 ||
            row_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)) > stride) {
            return false;
        }
        SparseMapView next;
        next.map = map;
        next.stride = stride;
        next.row_offset = row_offset;
        if (!ReadValue(*memory, map + static_cast<std::uintptr_t>(data_offset), next.data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_offset), next.num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_free_offset), next.num_free) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(max_offset), next.max) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_data_offset), next.flags_data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_num_offset), next.flags_num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_max_offset), next.flags_max) ||
            next.num < 0 || next.num_free < 0 || next.num_free > next.num ||
            next.num > next.max || next.max > 4096 || next.flags_num < next.num ||
            next.flags_max < next.flags_num || next.flags_num > 4096 ||
            (next.num != 0 && next.data == 0)) {
            return false;
        }
        if (next.num == 0) {
            view = std::move(next);
            return true;
        }
        const auto word_count = static_cast<std::size_t>((next.flags_num + 31) / 32);
        if (word_count == 0 || word_count > 128) return false;
        next.flags.resize(word_count);
        if (next.flags_data != 0) {
            if (!memory->Read(next.flags_data, next.flags.data(),
                              next.flags.size() * sizeof(std::uint32_t))) {
                return false;
            }
        } else if (word_count > 4 || !memory->Read(
                       map + static_cast<std::uintptr_t>(inline_flags_offset),
                       next.flags.data(), next.flags.size() * sizeof(std::uint32_t))) {
            return false;
        }
        view = std::move(next);
        return true;
    }

    void CacheLocalizedNameLocked(
        const std::uint64_t key,
        const std::string_view localized) {
        if (key == 0 || localized.empty()) return;
        localized_names_by_fname[key] = std::string(localized);
        std::string name;
        if (ResolveFNameLocked(
                static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(key >> 32U), name) && !name.empty()) {
            const auto add = [this, &localized](std::string value) {
                if (!value.empty()) localized_names_by_key[std::move(value)] = std::string(localized);
            };
            add(name);
            if (name.starts_with("Default__")) {
                name.erase(0, std::string_view{"Default__"}.size());
                add(name);
            }
            if (name.ends_with("_C")) add(name.substr(0, name.size() - 2U));
            for (const std::string_view prefix : {std::string_view{"GA_"},
                                                   std::string_view{"BP_"},
                                                   std::string_view{"GE_"},
                                                   std::string_view{"Buff_"}}) {
                if (name.starts_with(prefix)) add(name.substr(prefix.size()));
            }
        }
    }

    [[nodiscard]] bool BuildDisplayTableIndexLocked(
        const std::uintptr_t table,
        const std::string_view text_property,
        const std::int64_t text_offset,
        const std::int64_t alias_offset,
        const bool add_monster_identity_alias,
        DisplayTableIndex& index) {
        if (table == 0 || text_offset < 0) return false;
        if (index.table == table && index.text_offset == text_offset &&
            !index.rows_by_fname.empty()) {
            return true;
        }
        std::uintptr_t row_struct{};
        if (text_offset > (std::numeric_limits<std::int32_t>::max)() ||
            !ReadPointerAt(*memory, table, Layout(profile, "dataTable.rowStruct", -1), row_struct) ||
            !ValidateStructFieldLocked(row_struct,
                {text_property, "TextProperty", {}, static_cast<std::int32_t>(text_offset), 16}, true)) {
            return false;
        }
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        SparseMapView map;
        if (row_map_offset < 0 ||
            !ReadSparseMapViewLocked(
                table + static_cast<std::uintptr_t>(row_map_offset), map) ||
            map.num <= 0) {
            return false;
        }
        const auto element_bytes = static_cast<std::size_t>(map.num) *
            static_cast<std::size_t>(map.stride);
        std::vector<std::uint8_t> elements(element_bytes);
        if (!memory->Read(map.data, elements.data(), elements.size())) return false;

        DisplayTableIndex next;
        next.table = table;
        next.text_offset = text_offset;
        next.rows_by_fname.reserve(static_cast<std::size_t>(map.num - map.num_free));
        next.rows_by_key.reserve(static_cast<std::size_t>(map.num - map.num_free));
        for (std::int32_t slot{}; slot < map.num; ++slot) {
            const auto unsigned_slot = static_cast<std::uint32_t>(slot);
            if ((map.flags[static_cast<std::size_t>(unsigned_slot) / 32U] &
                    (1U << (unsigned_slot & 31U))) == 0) {
                continue;
            }
            const auto* element = elements.data() +
                static_cast<std::size_t>(slot) * static_cast<std::size_t>(map.stride);
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            std::uintptr_t row{};
            std::memcpy(&comparison_index, element, sizeof(comparison_index));
            std::memcpy(&number, element + sizeof(comparison_index), sizeof(number));
            std::memcpy(&row, element + map.row_offset, sizeof(row));
            if (row == 0) continue;
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            next.rows_by_fname.emplace(key, row);
            std::string key_text;
            if (ResolveFNameLocked(comparison_index, number, key_text) && !key_text.empty()) {
                if (add_monster_identity_alias) {
                    const auto monster_identity = MonsterDisplayIdentity(key_text);
                    if (!monster_identity.empty()) {
                        next.rows_by_key.emplace(monster_identity, row);
                    }
                }
                AddDisplayRowAliases(next, std::move(key_text), row);
            }
            if (alias_offset >= 0) {
                std::uint32_t alias_index{};
                std::uint32_t alias_number{};
                if (ReadValue(*memory,
                        row + static_cast<std::uintptr_t>(alias_offset),
                        alias_index) &&
                    ReadValue(*memory,
                        row + static_cast<std::uintptr_t>(alias_offset) +
                            sizeof(alias_index),
                        alias_number)) {
                    const auto alias = static_cast<std::uint64_t>(alias_index) |
                        (static_cast<std::uint64_t>(alias_number) << 32U);
                    if (alias != 0) {
                        next.rows_by_fname.emplace(alias, row);
                        std::string alias_text;
                        if (ResolveFNameLocked(
                                alias_index, alias_number, alias_text) &&
                            !alias_text.empty()) {
                            AddDisplayRowAliases(next, std::move(alias_text), row);
                        }
                    }
                }
            }
        }
        if (next.rows_by_fname.empty()) return false;
        index = std::move(next);
        return true;
    }

    static void AddDamageSkillAliases(
        DamageSkillIndex& index,
        std::string name,
        const std::uint64_t ability_key) {
        const auto add = [&index, ability_key](const std::string_view key) {
            if (!key.empty()) {
                index.ability_by_effect_key.emplace(std::string(key), ability_key);
            }
        };
        add(name);
        if (name.starts_with("Default__")) {
            name.erase(0, std::string_view{"Default__"}.size());
            add(name);
        }
        if (name.ends_with("_C")) {
            name.resize(name.size() - 2U);
            add(name);
        }
        if (name.starts_with("GE_")) add(std::string_view(name).substr(3U));
    }

    [[nodiscard]] bool BuildDamageSkillIndexLocked(
        const std::uintptr_t table,
        const std::int64_t ability_name_offset) {
        if (table == 0 || ability_name_offset < 0) return false;
        if (damage_skill_index.table == table &&
            !damage_skill_index.ability_by_effect_fname.empty()) {
            return true;
        }
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        SparseMapView map;
        if (row_map_offset < 0 ||
            !ReadSparseMapViewLocked(
                table + static_cast<std::uintptr_t>(row_map_offset), map) ||
            map.num <= 0) {
            return false;
        }
        const auto element_bytes = static_cast<std::size_t>(map.num) *
            static_cast<std::size_t>(map.stride);
        std::vector<std::uint8_t> elements(element_bytes);
        if (!memory->Read(map.data, elements.data(), elements.size())) return false;

        DamageSkillIndex next;
        next.table = table;
        next.ability_by_effect_fname.reserve(
            static_cast<std::size_t>(map.num - map.num_free));
        next.ability_by_effect_key.reserve(
            static_cast<std::size_t>(map.num - map.num_free));
        for (std::int32_t slot{}; slot < map.num; ++slot) {
            const auto unsigned_slot = static_cast<std::uint32_t>(slot);
            if ((map.flags[static_cast<std::size_t>(unsigned_slot) / 32U] &
                    (1U << (unsigned_slot & 31U))) == 0) {
                continue;
            }
            const auto* element = elements.data() +
                static_cast<std::size_t>(slot) * static_cast<std::size_t>(map.stride);
            std::uint32_t effect_index{};
            std::uint32_t effect_number{};
            std::uintptr_t row{};
            std::memcpy(&effect_index, element, sizeof(effect_index));
            std::memcpy(&effect_number,
                element + sizeof(effect_index), sizeof(effect_number));
            std::memcpy(&row, element + map.row_offset, sizeof(row));
            if (row == 0) continue;
            std::uint32_t ability_index{};
            std::uint32_t ability_number{};
            if (!ReadValue(*memory,
                    row + static_cast<std::uintptr_t>(ability_name_offset),
                    ability_index) ||
                !ReadValue(*memory,
                    row + static_cast<std::uintptr_t>(ability_name_offset) +
                        sizeof(ability_index),
                    ability_number)) {
                continue;
            }
            const auto effect_key = static_cast<std::uint64_t>(effect_index) |
                (static_cast<std::uint64_t>(effect_number) << 32U);
            const auto ability_key = static_cast<std::uint64_t>(ability_index) |
                (static_cast<std::uint64_t>(ability_number) << 32U);
            if (effect_key == 0 || ability_key == 0) continue;
            next.ability_by_effect_fname.emplace(effect_key, ability_key);
            std::string effect_name;
            if (ResolveFNameLocked(effect_index, effect_number, effect_name) &&
                !effect_name.empty()) {
                AddDamageSkillAliases(next, std::move(effect_name), ability_key);
            }
        }
        if (next.ability_by_effect_fname.empty()) return false;
        damage_skill_index = std::move(next);
        return true;
    }

    [[nodiscard]] bool FindDamageSkillAbilityLocked(
        const std::uint64_t effect_key,
        const std::string_view effect_name,
        std::uint64_t& ability_key) const {
        ability_key = 0;
        if (effect_key != 0) {
            const auto found =
                damage_skill_index.ability_by_effect_fname.find(effect_key);
            if (found != damage_skill_index.ability_by_effect_fname.end()) {
                ability_key = found->second;
                return true;
            }
        }
        if (effect_name.empty()) return false;
        std::string normalized(effect_name);
        if (normalized.starts_with("Default__")) normalized.erase(0, 9U);
        if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
        const auto find = [this, &ability_key](const std::string_view candidate) {
            const auto found = damage_skill_index.ability_by_effect_key.find(
                std::string(candidate));
            if (found == damage_skill_index.ability_by_effect_key.end()) return false;
            ability_key = found->second;
            return true;
        };
        if (find(normalized)) return true;
        return normalized.starts_with("GE_") &&
            find(std::string_view(normalized).substr(3U));
    }

    static void AddDisplayRowAliases(
        DisplayTableIndex& index,
        std::string name,
        const std::uintptr_t row) {
        const auto add = [&index, row](const std::string_view key) {
            if (!key.empty()) index.rows_by_key.emplace(std::string(key), row);
        };
        add(name);
        if (name.starts_with("Default__")) {
            name.erase(0, std::string_view{"Default__"}.size());
            add(name);
        }
        if (name.ends_with("_C")) {
            name.resize(name.size() - 2U);
            add(name);
        }
        for (const std::string_view prefix : {std::string_view{"GA_"},
                                               std::string_view{"GE_"},
                                               std::string_view{"Buff_"},
                                               std::string_view{"BP_"}}) {
            if (name.starts_with(prefix)) {
                add(std::string_view(name).substr(prefix.size()));
            }
        }
    }

    [[nodiscard]] static std::string MonsterDisplayIdentity(
        const std::string_view source) {
        return NteMonsterDisplayIdentity(source);
    }

    [[nodiscard]] static bool MonsterIdentityMatches(
        const std::string_view candidate,
        const std::string_view identity) {
        if (candidate.empty() || identity.empty()) return false;
        return MonsterDisplayIdentity(candidate) == identity;
    }

    [[nodiscard]] bool ResolveActorMonsterNameLocked(
        const std::string_view source, std::string& value, const bool base_only = false) const {
        value.clear();
        for (const auto& key : NteMonsterStringTableKeys(source, base_only)) {
            if (ResolveStringTableEntryLocked(key, value) && !value.empty()) return true;
        }
        return false;
    }

    [[nodiscard]] static std::string AbyssStringTableMonsterKey(
        const std::string_view source) {
        std::string key(source);
        if (key.starts_with("Default__")) key.erase(0, 9U);
        if (key.ends_with("_C")) key.resize(key.size() - 2U);
        const auto marker = key.find("_BP_");
        if (marker != std::string::npos) {
            key.resize(marker + 3U);
            return key;
        }
        if (key.ends_with("_BP")) return key;
        return {};
    }

    [[nodiscard]] bool FindDisplayRowLocked(
        const std::uint64_t key,
        const std::string_view key_text,
        std::uintptr_t& row,
        std::int64_t& text_offset) const {
        row = 0;
        text_offset = -1;
        if (key != 0) {
            for (const auto& index : display_table_indexes) {
                const auto found = index.rows_by_fname.find(key);
                if (found != index.rows_by_fname.end()) {
                    row = found->second;
                    text_offset = index.text_offset;
                    return true;
                }
            }
        }
        if (key_text.empty()) return false;
        std::string normalized(key_text);
        if (normalized.starts_with("Default__")) {
            normalized.erase(0, std::string_view{"Default__"}.size());
        }
        if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
        const auto find_text = [this, &row, &text_offset](const std::string_view candidate) {
            for (const auto& index : display_table_indexes) {
                const auto found = index.rows_by_key.find(std::string(candidate));
                if (found == index.rows_by_key.end()) continue;
                row = found->second;
                text_offset = index.text_offset;
                return true;
            }
            return false;
        };
        if (find_text(normalized)) return true;
        if (const auto monster_identity = MonsterDisplayIdentity(normalized);
            !monster_identity.empty()) {
            for (std::size_t index{}; index < display_table_indexes.size(); ++index) {
                if (index != 1U) continue;
                for (const auto& [candidate, candidate_row] :
                         display_table_indexes[index].rows_by_key) {
                    if (!MonsterIdentityMatches(candidate, monster_identity)) continue;
                    row = candidate_row;
                    text_offset = display_table_indexes[index].text_offset;
                    return true;
                }
            }
        }
        for (const std::string_view prefix : {std::string_view{"GA_"},
                                               std::string_view{"GE_"},
                                               std::string_view{"Buff_"},
                                               std::string_view{"BP_"}}) {
            if (normalized.starts_with(prefix) &&
                find_text(std::string_view(normalized).substr(prefix.size()))) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool ResolveDisplayNameFromTableLocked(
        const std::size_t table_index,
        const std::uint64_t key,
        const std::string_view key_text,
        std::string& value) {
        value.clear();
        if (table_index >= display_table_indexes.size()) return false;
        const auto& index = display_table_indexes[table_index];
        std::uintptr_t row{};
        if (key != 0) {
            const auto found = index.rows_by_fname.find(key);
            if (found != index.rows_by_fname.end()) row = found->second;
        }
        const auto find_text = [&index, &row](const std::string_view candidate) {
            const auto found = index.rows_by_key.find(std::string(candidate));
            if (found == index.rows_by_key.end()) return false;
            row = found->second;
            return true;
        };
        if (row == 0 && !key_text.empty()) {
            std::string normalized(key_text);
            if (normalized.starts_with("Default__")) {
                normalized.erase(0, std::string_view{"Default__"}.size());
            }
            if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
            if (!find_text(normalized)) {
                bool found_alias{};
                if (table_index == 1U) {
                    const auto monster_identity = MonsterDisplayIdentity(normalized);
                    found_alias = !monster_identity.empty() && find_text(monster_identity);
                    if (!found_alias && !monster_identity.empty()) {
                        for (const auto& [candidate, candidate_row] : index.rows_by_key) {
                            if (!MonsterIdentityMatches(candidate, monster_identity)) continue;
                            row = candidate_row;
                            found_alias = true;
                            break;
                        }
                    }
                }
                if (!found_alias) {
                    for (const std::string_view prefix : {std::string_view{"GA_"},
                                                           std::string_view{"GE_"},
                                                           std::string_view{"Buff_"},
                                                           std::string_view{"BP_"}}) {
                        if (normalized.starts_with(prefix) &&
                            find_text(std::string_view(normalized).substr(prefix.size()))) {
                            break;
                        }
                    }
                }
            }
        }
        const auto try_row = [this, &index, &value](const std::uintptr_t candidate) {
            std::uintptr_t ftext_address{};
            return candidate != 0 && index.text_offset >= 0 &&
                AddAddress(candidate, index.text_offset, ftext_address) &&
                ResolveFTextLocked(ftext_address, value) && !value.empty();
        };
        if (try_row(row)) return true;
        if (table_index == 1U && !key_text.empty()) {
            std::string normalized(key_text);
            if (normalized.starts_with("Default__")) {
                normalized.erase(0, std::string_view{"Default__"}.size());
            }
            if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
            const auto identity = MonsterDisplayIdentity(normalized);
            if (!identity.empty()) {
                for (const auto& [candidate, candidate_row] : index.rows_by_key) {
                    if (candidate_row == row || !MonsterIdentityMatches(candidate, identity)) {
                        continue;
                    }
                    if (try_row(candidate_row)) return true;
                }
            }
        }
        value.clear();
        return false;
    }

    [[nodiscard]] bool ResolveIndexedDisplayNameLocked(
        const std::uint64_t key,
        const std::string_view supplied_key_text,
        std::string& value) {
        value.clear();
        if (key != 0) {
            const auto cached = localized_names_by_fname.find(key);
            if (cached != localized_names_by_fname.end() && !cached->second.empty()) {
                value = cached->second;
                return true;
            }
        }
        if (!supplied_key_text.empty()) {
            const auto cached = localized_names_by_key.find(
                std::string(supplied_key_text));
            if (cached != localized_names_by_key.end() && !cached->second.empty()) {
                value = cached->second;
                return true;
            }
        }
        std::string key_text(supplied_key_text);
        if (key_text.empty()) {
            static_cast<void>(ResolveFNameLocked(
                static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(key >> 32U), key_text));
        }
        std::uintptr_t row{};
        std::int64_t text_offset{};
        std::uintptr_t ftext_address{};
        if (!FindDisplayRowLocked(key, key_text, row, text_offset) ||
            text_offset < 0 || !AddAddress(row, text_offset, ftext_address) ||
            !ResolveFTextLocked(ftext_address, value) || value.empty()) {
            display_name_demand.store(true, std::memory_order_release);
            return false;
        }
        if (key != 0) {
            CacheLocalizedNameLocked(key, value);
        } else if (!key_text.empty()) {
            localized_names_by_key[std::move(key_text)] = value;
        }
        return true;
    }

    void RefreshDisplayTablesLocked(const std::uint64_t sequence) noexcept {
        if (display_table_generation != object_generation) {
            display_table_generation = object_generation;
            display_table_scan_complete = false;
            display_table_loaded_mask = 0;
            display_game_data = 0;
            display_game_data_generation = object_generation;
            display_table_indexes = {};
            damage_skill_index = {};
        }
        static_cast<void>(sequence);
        if (!display_name_demand.exchange(false, std::memory_order_acq_rel) ||
            display_table_scan_complete ||
            object_registry.items == 0 || object_registry.count == 0) {
            return;
        }
        try {
            if (display_game_data_generation != object_generation) {
                display_game_data = 0;
                display_game_data_generation = object_generation;
            }
            if (display_game_data == 0 && !ResolveGameDataLocked(display_game_data)) return;
            struct TableSpec {
                std::size_t slot;
                std::string_view table_key;
                std::string_view text_property;
                std::string_view text_key;
                std::string_view alias_key;
                std::int64_t alias_additional{};
                std::uint8_t bit;
            };
            static constexpr std::array specs{
                TableSpec{0, "gameData.characterDataTable", "ItemName", "staticItemData.itemName",
                    {}, 0, 0x01U},
                // Slot 1 was an icon-only StaticMonsterInfo table, not a name source.
                TableSpec{2, "gameData.gameplayAbilityTipsDataTable", "Name", "gameplayAbilityTips.name",
                    "gameplayAbilityTips.gameplayAbility", 24, 0x04U},
                TableSpec{3, "gameData.gameplayEffectTipsDataTable", "Name", "gameplayEffectTips.name",
                    "gameplayEffectTips.geParamName", 0, 0x08U},
            };
            for (const auto& spec : specs) {
                if ((display_table_loaded_mask & spec.bit) != 0) continue;
                std::uintptr_t table{};
                const auto table_offset = Layout(profile, spec.table_key, -1);
                const auto alias_base = spec.alias_key.empty()
                    ? -1 : Layout(profile, spec.alias_key, -1);
                const auto alias_offset = alias_base < 0
                    ? -1 : alias_base + spec.alias_additional;
                if (table_offset < 0 ||
                    !ReadPointerAt(*memory, display_game_data, table_offset, table) ||
                    table == 0 ||
                    !BuildDisplayTableIndexLocked(
                        table, spec.text_property, Layout(profile, spec.text_key, -1), alias_offset,
                        false, display_table_indexes[spec.slot])) {
                    continue;
                }
                display_table_loaded_mask |= spec.bit;
            }
            if ((display_table_loaded_mask & 0x10U) == 0) {
                std::uintptr_t ability_data{};
                std::uintptr_t skill_damage_table{};
                if (ReadPointerAt(*memory, display_game_data,
                        Layout(profile, "gameData.abilityDataAsset"), ability_data) &&
                    ability_data != 0 &&
                    ReadPointerAt(*memory, ability_data,
                        Layout(profile, "abilityData.skillDamageDataTable"),
                        skill_damage_table) &&
                    skill_damage_table != 0 &&
                    BuildDamageSkillIndexLocked(
                        skill_damage_table, Layout(profile, "skillDamage.gaName", -1))) {
                    display_table_loaded_mask |= 0x10U;
                }
            }
            display_table_scan_complete = display_table_loaded_mask == 0x1DU;
        } catch (...) {
        }
    }

    [[nodiscard]] static bool CombatHandleCompatible(
        const AnomalyGenerationHandleV1 left,
        const AnomalyGenerationHandleV1 right) noexcept {
        return left.id == 0 || right.id == 0 ||
            (left.id == right.id && left.generation == right.generation);
    }

    // Damage text already contains the complete weak-object pair. Validate it
    // only when a consumer resolves the participant; walking the object table
    // for both sides of every hit stalls the game-thread tick.
    [[nodiscard]] AnomalyGenerationHandleV1 WeakHandleFromBytesLocked(
        const std::int32_t index, const std::int32_t serial) const noexcept {
        if (index < 0 || serial <= 0) return {};
        return {EncodeObjectHandle(
                    static_cast<std::uint32_t>(index),
                    static_cast<std::uint32_t>(serial)),
                object_generation};
    }

    [[nodiscard]] bool MergeCombatDamageLocked(
        const AnomalyNteCombatEventV1& incoming) noexcept {
        const bool incoming_display =
            (incoming.flags & ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID) != 0;
        constexpr std::size_t kMaximumMergeLookback = 64;
        const std::size_t lookback = (std::min)(combat_event_count, kMaximumMergeLookback);
        for (std::size_t index = combat_event_count;
             index > combat_event_count - lookback; --index) {
            auto& candidate = combat_events[
                (combat_event_start + index - 1U) % kCombatEventCapacity].event;
            if (candidate.kind != ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE ||
                candidate.tick_sequence != incoming.tick_sequence) {
                continue;
            }
            const bool same_participants =
                CombatHandleCompatible(candidate.source, incoming.source) &&
                CombatHandleCompatible(candidate.target, incoming.target);
            const bool same_source_name = candidate.name_id != 0 &&
                incoming.name_id != 0 && candidate.name_id == incoming.name_id;
            if (!same_participants && !same_source_name) continue;
            const bool candidate_display =
                (candidate.flags & ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID) != 0;
            if (candidate.value == incoming.value &&
                candidate.basic_value == incoming.basic_value &&
                candidate.final_value == incoming.final_value &&
                candidate.damage_type == incoming.damage_type &&
                candidate.display_type == incoming.display_type &&
                candidate.reaction_type == incoming.reaction_type &&
                candidate.reaction_display_type == incoming.reaction_display_type &&
                candidate_display == incoming_display &&
                candidate.name_id == incoming.name_id) {
                // CharacterOnDamaged and the UI multicast can both emit the
                // same hit. Collapse exact duplicates before publishing the
                // event so the demo never shows damage twice.
                return true;
            }
            if (candidate_display == incoming_display) {
                // Both native damage delegates and both floaties delegates can
                // publish the same hit. Participant compatibility plus the
                // complete numeric payload distinguishes it from a real
                // multi-hit that happens in the same game tick.
                if (candidate.value == incoming.value &&
                    candidate.basic_value == incoming.basic_value &&
                    candidate.final_value == incoming.final_value &&
                    candidate.damage_type == incoming.damage_type &&
                    candidate.display_type == incoming.display_type &&
                    candidate.reaction_type == incoming.reaction_type &&
                    candidate.reaction_display_type == incoming.reaction_display_type) {
                    return true;
                }
                continue;
            }
            if (candidate.source.id == 0) candidate.source = incoming.source;
            if (candidate.target.id == 0) candidate.target = incoming.target;
            candidate.flags |= incoming.flags;
            // The native event uses a generation-local GameplayEffect source
            // id. Damage text carries the stable InjurySourceName FName used by
            // the indexed localized tables, so it is the authoritative display
            // key even when its FText is materialized later in this tick.
            if (incoming.name_id != 0) {
                const auto incoming_name = combat_event_names.find(incoming.name_id);
                const auto candidate_name = combat_event_names.find(candidate.name_id);
                const bool incoming_resolved = incoming_name != combat_event_names.end() &&
                    !incoming_name->second.empty();
                const bool candidate_resolved = candidate_name != combat_event_names.end() &&
                    !candidate_name->second.empty();
                const bool incoming_is_object_source =
                    (incoming.name_id & kDamageSourceBase) != 0;
                const bool candidate_is_object_source =
                    (candidate.name_id & kDamageSourceBase) != 0;
                if (incoming_is_object_source && !candidate_is_object_source &&
                    candidate.name_id != 0) {
                    const auto source = damage_source_objects.find(incoming.name_id);
                    if (source != damage_source_objects.end()) {
                        combat_event_objects[candidate.name_id] = source->second;
                    }
                } else if (candidate.name_id == 0 ||
                    (!incoming_is_object_source && candidate_is_object_source) ||
                    (incoming_resolved && !candidate_resolved)) {
                    candidate.name_id = incoming.name_id;
                }
                if (!incoming_is_object_source &&
                    incoming_name == combat_event_names.end()) {
                    CacheCombatEventNameLocked(incoming.name_id);
                }
            }
            if (incoming_display) {
                candidate.value = incoming.value;
                candidate.basic_value = incoming.basic_value;
                candidate.final_value = incoming.final_value;
                candidate.damage_type = incoming.damage_type;
                candidate.display_type = incoming.display_type;
                candidate.reaction_type = incoming.reaction_type;
                candidate.reaction_display_type = incoming.reaction_display_type;
            } else {
                candidate.final_value = incoming.final_value;
                if (candidate.basic_value == 0) candidate.basic_value = incoming.basic_value;
                if (candidate.value == 0) candidate.value = incoming.value;
            }
            return true;
        }
        return false;
    }

    template <typename Value>
    [[nodiscard]] static bool ReadCaptureBytes(
        const std::span<const std::uint8_t> bytes,
        const std::int64_t offset,
        Value& destination) noexcept {
        if (offset < 0 || static_cast<std::uint64_t>(offset) > bytes.size() ||
            sizeof(Value) > bytes.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(
            &destination, bytes.data() + static_cast<std::size_t>(offset), sizeof(Value));
        return true;
    }

    [[nodiscard]] bool DamageTagsContainCriticalLocked(
        const NteDamageTags& tags) noexcept {
        const auto is_critical = [](std::string_view tag) noexcept {
            std::string normalized(tag);
            for (char& character : normalized) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return normalized == "critical" || normalized == "criticalhit" ||
                normalized.ends_with(".critical") ||
                normalized.ends_with(".criticalhit") || normalized.ends_with(".crit");
        };
        for (const auto key : tags.Values()) {
            const auto cached = damage_critical_tag_cache.find(key);
            if (cached != damage_critical_tag_cache.end()) {
                if (cached->second) return true;
                continue;
            }
            std::string tag;
            const bool critical = ResolveFNameLocked(
                static_cast<std::uint32_t>(key),
                static_cast<std::uint32_t>(key >> 32U), tag) && is_critical(tag);
            damage_critical_tag_cache.emplace(key, critical);
            if (critical) return true;
        }
        return false;
    }

    void EnrichDamageRecordLocked(
        const AnomalyNteCombatEventV1& display_event) noexcept {
        if (display_event.tick_sequence == 0 ||
            (display_event.source.id == 0 && display_event.target.id == 0)) {
            return;
        }
        const std::size_t lookback = (std::min)(damage_event_count, std::size_t{64});
        for (std::size_t index = damage_event_count;
             index > damage_event_count - lookback; --index) {
            auto& candidate = damage_events[
                (damage_event_start + index - 1U) % kDamageEventCapacity].event;
            if (candidate.tick_sequence != display_event.tick_sequence ||
                (display_event.source.id != 0 &&
                    !SameHandle(candidate.attacker, display_event.source)) ||
                (display_event.target.id != 0 &&
                    !SameHandle(candidate.victim, display_event.target))) {
                continue;
            }
            candidate.display_damage = display_event.value;
            candidate.basic_damage = display_event.basic_value;
            candidate.final_damage = display_event.final_value;
            candidate.damage_type = display_event.damage_type;
            candidate.display_type = display_event.display_type;
            candidate.reaction_type = display_event.reaction_type;
            candidate.reaction_display_type = display_event.reaction_display_type;
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_HEAD_HIT;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_WEAK_UNBALANCE;
            }
            if (candidate.source_id != 0 && display_event.name_id != 0) {
                const auto source = damage_source_objects.find(candidate.source_id);
                if (source != damage_source_objects.end()) {
                    combat_event_objects[display_event.name_id] = source->second;
                }
            }
            return;
        }
    }

    void CaptureDamageTextBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint16_t info_offset,
        const std::uint64_t capture_tick_sequence) noexcept {
        if (info_offset > parameters.size() ||
            72U > parameters.size() - static_cast<std::size_t>(info_offset)) return;
        const auto bytes = parameters.subspan(info_offset, 72U);
        const auto read_at = [&bytes](const std::int64_t offset, auto& destination) noexcept {
            using Value = std::remove_reference_t<decltype(destination)>;
            if (offset < 0 || static_cast<std::uint64_t>(offset) > bytes.size() ||
                sizeof(Value) > bytes.size() - static_cast<std::size_t>(offset)) {
                return false;
            }
            std::memcpy(
                &destination, bytes.data() + static_cast<std::size_t>(offset), sizeof(Value));
            return true;
        };
        const auto read_weak = [&read_at, this](
                                   const std::int64_t offset,
                                   AnomalyGenerationHandleV1& handle) noexcept {
            std::int32_t index{-1};
            std::int32_t serial{};
            const auto index_offset = Layout(profile, "weakObject.index", -1);
            const auto serial_offset = Layout(profile, "weakObject.serial", -1);
            if (index_offset < 0 || serial_offset < 0 ||
                !read_at(offset + index_offset, index) ||
                !read_at(offset + serial_offset, serial)) {
                return false;
            }
            handle = WeakHandleFromBytesLocked(index, serial);
            return handle.id != 0;
        };
        AnomalyNteCombatEventV1 event{};
        event.kind = ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE;
        event.tick_sequence = capture_tick_sequence;
        event.world = {1, world_generation};
        std::int32_t display_damage{};
        std::uint8_t damage_type{};
        std::uint8_t critical{};
        std::uint8_t head_hit{};
        std::uint8_t weak_unbalance{};
        std::uint8_t display_type{};
        std::uint8_t reaction_type{};
        std::uint8_t reaction_display_type{};
        std::int32_t basic_damage{};
        std::int32_t final_damage{};
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.displayDamage"), display_damage));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.damageType"), damage_type));
        const bool critical_valid = read_at(Layout(profile, "damageTextInfo.critical"), critical);
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.headHit"), head_hit));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.weakUnbalance"), weak_unbalance));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.displayType"), display_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.reactionType"), reaction_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.reactionDisplayType"), reaction_display_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.basicDamage"), basic_damage));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.finalDamage"), final_damage));
        static_cast<void>(read_weak(Layout(profile, "damageTextInfo.attacker"), event.source));
        static_cast<void>(read_weak(Layout(profile, "damageTextInfo.victim"), event.target));
        event.value = display_damage;
        event.damage_type = damage_type;
        event.display_type = display_type;
        event.reaction_type = reaction_type;
        event.reaction_display_type = reaction_display_type;
        event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID;
        if (critical != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL;
        if (critical_valid) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID;
        if (head_hit != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT;
        if (weak_unbalance != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE;
        std::uint32_t source_name_id{};
        std::uint32_t source_name_number{};
        // FCombatStatisticsData::InjurySourceName is the first FName in the
        // embedded statistics struct. Keep the explicit field name while
        // accepting older Profiles that only exposed the struct offset.
        const auto statistics = Layout(
            profile, "damageTextInfo.injurySourceName",
            Layout(profile, "damageTextInfo.combatStatistics"));
        static_cast<void>(read_at(statistics, source_name_id));
        static_cast<void>(read_at(statistics + static_cast<std::int64_t>(sizeof(source_name_id)), source_name_number));
        event.name_id = static_cast<std::uint64_t>(source_name_id) |
            (static_cast<std::uint64_t>(source_name_number) << 32U);
        if (event.name_id != 0 && !combat_event_names.contains(event.name_id)) {
            CacheCombatEventNameLocked(event.name_id);
        }
        event.basic_value = basic_damage;
        event.final_value = final_damage;
        EnrichDamageRecordLocked(event);
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        if (!MergeCombatDamageLocked(event)) RecordCombatEventLocked(event);
    }

    void CaptureBuffBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint64_t capture_tick_sequence) noexcept {
        // EnqueueCombatProcessEvent compacts every Buff ABI variant to
        // [GameplayEffect*, duration, stack count, is-add]. This keeps the
        // game-thread hook independent of the 0x298-byte reflected spec.
        std::uintptr_t definition{};
        if (!ReadCaptureBytes(parameters, 0, definition) || definition == 0) return;
        float duration{};
        std::int32_t stack_count{};
        std::uint8_t is_add =
            (parameters.size() >= sizeof(definition) + sizeof(float) + sizeof(std::int32_t) + sizeof(is_add) &&
             ReadCaptureBytes(parameters,
                 sizeof(definition) + sizeof(float) + sizeof(std::int32_t), is_add))
            ? is_add : static_cast<std::uint8_t>(0);
        static_cast<void>(ReadCaptureBytes(parameters, sizeof(definition), duration));
        static_cast<void>(ReadCaptureBytes(
            parameters, sizeof(definition) + sizeof(duration), stack_count));
        const bool added = is_add != 0;
        AnomalyNteCombatEventV1 event{};
        event.kind = added ? ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD : ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE;
        event.tick_sequence = capture_tick_sequence;
        event.world = {1, world_generation};
        AnomalyGenerationHandleV1 definition_handle{};
        if (!ObjectHandleLocked(definition, definition_handle)) return;
        std::uint32_t comparison_index{};
        std::uint32_t number{};
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        if (name_offset >= 0 &&
            ReadValue(*memory, definition + static_cast<std::uintptr_t>(name_offset), comparison_index) &&
            ReadValue(*memory, definition + static_cast<std::uintptr_t>(name_offset) + sizeof(comparison_index), number)) {
            event.name_id = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
        }
        if (event.name_id == 0) event.name_id = definition_handle.id;
        CacheCombatEventNameLocked(event.name_id, definition);
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        if (added) {
            event.duration_seconds = duration;
            event.stack_count = stack_count;
        }
        RecordCombatEventLocked(event);
    }

    void CaptureCombatProcessEventLocked(
        const CombatCaptureKind kind,
        const std::span<const std::uint8_t> parameters,
        const std::uint16_t info_offset,
        const std::uint64_t capture_tick_sequence) noexcept {
        switch (kind) {
        case CombatCaptureKind::CharacterDamage:
            CaptureCharacterDamageBytesLocked(parameters, capture_tick_sequence);
            break;
        case CombatCaptureKind::Damage:
            CaptureDamageTextBytesLocked(parameters, info_offset, capture_tick_sequence);
            break;
        case CombatCaptureKind::BuffAdd:
        case CombatCaptureKind::BuffRemove:
            CaptureBuffBytesLocked(parameters, capture_tick_sequence);
            break;
        }
    }

    void DrainCombatCaptureQueueLocked() noexcept {
        auto read = combat_capture_read.load(std::memory_order_relaxed);
        const auto write = combat_capture_write.load(std::memory_order_acquire);
        constexpr std::size_t kMaximumCaptureDrainPerTick = 32;
        std::size_t drained{};
        while (read != write && drained < kMaximumCaptureDrainPerTick) {
            const auto& capture = combat_capture_queue[
                read % kCombatCaptureQueueCapacity];
            CaptureCombatProcessEventLocked(
                capture.kind,
                std::span<const std::uint8_t>(capture.payload).first(capture.payload_size),
                capture.info_offset, capture.tick_sequence);
            ++read;
            ++drained;
        }
        combat_capture_read.store(read, std::memory_order_release);
    }

    [[nodiscard]] bool EnqueueDamageTextPayload(
        const std::uint8_t* const source,
        const std::uint64_t capture_tick_sequence) noexcept {
        if (source == nullptr) return false;
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
            combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = CombatCaptureKind::Damage;
        capture.payload_size = 72;
        capture.info_offset = 0;
        capture.tick_sequence = capture_tick_sequence;
        std::memcpy(capture.payload.data(), source, capture.payload_size);
        combat_capture_write.store(write + 1U, std::memory_order_release);
        return true;
    }

    void EnqueueCombatProcessEvent(
        const CombatCaptureBindings& bindings,
        const std::uintptr_t function,
        const void* const parameters) noexcept {
        if (parameters == nullptr) return;
        if (function == bindings.damage) {
            damage_floaties_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.monster_damage) {
            monster_damage_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.player_damage_queue) {
            player_damage_queue_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.damage_widget) {
            damage_widget_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (std::ranges::any_of(
                       bindings.buffs, [function](const auto& buff) {
                           return buff.function != 0 && buff.function == function;
                       })) {
            buff_call_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            return;
        }
        if (function == bindings.player_damage_queue) {
            struct DamageTextArrayView {
                std::uintptr_t data{};
                std::int32_t num{};
                std::int32_t max{};
            } view;
            const auto* const bytes = static_cast<const std::uint8_t*>(parameters);
            std::memcpy(&view, bytes + bindings.player_damage_queue_offset, sizeof(view));
            if (view.data == 0 || view.num <= 0 || view.max < view.num) return;
            const auto capture_tick_sequence =
                tick_sequence.load(std::memory_order_relaxed);
            for (std::int32_t index{}; index < view.num; ++index) {
                const auto* const damage_text = reinterpret_cast<const std::uint8_t*>(
                    view.data + static_cast<std::uintptr_t>(index) * 72U);
                if (!EnqueueDamageTextPayload(damage_text, capture_tick_sequence)) {
                    if (index + 1 < view.num) {
                        combat_capture_drop_count.fetch_add(
                            static_cast<std::uint64_t>(view.num - index - 1),
                            std::memory_order_relaxed);
                    }
                    break;
                }
            }
            return;
        }
        if (function == bindings.damage_widget) {
            const auto* const damage_text =
                static_cast<const std::uint8_t*>(parameters) +
                bindings.damage_widget_info_offset;
            static_cast<void>(EnqueueDamageTextPayload(
                damage_text, tick_sequence.load(std::memory_order_relaxed)));
            return;
        }
        CombatCaptureKind kind{};
        std::uint16_t payload_size{};
        std::uint16_t info_offset{};
        if (function == bindings.damage || function == bindings.monster_damage) {
            kind = CombatCaptureKind::Damage;
            info_offset = bindings.damage_info_offset;
            payload_size = 72;
        } else {
            const CombatCaptureBindings::Buff* buff = nullptr;
            for (const auto& candidate : bindings.buffs) {
                if (candidate.function != 0 && candidate.function == function) {
                    buff = &candidate;
                    break;
                }
            }
            if (buff == nullptr || buff->object_offset == 0xFFFFU) return;
            kind = CombatCaptureKind::BuffAdd;
            payload_size = static_cast<std::uint16_t>(
                sizeof(std::uintptr_t) + sizeof(float) + sizeof(std::int32_t) + sizeof(std::uint8_t));
            const auto write = combat_capture_write.load(std::memory_order_relaxed);
            const auto read = combat_capture_read.load(std::memory_order_acquire);
            if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
                combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
            capture.kind = kind;
            capture.payload_size = payload_size;
            capture.info_offset = 0;
            capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
            std::memset(capture.payload.data(), 0, payload_size);
            const auto* source = static_cast<const std::uint8_t*>(parameters);
            std::uintptr_t object_value{};
            if (buff->parms_size == 0 ||
                static_cast<std::size_t>(buff->object_offset) + sizeof(object_value) > buff->parms_size ||
                (buff->duration_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->duration_offset) + sizeof(float) > buff->parms_size) ||
                (buff->stack_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->stack_offset) + sizeof(std::int32_t) > buff->parms_size) ||
                (buff->is_add_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->is_add_offset) + sizeof(std::uint8_t) > buff->parms_size)) {
                return;
            }
            if (buff->definition_offset != 0xFFFFU) {
                if (static_cast<std::size_t>(buff->object_offset) + buff->definition_offset +
                    sizeof(object_value) > buff->parms_size) return;
                std::memcpy(&object_value, source + buff->object_offset + buff->definition_offset,
                    sizeof(object_value));
            } else {
                std::memcpy(&object_value, source + buff->object_offset, sizeof(object_value));
            }
            if (object_value == 0) return;
            std::memcpy(capture.payload.data(), &object_value, sizeof(object_value));
            if (buff->duration_offset != 0xFFFFU) {
                std::memcpy(capture.payload.data() + sizeof(object_value),
                    source + buff->duration_offset, sizeof(float));
            }
            if (buff->stack_offset != 0xFFFFU) {
                std::memcpy(capture.payload.data() + sizeof(object_value) + sizeof(float),
                    source + buff->stack_offset, sizeof(std::int32_t));
            }
            std::uint8_t added =
                (buff->duration_offset == 0xFFFFU &&
                    buff->stack_offset == 0xFFFFU &&
                    buff->is_add_offset == 0xFFFFU)
                ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(1);
            if (buff->is_add_offset != 0xFFFFU) {
                std::memcpy(&added, source + buff->is_add_offset, sizeof(added));
            }
            std::memcpy(capture.payload.data() + sizeof(object_value) + sizeof(float) + sizeof(std::int32_t),
                &added, sizeof(added));
            capture.kind = added != 0 ? CombatCaptureKind::BuffAdd : CombatCaptureKind::BuffRemove;
            combat_capture_write.store(write + 1U, std::memory_order_release);
            return;
        }
        if (payload_size == 0 || payload_size > kCombatCapturePayloadBytes) return;
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >=
            kCombatCaptureQueueCapacity) {
            combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = kind;
        capture.payload_size = payload_size;
        capture.info_offset = 0;
        capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
        const auto* const source = static_cast<const std::uint8_t*>(parameters);
        std::memcpy(capture.payload.data(), source + info_offset, payload_size);
        combat_capture_write.store(write + 1U, std::memory_order_release);
    }

    void EnqueueCharacterDamage(
        const CombatCaptureBindings& bindings,
        const std::uintptr_t damage_event,
        const std::uintptr_t victim,
        const std::uintptr_t attacker,
        const std::uintptr_t damage_causer) noexcept {
        damage_native_call_count.fetch_add(1, std::memory_order_relaxed);
        const auto drop = [this]() noexcept {
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
        };
        const DWORD expected_thread = game_thread_id.load(std::memory_order_acquire);
        if (!started.load(std::memory_order_acquire) || damage_event == 0 || victim == 0 ||
            expected_thread == 0 || expected_thread != GetCurrentThreadId() ||
            bindings.damage_value_offset == 0xFFFFU ||
            bindings.damage_source_offset == 0xFFFFU ||
            bindings.damage_tags_offset == 0xFFFFU) {
            drop();
            return;
        }
        NteDamageCriticalState synchronous_critical;
        bool query_failed{};
        for (const std::uintptr_t candidate : {victim, attacker}) {
            if (candidate == 0 || synchronous_critical.critical) continue;
            std::uintptr_t ability_system{};
            if (!CurrentAbilitySystemLocked(candidate, ability_system)) {
                query_failed = true;
                continue;
            }
            bool candidate_critical{};
            crit_query_call_count.fetch_add(1, std::memory_order_relaxed);
            if (InvokeNteBoolReturnLocked(
                    NteFunctionKind::CurrentDamageIsCrit, ability_system, candidate_critical)) {
                synchronous_critical.valid = true;
                crit_query_success_count.fetch_add(1, std::memory_order_relaxed);
                if (candidate_critical) {
                    synchronous_critical.critical = true;
                    crit_true_count.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                query_failed = true;
            }
        }
        synchronous_critical.valid = synchronous_critical.critical ||
            (synchronous_critical.valid && !query_failed);
        // Query before reserving the queue slot: ProcessEvent can reenter capture.
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
            drop();
            return;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = CombatCaptureKind::CharacterDamage;
        capture.payload_size = sizeof(NteCharacterDamageCapture);
        capture.info_offset = 0;
        capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
        NteCharacterDamageCapture damage;
        const auto* const source = static_cast<const std::uint8_t*>(
            reinterpret_cast<const void*>(damage_event));
        std::memcpy(&damage.damage, source + bindings.damage_value_offset, sizeof(float));
        std::memcpy(&damage.source_index, source + bindings.damage_source_offset, 4);
        std::memcpy(&damage.source_serial, source + bindings.damage_source_offset + 4, 4);
        damage.victim = victim;
        damage.attacker = attacker;
        damage.critical = synchronous_critical;
        std::uintptr_t saved_skill_cdo{};
        std::int32_t active_spec_handle{};
        const auto trigger_handle_offset = Layout(
            profile, "abilitySpawnActor.triggerAbilityHandle", -1);
        const auto saved_skill_offset = Layout(
            profile, "abilitySpawnActor.savedTriggerSkillCDO", -1);
        if (damage_causer != 0 && trigger_handle_offset >= 0) {
            static_cast<void>(ReadValue(
                *memory,
                damage_causer + static_cast<std::uintptr_t>(trigger_handle_offset),
                active_spec_handle));
            if (active_spec_handle < 0) active_spec_handle = 0;
        }
        if (damage_causer != 0 && saved_skill_offset >= 0) {
            static_cast<void>(ReadPointerAt(
                *memory, damage_causer, saved_skill_offset, saved_skill_cdo));
        }
        damage.saved_skill_cdo = saved_skill_cdo;
        damage.active_spec_handle = active_spec_handle;
        damage.tags = CaptureNteDamageTags(damage_event + bindings.damage_tags_offset,
            [this](std::uintptr_t address, void* destination, std::size_t size) {
                return memory->Read(address, destination, size);
            });
        std::memcpy(capture.payload.data(), &damage, sizeof(damage));
        combat_capture_write.store(write + 1U, std::memory_order_release);
    }

    void CacheDamageParticipantPathLocked(
        const AnomalyGenerationHandleV1 participant) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            damage_participant_paths.contains(participant.id)) {
            return;
        }
        try {
            std::uintptr_t object{};
            if (!ResolveObjectHandleLocked(participant, object)) return;
            std::string path = ObjectPathLocked(object);
            if (!path.empty()) {
                damage_participant_paths.emplace(participant.id, std::move(path));
            }
        } catch (...) {
        }
    }

    [[nodiscard]] std::uint64_t ResolveDamageEventSourceLocked(
        const std::int32_t source_index,
        const std::int32_t source_serial) noexcept {
        if (source_index < 0 || source_serial <= 0) return 0;
        AnomalyGenerationHandleV1 source_handle{};
        if (!WeakObjectHandleLocked(source_index, source_serial, source_handle)) {
            return 0;
        }
        const auto existing = damage_source_object_ids.find(source_handle.id);
        if (existing != damage_source_object_ids.end()) return existing->second;

        std::uintptr_t source_object{};
        if (!ResolveObjectHandleLocked(source_handle, source_object)) return 0;
        const std::uint64_t source_id = damage_next_source_id++;
        damage_source_object_ids.emplace(source_handle.id, source_id);
        damage_source_objects.emplace(source_id, source_object);
        combat_event_objects.emplace(source_id, source_object);
        return source_id;
    }

    [[nodiscard]] const std::string& ResolveDamageSourceNameLocked(
        const std::uint64_t source_id) noexcept {
        const auto cached = damage_source_names.find(source_id);
        if (cached != damage_source_names.end()) return cached->second;
        static const std::string empty;
        return empty;
    }

    [[nodiscard]] bool ResolveDamageDefinitionDisplayNameLocked(
        const std::uint64_t source_id) noexcept {
        if (source_id == 0 || combat_event_names.contains(source_id) ||
            !display_table_scan_complete || damage_skill_index.table == 0) {
            return combat_event_names.contains(source_id);
        }
        const auto source = damage_source_objects.find(source_id);
        if (source == damage_source_objects.end() || source->second == 0) return false;
        try {
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            if (name_offset < 0) return false;
            const auto try_object = [this, name_offset](
                                        const std::uintptr_t object,
                                        std::string& value) {
                std::uint32_t comparison_index{};
                std::uint32_t number{};
                if (object == 0 ||
                    !ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(name_offset),
                        comparison_index) ||
                    !ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(comparison_index),
                        number)) {
                    return false;
                }
                const auto effect_key = static_cast<std::uint64_t>(comparison_index) |
                    (static_cast<std::uint64_t>(number) << 32U);
                std::string effect_name;
                static_cast<void>(ResolveFNameLocked(
                    comparison_index, number, effect_name));
                std::uint64_t ability_key{};
                if (!FindDamageSkillAbilityLocked(
                        effect_key, effect_name, ability_key) || ability_key == 0) {
                    return false;
                }
                std::string ability_name;
                static_cast<void>(ResolveFNameLocked(
                    static_cast<std::uint32_t>(ability_key & 0xFFFFFFFFU),
                    static_cast<std::uint32_t>(ability_key >> 32U), ability_name));
                return ResolveIndexedDisplayNameLocked(
                    ability_key, ability_name, value) && !value.empty();
            };

            std::string value;
            std::uintptr_t source_class{};
            if ((ReadPointerAt(*memory, source->second,
                     Layout(profile, "object.class"), source_class) &&
                    try_object(source_class, value)) ||
                try_object(source->second, value)) {
                combat_event_names.emplace(source_id, std::move(value));
                MarkCombatEventNameResolvedLocked(source_id);
                return true;
            }
        } catch (...) {
        }
        return false;
    }

    [[nodiscard]] bool ResolveAbilityDisplayNameForClassLocked(
        const std::uintptr_t ability_class,
        std::string& value) noexcept {
        value.clear();
        if (ability_class == 0) return false;
        AnomalyGenerationHandleV1 handle{};
        if (!ObjectHandleLocked(ability_class, handle)) return false;
        const auto cached = ability_display_names.find(handle.id);
        if (cached != ability_display_names.end() && !cached->second.empty()) {
            value = cached->second;
            return true;
        }
        try {
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            if (name_offset < 0) return false;
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            if (!ReadValue(*memory,
                    ability_class + static_cast<std::uintptr_t>(name_offset),
                    comparison_index) ||
                !ReadValue(*memory,
                    ability_class + static_cast<std::uintptr_t>(name_offset) +
                        sizeof(comparison_index), number)) {
                return false;
            }
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            std::string key_text;
            static_cast<void>(ResolveFNameLocked(comparison_index, number, key_text));
            if (!ResolveIndexedDisplayNameLocked(key, key_text, value) || value.empty()) {
                value.clear();
                return false;
            }
            ability_display_names.emplace(handle.id, value);
            return true;
        } catch (...) {
            value.clear();
            return false;
        }
    }

    enum class DamageSourceMappingKind : std::uint8_t {
        None,
        SavedTriggerSkillCdo,
        TriggerAbilityHandle,
    };

    [[nodiscard]] bool ResolveDamageSourceAbilityClassLocked(
        const PendingDamageSourceMapping& pending,
        std::uintptr_t& ability_class,
        DamageSourceMappingKind& mapping_kind) noexcept {
        ability_class = 0;
        mapping_kind = DamageSourceMappingKind::None;
        if (pending.saved_skill_cdo != 0 &&
            ReadPointerAt(*memory, pending.saved_skill_cdo,
                Layout(profile, "object.class"), ability_class) &&
            ability_class != 0 && combat_skill_discovery.gameplay_ability_class != 0 &&
            IsClassDerivedFromLocked(
                ability_class, combat_skill_discovery.gameplay_ability_class)) {
            mapping_kind = DamageSourceMappingKind::SavedTriggerSkillCdo;
            return true;
        }
        if (pending.attacker_is_player && pending.active_spec_handle != 0 && skills_available) {
            const auto found = std::ranges::find_if(
                skills, [&](const SkillRecord& record) {
                    return record.spec_handle == pending.active_spec_handle;
                });
            if (found != skills.end()) {
                ability_class = found->ability_class_pointer;
                mapping_kind = DamageSourceMappingKind::TriggerAbilityHandle;
                return ability_class != 0;
            }
        }
        if (pending.attacker_is_player && pending.active_spec_handle == 0 && skills_available) {
            const SkillRecord* active{};
            for (const auto& record : skills) {
                if ((record.flags & ANOMALY_NTE_SKILL_V1_ACTIVE) == 0) continue;
                if (active != nullptr) {
                    active = nullptr;
                    break;
                }
                active = &record;
            }
            if (active != nullptr) {
                ability_class = active->ability_class_pointer;
                mapping_kind = DamageSourceMappingKind::TriggerAbilityHandle;
                return ability_class != 0;
            }
        }
        return false;
    }

    void CompleteDamageSourceNamesForAbilityLocked(
        const std::uint64_t ability_class_id,
        const std::string_view name) noexcept {
        if (name.empty()) return;
        for (auto& [source_id, mapping] : damage_source_ability_classes) {
            if (!mapping.name_pending || mapping.class_handle.id != ability_class_id ||
                combat_event_names.contains(source_id)) {
                continue;
            }
            combat_event_names.emplace(source_id, std::string(name));
            mapping.name_pending = false;
            ++delayed_damage_name_completion_count;
            MarkCombatEventNameResolvedLocked(source_id);
        }
    }

    void QueueDamageSourceAbilityNameLocked(
        const std::uint64_t source_id,
        const std::uintptr_t saved_skill_cdo,
        const std::int32_t active_spec_handle,
        const bool attacker_is_player) noexcept {
        if (source_id == 0 || combat_event_names.contains(source_id)) return;
        if (
            damage_source_ability_classes.contains(source_id) ||
            observed_damage_source_mappings.contains(source_id)) {
            return;
        }
        try {
            observed_damage_source_mappings.insert(source_id);
            pending_damage_source_mappings.push_back({
                source_id, saved_skill_cdo, active_spec_handle, attacker_is_player, 0});
        } catch (...) {
            observed_damage_source_mappings.erase(source_id);
        }
    }

    void ResolveNextDamageSourceMappingLocked() noexcept {
        constexpr std::size_t kMaximumMappingsPerTick = 1;
        constexpr std::uint8_t kMaximumMappingAttempts = 8;
        const auto mapping_count = (std::min)(
            pending_damage_source_mappings.size(), kMaximumMappingsPerTick);
        for (std::size_t mapping_index{}; mapping_index < mapping_count;
             ++mapping_index) {
            auto pending = pending_damage_source_mappings.front();
            if (ResolveDamageDefinitionDisplayNameLocked(pending.source_id)) {
                pending_damage_source_mappings.pop_front();
                continue;
            }
            std::uintptr_t ability_class{};
            DamageSourceMappingKind mapping_kind{};
            if (!ResolveDamageSourceAbilityClassLocked(
                    pending, ability_class, mapping_kind)) {
                // A player hit may arrive before the first skill frame. Keep the
                // source queued until that frame supplies the handle-to-class map;
                // NPC hits do not depend on the player's skill cache.
                if (pending.attacker_is_player && !skills_available) return;
                pending_damage_source_mappings.pop_front();
                if (pending.attempts + 1U < kMaximumMappingAttempts) {
                    ++pending.attempts;
                    pending_damage_source_mappings.push_back(pending);
                } else {
                    ++damage_source_mapping_failure_count;
                }
                continue;
            }
            pending_damage_source_mappings.pop_front();
            AnomalyGenerationHandleV1 class_handle{};
            if (!ObjectHandleLocked(ability_class, class_handle)) {
                if (pending.attempts + 1U < kMaximumMappingAttempts) {
                    ++pending.attempts;
                    pending_damage_source_mappings.push_back(pending);
                } else {
                    ++damage_source_mapping_failure_count;
                }
                continue;
            }
            if (mapping_kind == DamageSourceMappingKind::SavedTriggerSkillCdo) {
                ++saved_trigger_skill_mapping_count;
            } else if (mapping_kind == DamageSourceMappingKind::TriggerAbilityHandle) {
                ++trigger_ability_handle_mapping_count;
            }
            auto [mapping, inserted] = damage_source_ability_classes.emplace(
                pending.source_id,
                DamageSourceAbilityMapping{ability_class, class_handle, true, 0, 0});
            if (!inserted) continue;
            const auto name = ability_display_names.find(class_handle.id);
            if (name != ability_display_names.end() && !name->second.empty()) {
                combat_event_names.emplace(pending.source_id, name->second);
                mapping->second.name_pending = false;
                MarkCombatEventNameResolvedLocked(pending.source_id);
            } else {
                std::string resolved_name;
                if (ResolveAbilityDisplayNameForClassLocked(
                        ability_class, resolved_name)) {
                    CompleteDamageSourceNamesForAbilityLocked(
                        class_handle.id, resolved_name);
                } else {
                    mapping->second.name_attempts = 1;
                    mapping->second.next_name_sequence =
                        tick_sequence.load(std::memory_order_relaxed) + 15U;
                }
            }
        }
    }

    void ResolveNextPendingDamageSourceNameLocked(
        const std::uint64_t sequence) noexcept {
        if ((display_table_loaded_mask & 0x04U) == 0) {
            display_name_demand.store(true, std::memory_order_release);
            return;
        }
        constexpr std::uint8_t kMaximumNameAttempts = 8;
        constexpr std::uint64_t kRetryInterval = 15;
        for (auto& [source_id, mapping] : damage_source_ability_classes) {
            static_cast<void>(source_id);
            if (!mapping.name_pending ||
                mapping.name_attempts >= kMaximumNameAttempts ||
                sequence < mapping.next_name_sequence) {
                continue;
            }
            std::string resolved_name;
            if (ResolveAbilityDisplayNameForClassLocked(
                    mapping.class_pointer, resolved_name)) {
                CompleteDamageSourceNamesForAbilityLocked(
                    mapping.class_handle.id, resolved_name);
            } else {
                ++mapping.name_attempts;
                mapping.next_name_sequence = sequence + kRetryInterval;
            }
            return;
        }
    }

    void ProcessCharacterDamageLocked(
        const float damage,
        const std::int32_t source_index,
        const std::int32_t source_serial,
        const std::uintptr_t victim,
        const std::uintptr_t attacker,
        const std::uintptr_t saved_skill_cdo,
        const std::int32_t active_spec_handle,
        const bool critical,
        const bool critical_valid,
        const bool partial,
        const std::uint64_t capture_tick_sequence) noexcept {
        const auto drop = [this]() noexcept {
            ++damage_dropped_count;
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
        };
        if (!started.load(std::memory_order_acquire) || victim == 0 ||
            !SemanticFeatureAvailable("nte.combat") || world_pointer == 0 ||
            !std::isfinite(damage) || damage < 0.0F ||
            damage > static_cast<float>((std::numeric_limits<std::int64_t>::max)())) {
            drop();
            return;
        }
        try {
            bool is_critical = critical;
            AnomalyNteDamageEventV1 event{};
            event.flags = ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT;
            if (is_critical) event.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL;
            if (critical_valid) event.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID;
            event.tick_sequence = capture_tick_sequence;
            event.world = {1, world_generation};
            if (attacker != 0 && !ObjectHandleLocked(attacker, event.attacker)) {
                ++damage_attacker_resolution_failure_count;
            }
            if (!ObjectHandleLocked(victim, event.victim)) {
                ++damage_victim_resolution_failure_count;
                drop();
                return;
            }
            if (attacker != 0 && event.attacker.id != 0) {
                QueueCombatParticipantNameLocked(event.attacker, attacker);
            }
            QueueCombatParticipantNameLocked(event.victim, victim);
            event.source_id = ResolveDamageEventSourceLocked(source_index, source_serial);
            if (event.source_id == 0) ++damage_source_resolution_failure_count;
            if (event.source_id != 0) {
                QueueDamageSourceAbilityNameLocked(
                    event.source_id, saved_skill_cdo, active_spec_handle,
                    attacker == player_pawn);
            }
            event.final_damage = static_cast<std::int64_t>(std::llround(damage));
            RecordDamageEventLocked(event);
            AnomalyNteCombatEventV1 combat_event{};
            combat_event.kind = ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE;
            combat_event.tick_sequence = event.tick_sequence;
            combat_event.world = event.world;
            combat_event.source = event.attacker;
            combat_event.target = event.victim;
            combat_event.final_value = event.final_damage;
            combat_event.value = event.final_damage;
            combat_event.name_id = event.source_id;
            if (partial) combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_PARTIAL;
            if (critical_valid) combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID;
            if (is_critical) {
                combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL;
            }
            if (event.source_id != 0) {
                if (combat_event_names.contains(event.source_id)) {
                    combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
                }
            }
            if (!MergeCombatDamageLocked(combat_event)) {
                RecordCombatEventLocked(combat_event);
            }
            ++damage_captured_event_count;
        } catch (...) {
            drop();
        }
    }

    void CaptureCharacterDamageBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint64_t capture_tick_sequence) noexcept {
        NteCharacterDamageCapture damage;
        if (!ReadCaptureBytes(parameters, 0, damage)) {
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool critical = damage.critical.critical ||
            DamageTagsContainCriticalLocked(damage.tags);
        const bool critical_valid = damage.critical.valid || critical;
        ProcessCharacterDamageLocked(
            damage.damage, damage.source_index, damage.source_serial,
            damage.victim, damage.attacker, damage.saved_skill_cdo, damage.active_spec_handle,
            critical, critical_valid, damage.tags.partial || !critical_valid,
            capture_tick_sequence);
    }

    void RefreshEntityCache(
        std::uint64_t sequence,
        bool all_levels,
        std::shared_ptr<const EntityFrameCache>& destination,
        std::uint64_t& generation) noexcept {
        const auto invalidate = [&]() noexcept {
            if (destination) ++generation;
            destination.reset();
        };
        try {
            std::vector<EntityRecord> next;
            std::unordered_map<std::uint64_t, std::string> class_names;
            std::unordered_map<std::uint64_t, std::string> entity_names;
            const auto actors_offset = Layout(profile, "level.actors");
            const auto maximum = Layout(profile, "entities.maxCount", 16384);
            const auto maximum_levels = Layout(profile, "entities.maxLevels", 4096);
            std::int64_t actor_count_offset{};
            if ((!all_levels && !SemanticFeatureAvailable("nte.entities")) ||
                (all_levels && !NteActorsLayoutAvailable()) || maximum <= 0 ||
                maximum > (std::numeric_limits<std::int32_t>::max)() ||
                (all_levels && (maximum_levels <= 0 || maximum_levels > 4096)) ||
                !AddLayoutOffset(
                    actors_offset, static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                    actor_count_offset)) {
                invalidate();
                return;
            }

            bool partial{!player_esp_available};
            std::vector<std::uintptr_t> levels;
            const auto levels_offset = Layout(profile, "world.levels");
            if (all_levels) {
                std::uintptr_t levels_array{};
                std::uintptr_t levels_count_address{};
                std::int32_t level_count{};
                if (!ReadPointerAt(*memory, world_pointer, levels_offset, levels_array) ||
                    !AddAddress(
                        world_pointer,
                        levels_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                        levels_count_address) ||
                    !ReadValue(*memory, levels_count_address, level_count) || level_count < 0 ||
                    level_count > maximum_levels || (level_count != 0 && levels_array == 0)) {
                    invalidate();
                    return;
                }
                levels.reserve(static_cast<std::size_t>(level_count));
                for (std::int32_t index = 0; index < level_count; ++index) {
                    std::uintptr_t level_slot{};
                    std::uintptr_t level{};
                    if (!AddAddress(
                            levels_array,
                            static_cast<std::int64_t>(index) *
                                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                            level_slot) ||
                        !ReadValue(*memory, level_slot, level)) {
                        partial = true;
                        continue;
                    }
                    if (level != 0) levels.push_back(level);
                }
            } else {
                std::uintptr_t level{};
                if (!ReadPointerAt(
                        *memory, world_pointer, Layout(profile, "world.persistentLevel"), level)) {
                    invalidate();
                    return;
                }
                levels.push_back(level);
            }

            std::unordered_set<std::uintptr_t> seen_actors;
            if (all_levels) {
                next.reserve(static_cast<std::size_t>(maximum));
                class_names.reserve(256);
                entity_names.reserve(static_cast<std::size_t>(maximum));
                seen_actors.reserve(static_cast<std::size_t>(maximum));
            }
            const auto finite = [](const std::array<double, 3>& values) {
                return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
            };
            std::uint64_t fallback_index{};
            std::int64_t total_actor_slots{};
            for (const std::uintptr_t level : levels) {
                std::uintptr_t actor_array{};
                std::uintptr_t count_address{};
                std::int32_t actor_count{};
                if (!ReadPointerAt(*memory, level, actors_offset, actor_array) ||
                    !AddAddress(level, actor_count_offset, count_address) ||
                    !ReadValue(*memory, count_address, actor_count) || actor_count < 0 ||
                    actor_count > maximum - total_actor_slots ||
                    (actor_count != 0 && actor_array == 0)) {
                    invalidate();
                    return;
                }
                total_actor_slots += actor_count;
                if (!all_levels) {
                    next.reserve(static_cast<std::size_t>(actor_count));
                    class_names.reserve(static_cast<std::size_t>(actor_count));
                    entity_names.reserve(static_cast<std::size_t>(actor_count));
                }
                for (std::int32_t index = 0; index < actor_count; ++index, ++fallback_index) {
                    std::uintptr_t actor_slot{};
                    std::uintptr_t actor{};
                    if (!AddAddress(
                            actor_array,
                            static_cast<std::int64_t>(index) *
                                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                            actor_slot) ||
                        !ReadValue(*memory, actor_slot, actor)) {
                        partial = true;
                        continue;
                    }
                    if (actor == 0 || (all_levels && !seen_actors.insert(actor).second)) continue;
                    std::uintptr_t root{};
                    std::uintptr_t pointer_address{};
                    std::uintptr_t bounds_center_address{};
                    std::uintptr_t bounds_extent_address{};
                    if (!AddAddress(
                            actor, Layout(profile, "actor.rootComponent"), pointer_address) ||
                        !ReadValue(*memory, pointer_address, root)) {
                        partial = true;
                        continue;
                    }
                    // PersistentLevel commonly contains actors without a scene root. They
                    // are not renderable entity candidates, but their presence does not
                    // make the rest of the actor-array sample incomplete.
                    if (root == 0) continue;
                    if (!AddAddress(
                            root, Layout(profile, "sceneComponent.boundsOrigin"),
                            bounds_center_address) ||
                        !AddAddress(
                            root, Layout(profile, "sceneComponent.boundsExtent"),
                            bounds_extent_address)) {
                        partial = true;
                        continue;
                    }

                    EntityRecord entity;
                    entity.actor = actor;
                    if (!memory->Read(
                            bounds_center_address, entity.bounds_center.data(),
                            sizeof(entity.bounds_center)) ||
                        !memory->Read(
                            bounds_extent_address, entity.bounds_extent.data(),
                            sizeof(entity.bounds_extent))) {
                        partial = true;
                        continue;
                    }
                    // Successfully-read zero, non-finite, or otherwise unusable bounds
                    // describe a non-renderable actor rather than a truncated frame.
                    if (!finite(entity.bounds_center) || !finite(entity.bounds_extent) ||
                        std::ranges::any_of(entity.bounds_extent, [](double value) {
                            return value <= 0.0 || value > 1000000000.0;
                        })) {
                        continue;
                    }

                    std::int32_t entity_index{-1};
                    std::int32_t class_index{-1};
                    std::uintptr_t address{};
                    if (AddAddress(actor, Layout(profile, "object.internalIndex"), address)) {
                        static_cast<void>(ReadValue(*memory, address, entity_index));
                    }
                    if (entity_index >= 0) {
                        std::uintptr_t registered_object{};
                        std::uint32_t registered_serial{};
                        entity.object_index = static_cast<std::uint32_t>(entity_index);
                        if (ReadObjectSlot(
                                *memory, object_registry, entity.object_index,
                                registered_object, registered_serial) &&
                            registered_object == actor && registered_serial != 0) {
                            entity.object_serial = registered_serial;
                            entity.object_identity_available = true;
                        }
                    }
                    if (AddAddress(actor, Layout(profile, "object.nameOffset"), address)) {
                        static_cast<void>(ReadValue(*memory, address, entity.entity_name_id));
                    }

                    // Class/name/index metadata is optional. Stable slot/name fallbacks
                    // keep otherwise valid geometry usable when metadata is absent.
                    std::uintptr_t class_object{};
                    if (AddAddress(actor, Layout(profile, "object.class"), address)) {
                        static_cast<void>(ReadValue(*memory, address, class_object));
                    }
                    if (class_object != 0) {
                        entity.class_object = class_object;
                        if (AddAddress(
                                class_object, Layout(profile, "object.internalIndex"), address)) {
                            static_cast<void>(ReadValue(*memory, address, class_index));
                        }
                        if (AddAddress(
                                class_object, Layout(profile, "object.nameOffset"), address)) {
                            static_cast<void>(ReadValue(*memory, address, entity.class_name_id));
                        }
                    }
                    entity.entity_id = entity_index >= 0
                        ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(entity_index)) + 1
                        : fallback_index + 1;
                    entity.class_id = class_index >= 0
                        ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(class_index)) + 1
                        : static_cast<std::uint64_t>(entity.class_name_id) + 1;

                    if (entity.class_name_id != 0 && !class_names.contains(entity.class_id)) {
                        if (std::string name = ResolveNameForScanLocked(entity.class_name_id);
                            !name.empty()) {
                            class_names.emplace(entity.class_id, std::move(name));
                        }
                    }
                    if (entity.entity_name_id != 0 && !entity_names.contains(entity.entity_id)) {
                        if (std::string name = ResolveNameForScanLocked(entity.entity_name_id);
                            !name.empty()) {
                            entity_names.emplace(entity.entity_id, std::move(name));
                        }
                    }

                    std::uint8_t mobility{};
                    if (AddAddress(root, Layout(profile, "sceneComponent.mobility"), address) &&
                        ReadValue(*memory, address, mobility)) {
                        if (mobility == 0) entity.flags |= ANOMALY_NTE_ENTITY_V1_STATIC;
                        else if (mobility == 1) entity.flags |= ANOMALY_NTE_ENTITY_V1_STATIONARY;
                        else if (mobility == 2) entity.flags |= ANOMALY_NTE_ENTITY_V1_MOVABLE;
                    } else {
                        partial = true;
                    }
                    if (actor == player_pawn) entity.flags |= ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER;
                    next.push_back(entity);
                }
            }
            auto cache = std::make_shared<EntityFrameCache>();
            cache->entities = std::move(next);
            cache->class_names = std::move(class_names);
            cache->entity_names = std::move(entity_names);
            cache->generation = ++generation;
            cache->sequence = sequence;
            cache->camera_position = camera_position;
            cache->camera_rotation = camera_rotation;
            cache->camera_horizontal_fov = camera_horizontal_fov;
            cache->partial = partial;
            destination = std::move(cache);
        } catch (...) {
            invalidate();
        }
    }

    void RefreshEntities(std::uint64_t sequence) noexcept {
        entity_attempt_sequence = sequence;
        const auto current = entity_frame_cache;
        const auto previous = previous_entity_frame_cache;
        RefreshEntityCache(
            sequence, false, entity_frame_cache, entity_generation);
        if (entity_frame_cache != current) {
            previous_entity_frame_cache = current ? current : previous;
        }
    }

    void RefreshActors(std::uint64_t sequence) noexcept {
        RefreshEntityCache(sequence, true, actor_frame_cache, actor_generation);
        if (actor_frame_cache) actor_world_generation = world_generation;
    }

    static AnomalyStatusV1 ANOMALY_CALL BuildId(
        void* user, char* destination, std::size_t* size) noexcept {
        return CopyString(static_cast<State*>(user)->fingerprint.id, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL ProfileHash(
        void* user, char* destination, std::size_t* size) noexcept {
        return CopyString(static_cast<State*>(user)->profile.source_hash, destination, size);
    }

    static std::uint32_t ANOMALY_CALL FeatureStateThunk(
        void* user, AnomalyStringViewV1 id) noexcept {
        return static_cast<State*>(user)->FeatureState(id);
    }

    static std::uint32_t ANOMALY_CALL GameThreadIdThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)
            ? static_cast<std::uint32_t>(state.game_thread_id.load(std::memory_order_acquire))
            : 0;
    }

    static std::uint64_t ANOMALY_CALL TickSequenceThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)
            ? state.tick_sequence.load(std::memory_order_acquire)
            : 0;
    }

    static int ANOMALY_CALL IsGameThreadThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)) {
            return 0;
        }
        const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
        return expected != 0 && expected == GetCurrentThreadId() ? 1 : 0;
    }

    AnomalyStatusV1 ResolveNameIdLocked(
        std::uint32_t name_id, char* destination, std::size_t* size) const noexcept {
        const auto* names = Symbol("ue5.FNamePool");
        if (names == nullptr || !names->Available()) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        const auto blocks_offset = Layout(profile, "names.blocksOffset");
        const auto block_bits = Layout(profile, "names.blockBits", 16);
        const auto entry_stride = Layout(profile, "names.entryStride", 2);
        const auto length_shift = Layout(profile, "names.headerLengthShift", 6);
        if (blocks_offset < 0 || block_bits <= 0 || block_bits >= 31 || entry_stride <= 0 ||
            length_shift <= 0 || length_shift >= 16) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name layout is unavailable");
        }
        const std::uint32_t block_index = name_id >> block_bits;
        const std::uint32_t entry_offset = name_id & ((1U << block_bits) - 1U);
        std::uintptr_t block_slot{};
        std::uintptr_t block{};
        std::int64_t indexed_blocks_offset{};
        const auto block_stride = static_cast<std::uint64_t>(block_index) *
            sizeof(std::uintptr_t);
        if (block_stride > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            !AddLayoutOffset(
                blocks_offset, static_cast<std::int64_t>(block_stride), indexed_blocks_offset)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name block layout is invalid");
        }
        if (!AddAddress(
                names->address,
                indexed_blocks_offset,
                block_slot) || !ReadValue(*memory, block_slot, block) || block == 0) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "name block is unavailable");
        }
        std::uintptr_t entry{};
        if (entry_offset != 0 && entry_stride >
                (std::numeric_limits<std::int64_t>::max)() /
                    static_cast<std::int64_t>(entry_offset)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name entry layout is invalid");
        }
        const auto entry_distance = static_cast<std::int64_t>(entry_offset) * entry_stride;
        if (!AddAddress(block, entry_distance, entry)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        std::uint16_t header{};
        if (!ReadValue(*memory, entry, header)) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const std::size_t length = header >> length_shift;
        const bool wide = (header & 1U) != 0;
        if (length == 0 || length > 1024) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "invalid name length");
        }
        if (!wide) {
            std::string value(length, '\0');
            if (!memory->Read(entry + sizeof(header), value.data(), value.size())) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            }
            return CopyString(value, destination, size);
        }
        std::vector<wchar_t> wide_value(length);
        if (!memory->Read(entry + sizeof(header), wide_value.data(), wide_value.size() * sizeof(wchar_t))) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_value.data(), static_cast<int>(wide_value.size()),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "wide name decode failed");
        std::string value(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide_value.data(), static_cast<int>(wide_value.size()),
                value.data(), required, nullptr, nullptr) != required) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "wide name decode failed");
        }
        return CopyString(value, destination, size);
    }

    // How far back through the pool a name lookup walks before giving up.
    static constexpr std::uint32_t kMaximumScannedNameBlocks = 16;
    // The pool's block table holds this many slots, which UE fixes.
    static constexpr std::uint32_t kMaximumNameBlocks = 8192;

    // Finds the id a name is registered under by walking the pool's newest entries backwards.
    // Ids are handed out in registration order, so the id a name carries differs from run to
    // run and cannot be recorded; a type whose name is known can only be named again by asking
    // the pool for it. The walk starts at the newest block -- a reflected type is registered
    // with the content that declares it, so it sits near the newest entries -- and stops after
    // kMaximumScannedNameBlocks blocks. Without that bound a name that is not in the pool would
    // walk every name the process ever registered, which is minutes of game-thread time.
    AnomalyStatusV1 FindNameIdLocked(
        const std::string_view name, std::uint32_t& name_id) const noexcept {
        name_id = 0;
        if (name.empty() || name.size() > 1024) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "name is invalid");
        }
        const auto* names = Symbol("ue5.FNamePool");
        if (names == nullptr || !names->Available()) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        const auto blocks_offset = Layout(profile, "names.blocksOffset");
        const auto block_bits = Layout(profile, "names.blockBits", 16);
        const auto entry_stride = Layout(profile, "names.entryStride", 2);
        const auto length_shift = Layout(profile, "names.headerLengthShift", 6);
        if (blocks_offset < 0 || block_bits <= 0 || block_bits >= 31 || entry_stride <= 0 ||
            length_shift <= 0 || length_shift >= 16) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name layout is unavailable");
        }
        const auto entries_per_block = static_cast<std::uint64_t>(1) << block_bits;
        const auto block_at = [&](const std::uint32_t block_index, std::uintptr_t& block) {
            std::int64_t slot_offset{};
            std::uintptr_t block_slot{};
            return AddLayoutOffset(
                       blocks_offset,
                       static_cast<std::int64_t>(block_index) * sizeof(std::uintptr_t),
                       slot_offset) &&
                AddAddress(names->address, slot_offset, block_slot) &&
                ReadValue(*memory, block_slot, block) && block != 0;
        };
        // The newest block is found from the block table itself rather than from the allocator
        // cursor that precedes it: that cursor read back as zero, which limited the search to
        // the pool's first block and made every lookup miss. The table is the one the forward
        // direction reads, so a block found here is known to be the one holding its entries.
        std::uint32_t newest_block{};
        bool found_newest = false;
        for (std::uint32_t block_index = kMaximumNameBlocks; block_index-- > 0;) {
            std::uintptr_t block{};
            if (!block_at(block_index, block)) continue;
            newest_block = block_index;
            found_newest = true;
            break;
        }
        if (!found_newest) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "the pool holds no blocks");
        const auto lowest_block = newest_block >= kMaximumScannedNameBlocks
            ? newest_block - kMaximumScannedNameBlocks + 1
            : 0U;
        for (std::uint32_t block_index = newest_block;; --block_index) {
            std::uintptr_t block{};
            if (!block_at(block_index, block)) {
                if (block_index == lowest_block) break;
                continue;
            }
            // Only the newest block may be partly filled, and its used span is the entries that
            // were written -- found by walking down to the first one that was not, because
            // entries are handed out in order. Every block before it holds a full span.
            auto entries = entries_per_block;
            if (block_index == newest_block) {
                entries = 0;
                for (std::uint64_t entry_index = entries_per_block; entry_index-- > 0;) {
                    std::uintptr_t entry{};
                    if (!AddAddress(
                            block, static_cast<std::int64_t>(entry_index * entry_stride), entry)) {
                        break;
                    }
                    std::uint16_t header{};
                    if (!ReadValue(*memory, entry, header)) break;
                    if (header != 0) {
                        entries = entry_index + 1;
                        break;
                    }
                }
                if (entries == 0) {
                    if (block_index == lowest_block) break;
                    continue;
                }
            }
            for (std::uint64_t entry_index = entries; entry_index-- > 0;) {
                std::uintptr_t entry{};
                if (!AddAddress(
                        block, static_cast<std::int64_t>(entry_index * entry_stride), entry)) {
                    break;
                }
                std::uint16_t header{};
                if (!ReadValue(*memory, entry, header)) break;
                // The length sits in the header, so an entry that cannot match is skipped
                // without reading its characters.
                if (static_cast<std::size_t>(header >> length_shift) != name.size()) continue;
                std::string value(name.size(), '\0');
                if (!memory->Read(entry + sizeof(header), value.data(), value.size())) break;
                if (value != name) continue;
                name_id = (block_index << block_bits) | static_cast<std::uint32_t>(entry_index);
                return Status(ANOMALY_STATUS_V1_OK);
            }
            if (block_index == lowest_block) break;
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no entry in the pool spells the name");
    }

    std::string ResolveNameSnapshotLocked(std::uint32_t name_id) const {
        if (name_id == 0) return {};
        std::size_t size{};
        if (ResolveNameIdLocked(name_id, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
            size <= 1 || size > 1024) {
            return {};
        }
        std::string value(size, '\0');
        if (ResolveNameIdLocked(name_id, value.data(), &size).code != ANOMALY_STATUS_V1_OK) {
            return {};
        }
        value.resize(size - 1);
        return value;
    }

    // 仅供全量扫描使用：FName 的 comparison index 在进程内稳定，同一 name_id 永远对应
    // 同一个字符串，因此把解码结果记下来。全关卡扫描会为每个 actor 解析一次实体名
    // （数千个互不相同的 name_id），逐次解码宽字符名的总代价超过一秒；记忆化之后只有
    // 首次扫描需要真正解码。失败结果不缓存，那通常意味着布局尚未就绪，应当重试。
    std::string ResolveNameForScanLocked(std::uint32_t name_id) const {
        if (name_id == 0) return {};
        if (const auto cached = name_snapshot_cache.find(name_id);
            cached != name_snapshot_cache.end()) {
            return cached->second;
        }
        std::string value = ResolveNameSnapshotLocked(name_id);
        if (!value.empty()) name_snapshot_cache.emplace(name_id, value);
        return value;
    }

    [[nodiscard]] bool ResolveFTextBytesLocked(
        const std::array<std::uint8_t, 16>& ftext_bytes,
        std::string& value) const {
        value = ReadUe5FTextUtf8(profile, resolution, *memory, ftext_bytes);
        return !value.empty();
    }

    [[nodiscard]] bool EnsureRegisteredStringTablesBindingLocked() const noexcept {
        auto& binding = registered_string_tables;
        if (binding.object_generation != object_generation) {
            binding = {};
            binding.object_generation = object_generation;
        }
        if (binding.attempted) {
            return binding.function != 0 && binding.receiver != 0;
        }
        if (!ObjectFindAvailable() || !process_event_invoker) return false;
        try {
            std::uintptr_t function{};
            std::uintptr_t library_class{};
            if (!FindExactObjectLocked(
                    L"/Script/Engine.KismetStringTableLibrary.GetRegisteredStringTables",
                    function) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"),
                    library_class) ||
                !ReadPointerAt(*memory, library_class,
                    Layout(profile, "uclass.classDefaultObject"),
                    binding.receiver) ||
                binding.receiver == 0) {
                return false;
            }
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!ReadValue(*memory,
                    function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_offset) ||
                num_parms != 1 || parms_size != 16 || return_offset != 0 ||
                parms_size > binding.parameters.size()) {
                binding.receiver = 0;
                return false;
            }
            binding.parms_size = parms_size;
            binding.return_offset = return_offset;
            binding.function = function;
            binding.attempted = true;
            return true;
        } catch (...) {
            binding.function = 0;
            binding.receiver = 0;
            binding.attempted = false;
            return false;
        }
    }

    [[nodiscard]] bool ResolveRegisteredStringTableIdLocked(
        const std::string_view short_name,
        std::uint64_t& table_id) const noexcept {
        table_id = 0;
        if (short_name.empty() ||
            GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire) ||
            !EnsureRegisteredStringTablesBindingLocked()) {
            return false;
        }
        auto& binding = registered_string_tables;
        std::memset(binding.parameters.data(), 0, binding.parms_size);
        try {
            if (!ReadableRange(*memory, binding.receiver, 0x20U) ||
                !ReadableRange(*memory, binding.function, 0x20U) ||
                !InvokeNativeProcessEventLocked(
                    binding.receiver, binding.function,
                    binding.parameters.data(), binding.parms_size)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        const auto data_offset = Layout(profile, "tarray.data");
        const auto num_offset = Layout(profile, "tarray.num");
        const auto max_offset = Layout(profile, "tarray.max");
        if (data_offset < 0 || num_offset < 0 || max_offset < 0 ||
            binding.return_offset > binding.parms_size ||
            static_cast<std::size_t>(data_offset) + sizeof(std::uintptr_t) >
                binding.parms_size - binding.return_offset ||
            static_cast<std::size_t>(num_offset) + sizeof(std::int32_t) >
                binding.parms_size - binding.return_offset ||
            static_cast<std::size_t>(max_offset) + sizeof(std::int32_t) >
                binding.parms_size - binding.return_offset) {
            return false;
        }
        std::uintptr_t array_data{};
        std::int32_t array_count{};
        std::int32_t array_capacity{};
        std::memcpy(&array_data,
            binding.parameters.data() + binding.return_offset + data_offset,
            sizeof(array_data));
        std::memcpy(&array_count,
            binding.parameters.data() + binding.return_offset + num_offset,
            sizeof(array_count));
        std::memcpy(&array_capacity,
            binding.parameters.data() + binding.return_offset + max_offset,
            sizeof(array_capacity));
        if (array_count < 0 || array_capacity < array_count ||
            array_capacity > 4096 ||
            (array_count != 0 && array_data == 0)) {
            return false;
        }
        const std::string short_owned(short_name);
        int best_score{};
        std::uint64_t best_id{};
        for (std::int32_t index{}; index < array_count; ++index) {
            std::uintptr_t entry{};
            if (!AddAddress(
                    array_data, static_cast<std::int64_t>(index) * 8, entry)) {
                continue;
            }
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            if (!ReadValue(*memory, entry, comparison_index) ||
                !ReadValue(*memory, entry + sizeof(comparison_index), number)) {
                continue;
            }
            std::string name;
            if (!ResolveFNameLocked(comparison_index, number, name) || name.empty()) {
                continue;
            }
            int score{};
            if (name == short_owned) {
                score = 3;
            } else if (name.size() > short_owned.size() &&
                       name.compare(name.size() - short_owned.size() - 1U,
                           short_owned.size() + 1U,
                           "." + short_owned) == 0) {
                score = 2;
            } else if (name.size() > short_owned.size() &&
                       name.compare(name.size() - short_owned.size() - 1U,
                           short_owned.size() + 1U,
                           "/" + short_owned) == 0) {
                score = 1;
            }
            if (score > best_score) {
                best_score = score;
                best_id = static_cast<std::uint64_t>(comparison_index) |
                    (static_cast<std::uint64_t>(number) << 32U);
                if (score == 3) break;
            }
        }
        if (best_score == 0 || best_id == 0) return false;
        table_id = best_id;
        return true;
    }

    [[nodiscard]] bool EnsureStringTableEntryBindingLocked(
        const wchar_t* const table_object_path,
        StringTableEntryBinding& binding) const noexcept {
        if (binding.object_generation != object_generation) {
            binding = {};
            binding.object_generation = object_generation;
            string_table_binding_failure_code = 0;
        }
        if (binding.attempted) {
            return binding.function != 0 && binding.receiver != 0 &&
                binding.table_id != 0;
        }
        ++string_table_binding_attempts;
        if (!ObjectFindAvailable() || !process_event_invoker) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 1;
            return false;
        }
        try {
            std::uintptr_t function{};
            std::uintptr_t library_class{};
            std::uintptr_t table_object{};
            if (!FindExactObjectLocked(
                    L"/Script/Engine.KismetTextLibrary.TextFromStringTable",
                    function) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"),
                    library_class) ||
                !ReadPointerAt(*memory, library_class,
                    Layout(profile, "uclass.classDefaultObject"),
                    binding.receiver) ||
                binding.receiver == 0 ||
                !FindExactObjectLocked(
                    table_object_path, table_object) ||
                table_object == 0) {
                ++string_table_binding_failures;
                string_table_binding_failure_code = 2;
                return false;
            }
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!ReadValue(*memory,
                    function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_offset) ||
                num_parms != 3 || parms_size != 40 || return_offset != 24 ||
                parms_size > binding.parameters.size()) {
                binding.receiver = 0;
                ++string_table_binding_failures;
                string_table_binding_failure_code = 3;
                return false;
            }
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            std::uint32_t table_index{};
            std::uint32_t table_number{};
            if (name_offset < 0 ||
                !ReadValue(*memory,
                    table_object + static_cast<std::uintptr_t>(name_offset),
                    table_index) ||
                !ReadValue(*memory,
                    table_object + static_cast<std::uintptr_t>(name_offset) +
                        sizeof(table_index),
                    table_number) ||
                table_index == 0) {
                binding.receiver = 0;
                ++string_table_binding_failures;
                string_table_binding_failure_code = 4;
                return false;
            }
            std::string table_short_name;
            static_cast<void>(ResolveFNameLocked(
                table_index, table_number, table_short_name));
            std::uint64_t registered_table_id{};
            if (!table_short_name.empty() &&
                ResolveRegisteredStringTableIdLocked(
                    table_short_name, registered_table_id) &&
                registered_table_id != 0) {
                binding.table_id = registered_table_id;
            } else {
                binding.table_id = static_cast<std::uint64_t>(table_index) |
                    (static_cast<std::uint64_t>(table_number) << 32U);
            }
            binding.table_offset = 0;
            binding.key_offset = 8;
            binding.return_offset = return_offset;
            binding.function = function;
            binding.parms_size = parms_size;
            binding.attempted = true;
            string_table_binding_failure_code = 0;
            return true;
        } catch (...) {
            binding.function = 0;
            binding.receiver = 0;
            binding.table_id = 0;
            binding.attempted = false;
            ++string_table_binding_failures;
            string_table_binding_failure_code = 5;
            return false;
        }
    }

    [[nodiscard]] bool EnsureActorStringTableEntryBindingLocked() const noexcept {
        return EnsureStringTableEntryBindingLocked(
            L"/Game/Text/ST_ActorName.ST_ActorName", string_table_entry);
    }

    [[nodiscard]] bool EnsureAbyssStringTableEntryBindingLocked() const noexcept {
        return EnsureStringTableEntryBindingLocked(
            L"/Game/DataAssets/DataAssetSet/Abyss/ST_AbyssBattle.ST_AbyssBattle",
            abyss_string_table_entry);
    }

    [[nodiscard]] bool ResolveStringTableEntryLockedImpl(
        const std::string_view key,
        const wchar_t* const table_object_path,
        StringTableEntryBinding& binding,
        std::string& value) const noexcept {
        value.clear();
        string_table_last_key.assign(key.data(), key.size());
        string_table_last_value.clear();
        ++string_table_call_count;
        if (key.empty() || key.size() > 256 ||
            GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire) ||
            !EnsureStringTableEntryBindingLocked(table_object_path, binding)) {
            if (!key.empty() && key.size() <= 256 &&
                GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
                ++string_table_thread_rejections;
            }
            return false;
        }
        if (binding.table_offset > binding.parameters.size() ||
            sizeof(binding.table_id) >
                binding.parameters.size() - binding.table_offset ||
            binding.key_offset > binding.parameters.size() ||
            sizeof(NativeUtf16StringHeader) >
                binding.parameters.size() - binding.key_offset ||
            binding.parms_size > binding.parameters.size()) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 6;
            return false;
        }
        const int source_size = static_cast<int>(key.size());
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, key.data(), source_size, nullptr, 0);
        if (count <= 0 || count >= (std::numeric_limits<std::int32_t>::max)() - 1) {
            return false;
        }
        std::vector<wchar_t> key_storage(static_cast<std::size_t>(count) + 1U);
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, key.data(), source_size,
                key_storage.data(), count) != count) {
            return false;
        }
        key_storage[static_cast<std::size_t>(count)] = L'\0';
        NativeUtf16StringHeader key_header;
        key_header.data = key_storage.data();
        key_header.count = count + 1;
        key_header.capacity = count + 1;
        std::memset(binding.parameters.data(), 0, binding.parms_size);
        std::memcpy(
            binding.parameters.data() + binding.table_offset,
            &binding.table_id, sizeof(binding.table_id));
        std::memcpy(
            binding.parameters.data() + binding.key_offset,
            &key_header, sizeof(key_header));
        try {
            if (!ReadableRange(*memory, binding.receiver, 0x20U) ||
                !ReadableRange(*memory, binding.function, 0x20U) ||
                !InvokeNativeProcessEventLocked(
                    binding.receiver, binding.function,
                    binding.parameters.data(), binding.parms_size)) {
                ++string_table_binding_failures;
                string_table_binding_failure_code = 7;
                return false;
            }
        } catch (...) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 8;
            return false;
        }
        constexpr std::uint16_t kFTextSize = 16;
        if (binding.return_offset > binding.parms_size ||
            kFTextSize > binding.parms_size - binding.return_offset) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 9;
            return false;
        }
        std::array<std::uint8_t, kFTextSize> ftext_bytes{};
        std::memcpy(ftext_bytes.data(),
            binding.parameters.data() + binding.return_offset, ftext_bytes.size());
        const bool converted = ResolveFTextBytesLocked(ftext_bytes, value);
        const bool decoded = converted && !value.empty() &&
            value != "<MISSING STRING TABLE ENTRY>";
        if (converted && !decoded && value == "<MISSING STRING TABLE ENTRY>") {
            value.clear();
        }
        if (decoded) {
            ++string_table_success_count;
            string_table_last_value = value;
        } else {
            string_table_binding_failure_code = value.empty() ? 10 : 9;
        }
        return decoded;
    }

    [[nodiscard]] bool ResolveStringTableEntryLocked(
        const std::string_view key, std::string& value) const noexcept {
        return ResolveStringTableEntryLockedImpl(
            key, L"/Game/Text/ST_ActorName.ST_ActorName",
            string_table_entry, value);
    }

    [[nodiscard]] bool ResolveAbyssStringTableEntryLocked(
        const std::string_view key, std::string& value) const noexcept {
        return ResolveStringTableEntryLockedImpl(
            key,
            L"/Game/DataAssets/DataAssetSet/Abyss/ST_AbyssBattle.ST_AbyssBattle",
            abyss_string_table_entry, value);
    }

    [[nodiscard]] bool ResolveFTextLocked(
        const std::uintptr_t ftext_address, std::string& value) const {
        std::array<std::uint8_t, 16> bytes{};
        value.clear();
        return ftext_address != 0 &&
            memory->Read(ftext_address, bytes.data(), bytes.size()) &&
            ResolveFTextBytesLocked(bytes, value);
    }

    AnomalyStatusV1 ResolveFTextAddress(
        const std::uintptr_t address, char* destination, std::size_t* size) noexcept {
        if (size == nullptr || address == 0) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        std::scoped_lock lock(mutex);
        if (!ServiceAvailableForPublication(ANOMALY_UE5_NAMES_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 names service is unavailable");
        }
        std::string value;
        try {
            if (!ResolveFTextLocked(address, value)) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FText is unreadable");
            }
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "FText decode failed");
        }
        return CopyString(value, destination, size);
    }

    [[nodiscard]] bool ReadReflectedObjectNameLocked(
        const std::uintptr_t object,
        std::string& name) const {
        std::uintptr_t name_address{};
        std::uint32_t name_id{};
        if (!AddAddress(object, Layout(profile, "object.nameOffset"), name_address) ||
            !ReadValue(*memory, name_address, name_id)) {
            return false;
        }
        std::array<char, 1025> resolved{};
        std::size_t resolved_size = resolved.size();
        if (ResolveNameIdLocked(name_id, resolved.data(), &resolved_size).code !=
                ANOMALY_STATUS_V1_OK ||
            resolved_size <= 1 || resolved_size > resolved.size()) {
            return false;
        }
        name.assign(resolved.data(), resolved_size - 1U);
        return !name.empty();
    }

    [[nodiscard]] bool FindExactObjectLocked(
        const wchar_t* const path,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (path == nullptr || !ObjectFindAvailable() || object_registry.items == 0) return false;
        object = object_lookup(path);
        if (object == 0) return false;

        std::uintptr_t index_address{};
        std::int32_t internal_index{-1};
        if (!AddAddress(object, Layout(profile, "object.internalIndex"), index_address) ||
            !ReadValue(*memory, index_address, internal_index) || internal_index < 0 ||
            static_cast<std::uint64_t>(internal_index) >= object_registry.count) {
            object = 0;
            return false;
        }
        std::uintptr_t slot_object{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(*memory, object_registry, static_cast<std::uint32_t>(internal_index),
                slot_object, serial) || slot_object != object) {
            object = 0;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool DecodeUtf16StringLocked(
        const NativeUtf16StringHeader& native,
        std::string& value) const {
        value.clear();
        if (native.count == 0) return true;
        if (native.data == nullptr || native.count < 1 || native.count > 512 ||
            native.capacity < native.count || native.capacity > 4096) {
            return false;
        }
        std::vector<wchar_t> storage(static_cast<std::size_t>(native.count));
        if (!memory->Read(
                reinterpret_cast<std::uintptr_t>(native.data),
                storage.data(), storage.size() * sizeof(wchar_t))) {
            return false;
        }
        // FString counts include the terminator; the public UTF-8 ABI cannot
        // represent embedded nulls without silently truncating the result.
        if (storage.back() != L'\0' ||
            std::find(storage.begin(), storage.end() - 1, L'\0') != storage.end() - 1) {
            return false;
        }
        const std::size_t length = storage.size() - 1;
        if (length == 0) return true;
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, storage.data(), static_cast<int>(length),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0 || required >
                static_cast<int>(ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES)) {
            return false;
        }
        value.resize(static_cast<std::size_t>(required));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, storage.data(), static_cast<int>(length),
                   value.data(), required, nullptr, nullptr) == required;
    }

    [[nodiscard]] bool ResolveFNameLocked(
        const std::uint32_t comparison_index,
        const std::uint32_t number,
        std::string& value) const {
        if (comparison_index == 0) {
            value = "None";
            return true;
        }
        value = ResolveNameSnapshotLocked(comparison_index);
        if (value.empty()) return false;
        if (number != 0) {
            value += '_';
            value += std::to_string(number - 1U);
        }
        return value.size() <= ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES;
    }

    [[nodiscard]] bool BuildMapLandmarkBindingLocked(
        const std::uintptr_t function,
        MapLandmarkBinding& binding) const {
        try {
            std::string name;
            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadReflectedObjectNameLocked(function, name) || name != "MapIconTransfer" ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer_object) ||
                !ReadReflectedObjectNameLocked(class_object, name) || name != "Function" ||
                !ReadReflectedObjectNameLocked(outer_object, name) || name != "HTPlayerState") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(*memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"),
                    return_value_offset) ||
                num_parms != 2 || parms_size < 17 || parms_size > 128 ||
                return_value_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }

            MapLandmarkBinding candidate;
            candidate.function = function;
            candidate.parms_size = parms_size;
            candidate.object_generation = object_generation;
            bool found_id{};
            bool found_mode{};
            for (std::uint32_t count{}; property != 0 && count < 4; ++count) {
                std::uint32_t property_name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                std::uintptr_t next{};
                if (!ReadValue(*memory, property + Layout(profile, "ffield.name"), property_name_id) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.arrayDim"), array_dim) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.elementSize"), element_size) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.offsetInternal"), offset) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.propertyLinkNext"), next) ||
                    array_dim != 1 || element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(element_size) >
                        parms_size) {
                    return false;
                }
                const std::string property_name = ResolveNameSnapshotLocked(property_name_id);
                if (property_name == "TeleportID" && element_size == 16 && !found_id) {
                    candidate.teleport_id_offset = static_cast<std::uint16_t>(offset);
                    found_id = true;
                } else if (property_name == "InIconTransferFunc" && element_size == 1 && !found_mode) {
                    candidate.transfer_mode_offset = static_cast<std::uint16_t>(offset);
                    found_mode = true;
                } else {
                    return false;
                }
                property = next;
            }
            if (property != 0 || !found_id || !found_mode) return false;
            candidate.available = true;
            binding = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool ResolveGameDataLocked(std::uintptr_t& game_data) const {
        game_data = 0;
        std::uintptr_t function{};
        std::uintptr_t class_object{};
        std::uintptr_t class_default_object{};
        std::uint8_t num_parms{};
        std::uint16_t parms_size{};
        std::uint16_t return_value_offset{};
        if (!FindExactObjectLocked(L"/Script/HTGame.HTGameData.GetGameData", function) ||
            !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), class_object) ||
            !ReadPointerAt(*memory, class_object, Layout(profile, "uclass.classDefaultObject"),
                class_default_object) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"),
                return_value_offset) ||
            num_parms != 1 || parms_size < sizeof(std::uintptr_t) || parms_size > 64 ||
            return_value_offset > parms_size - sizeof(std::uintptr_t) || !process_event_invoker) {
            return false;
        }
        std::array<std::uint8_t, 64> parameters{};
        try {
            if (!ReadableRange(*memory, class_default_object, 0x20U) ||
                !ReadableRange(*memory, function, 0x20U) ||
                !InvokeProcessEventGuarded(
                    process_event_invoker, class_default_object, function,
                    parameters.data(), parms_size)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        std::memcpy(&game_data, parameters.data() + return_value_offset, sizeof(game_data));
        return game_data != 0;
    }

    [[nodiscard]] bool ScanMapLandmarksLocked(MapLandmarkCatalog& catalog) {
        std::uintptr_t map_function{};
        if (!FindExactObjectLocked(
                L"/Script/HTGame.HTPlayerState.MapIconTransfer", map_function) ||
            !BuildMapLandmarkBindingLocked(map_function, map_landmark_binding)) {
            return false;
        }

        std::uintptr_t game_data{};
        std::uintptr_t table{};
        std::uintptr_t row_struct{};
        std::string row_struct_name;
        if (!ResolveGameDataLocked(game_data) ||
            !ReadPointerAt(*memory, game_data, Layout(profile, "gameData.teleportPointDataTable"), table) ||
            !ReadPointerAt(*memory, table, Layout(profile, "dataTable.rowStruct"), row_struct) ||
            !ReadReflectedObjectNameLocked(row_struct, row_struct_name) ||
            row_struct_name != "TeleportPoint") {
            return false;
        }

        const auto row_map = static_cast<std::uintptr_t>(Layout(profile, "dataTable.rowMap"));
        std::uintptr_t data{};
        std::int32_t num{};
        std::int32_t num_free{};
        std::int32_t max{};
        std::uintptr_t flags_data{};
        std::int32_t flags_num{};
        std::int32_t flags_max{};
        if (!ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapData"), data) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapNum"), num) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapNumFree"), num_free) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapMax"), max) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsData"), flags_data) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsNum"), flags_num) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsMax"), flags_max) ||
            data == 0 || num <= 0 || num_free < 0 || num_free > num || max < num ||
            flags_num < num || flags_max < flags_num ||
            max > Layout(profile, "dataTable.maxRows")) {
            return false;
        }

        const auto word_count = static_cast<std::size_t>((flags_num + 31) / 32);
        if (word_count == 0 || word_count > 128) return false;
        std::vector<std::uint32_t> flags(word_count);
        if (flags_data != 0) {
            if (!memory->Read(flags_data, flags.data(), flags.size() * sizeof(std::uint32_t))) return false;
        } else if (word_count > 4 || !memory->Read(
                       table + row_map + Layout(profile, "dataTable.rowMapInlineFlags"),
                       flags.data(), flags.size() * sizeof(std::uint32_t))) {
            return false;
        }

        const auto row_stride = static_cast<std::size_t>(Layout(profile, "dataTable.rowMapElementStride"));
        const auto row_offset = static_cast<std::size_t>(Layout(profile, "dataTable.rowMapRowOffset"));
        const auto belongs_level = static_cast<std::size_t>(Layout(profile, "teleportPoint.belongsLevel"));
        const auto floor_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.floor"));
        const auto position_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.transformTranslation"));
        const auto type_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.type"));
        const auto can_teleport_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.canTeleport"));
        const auto override_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.overrideTransform"));
        const auto destination_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.overrideTranslation"));
        const auto row_bytes = (std::max)({belongs_level + sizeof(NativeUtf16StringHeader),
            floor_offset + sizeof(std::int32_t), position_offset + sizeof(double) * 3U,
            type_offset + sizeof(std::uint32_t), can_teleport_offset + sizeof(std::uint8_t),
            override_offset + sizeof(std::uint8_t), destination_offset + sizeof(double) * 3U});
        if (row_stride < row_offset + sizeof(std::uintptr_t) || row_stride > 256 ||
            row_bytes > 4096) {
            return false;
        }

        const auto slot_count = static_cast<std::size_t>(num);
        std::vector<std::uint8_t> elements(slot_count * row_stride);
        if (!memory->Read(data, elements.data(), elements.size())) return false;

        catalog.entries.clear();
        catalog.entries.reserve(slot_count);
        std::vector<std::uint8_t> row(row_bytes);
        for (std::int32_t index{}; index < num; ++index) {
            if ((flags[static_cast<std::size_t>(index) / 32U] &
                    (1U << (static_cast<std::uint32_t>(index) & 31U))) == 0) {
                continue;
            }
            const auto* const element = elements.data() + static_cast<std::size_t>(index) * row_stride;
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            std::uintptr_t row_address{};
            std::memcpy(&comparison_index, element, sizeof(comparison_index));
            std::memcpy(&number, element + sizeof(comparison_index), sizeof(number));
            std::memcpy(&row_address, element + row_offset, sizeof(row_address));
            if (row_address == 0 || !memory->Read(row_address, row.data(), row.size())) continue;

            std::uint8_t can_teleport{};
            std::uint8_t destination_overridden{};
            MapLandmarkRecord record;
            NativeUtf16StringHeader native_world{};
            std::memcpy(&can_teleport, row.data() + can_teleport_offset, sizeof(can_teleport));
            std::memcpy(&destination_overridden, row.data() + override_offset,
                sizeof(destination_overridden));
            if (can_teleport == 0) continue;
            std::memcpy(&native_world, row.data() + belongs_level, sizeof(native_world));
            std::memcpy(&record.floor, row.data() + floor_offset, sizeof(record.floor));
            std::memcpy(record.world_position.data(), row.data() + position_offset,
                sizeof(record.world_position));
            std::memcpy(&record.point_type, row.data() + type_offset, sizeof(record.point_type));
            if (!ResolveFNameLocked(comparison_index, number, record.teleport_id) ||
                !DecodeUtf16StringLocked(native_world, record.world) ||
                !std::ranges::all_of(record.world_position,
                    [](const double value) { return std::isfinite(value); })) {
                continue;
            }
            record.destination = record.world_position;
            record.destination_overridden = destination_overridden != 0;
            if (record.destination_overridden) {
                std::memcpy(record.destination.data(), row.data() + destination_offset,
                    sizeof(record.destination));
                if (!std::ranges::all_of(record.destination,
                        [](const double value) { return std::isfinite(value); })) {
                    continue;
                }
            }
            catalog.entries.push_back(std::move(record));
        }
        std::ranges::sort(catalog.entries, [](const MapLandmarkRecord& left,
                                              const MapLandmarkRecord& right) {
            return left.world == right.world ? left.teleport_id < right.teleport_id
                                             : left.world < right.world;
        });
        catalog.object_generation = object_generation;
        return true;
    }

    void RefreshMapLandmarksLocked(const std::uint64_t sequence) noexcept {
        try {
            if (!NteMapLandmarksAvailable()) {
                map_landmark_binding = {};
                map_landmark_catalog.reset();
                map_landmark_next_refresh_sequence = sequence + 120U;
                return;
            }
            if (map_landmark_catalog &&
                map_landmark_catalog->object_generation == object_generation) {
                return;
            }
            if (sequence < map_landmark_next_refresh_sequence) return;

            auto catalog = std::make_shared<MapLandmarkCatalog>();
            if (!ScanMapLandmarksLocked(*catalog)) {
                map_landmark_next_refresh_sequence = sequence + 120U;
                return;
            }
            ++map_landmark_catalog_sequence;
            if (map_landmark_catalog_sequence == 0) ++map_landmark_catalog_sequence;
            catalog->sequence = map_landmark_catalog_sequence;
            map_landmark_catalog = std::move(catalog);
            map_landmark_next_refresh_sequence = 0;
        } catch (...) {
            map_landmark_next_refresh_sequence = sequence + 120U;
        }
    }

    [[nodiscard]] bool ReadReflectedFieldClassNameLocked(
        const std::uintptr_t field,
        std::string& name) const {
        std::uintptr_t field_class{};
        std::uintptr_t name_address{};
        std::uint32_t name_id{};
        return ReadPointerAt(
                   *memory, field, Layout(profile, "ffield.class"), field_class) &&
            AddAddress(
                field_class, Layout(profile, "ffieldClass.name"), name_address) &&
            ReadValue(*memory, name_address, name_id) &&
            !(name = ResolveNameSnapshotLocked(name_id)).empty();
    }

    struct ReflectedPropertyInfo {
        std::uintptr_t property{};
        std::uintptr_t next{};
        std::string name;
        std::string type;
        std::int32_t array_dim{};
        std::int32_t element_size{};
        std::int32_t offset{};
        std::uintptr_t structure{};
    };

    [[nodiscard]] bool ReadReflectedPropertyLocked(
        const std::uintptr_t property,
        ReflectedPropertyInfo& info) const {
        std::uintptr_t name_address{};
        std::uintptr_t next_address{};
        std::uint32_t name_id{};
        ReflectedPropertyInfo candidate;
        candidate.property = property;
        if (property == 0 ||
            !AddAddress(property, Layout(profile, "ffield.name"), name_address) ||
            !AddAddress(
                property, Layout(profile, "fproperty.propertyLinkNext"), next_address) ||
            !ReadValue(*memory, name_address, name_id) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.arrayDim"),
                candidate.array_dim) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.elementSize"),
                candidate.element_size) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.offsetInternal"),
                candidate.offset) ||
            !ReadValue(*memory, next_address, candidate.next) ||
            !(candidate.name = ResolveNameSnapshotLocked(name_id)).size() ||
            !ReadReflectedFieldClassNameLocked(property, candidate.type)) {
            return false;
        }
        if (candidate.type == "StructProperty" &&
            !ReadPointerAt(*memory, property, Layout(profile, "fstructProperty.struct"), candidate.structure)) {
            return false;
        }
        info = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool ObjectHandleLocked(
        const std::uintptr_t object,
        AnomalyGenerationHandleV1& handle) const noexcept {
        handle = {};
        std::uintptr_t index_address{};
        std::int32_t index{-1};
        if (object == 0 || object_registry.items == 0 ||
            !AddAddress(object, Layout(profile, "object.internalIndex"), index_address) ||
            !ReadValue(*memory, index_address, index) || index < 0 ||
            static_cast<std::uint64_t>(index) >= object_registry.count) {
            return false;
        }
        std::uintptr_t registered{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(
                *memory, object_registry, static_cast<std::uint32_t>(index),
                registered, serial) || registered != object) {
            return false;
        }
        handle = {
            EncodeObjectHandle(static_cast<std::uint32_t>(index), serial),
            object_generation};
        return true;
    }

    [[nodiscard]] bool ResolveObjectHandleLocked(
        const AnomalyGenerationHandleV1 handle,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (handle.generation != object_generation || handle.id == 0) return false;
        const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
        if (encoded_index == 0) return false;
        const std::uint32_t index = encoded_index - 1U;
        const std::uint32_t serial = static_cast<std::uint32_t>(handle.id >> 32U);
        std::uint32_t observed_serial{};
        return ReadObjectSlot(*memory, object_registry, index, object, observed_serial) &&
            object != 0 && observed_serial == serial;
    }

    [[nodiscard]] bool IsClassDerivedFromLocked(
        std::uintptr_t candidate,
        const std::uintptr_t expected_base) const noexcept {
        constexpr std::size_t kMaximumDepth = 128;
        for (std::size_t depth{};
             candidate != 0 && depth < kMaximumDepth;
             ++depth) {
            if (candidate == expected_base) return true;
            std::uintptr_t next{};
            if (!ReadValue(
                    *memory,
                    candidate + Layout(profile, "ustruct.superStruct"),
                    next) || next == candidate) {
                return false;
            }
            candidate = next;
        }
        return false;
    }

    [[nodiscard]] std::string ObjectPathLocked(
        std::uintptr_t object) const {
        constexpr std::size_t kMaximumDepth = 64;
        std::vector<std::string> names;
        names.reserve(8);
        for (std::size_t depth{}; object != 0 && depth < kMaximumDepth; ++depth) {
            std::string name;
            if (!ReadReflectedObjectNameLocked(object, name)) return {};
            names.push_back(std::move(name));
            std::uintptr_t outer{};
            if (!ReadValue(
                    *memory, object + Layout(profile, "object.outer"), outer) ||
                outer == object) {
                return {};
            }
            object = outer;
        }
        if (object != 0 || names.empty()) return {};
        std::string path;
        for (auto iterator = names.rbegin(); iterator != names.rend(); ++iterator) {
            if (!path.empty()) path.push_back('.');
            path.append(*iterator);
        }
        return path;
    }

    [[nodiscard]] bool ReadReflectedBoolParameterLocked(
        const ReflectedPropertyInfo& property,
        const std::uint16_t parms_size,
        ReflectedBoolParameter& parameter) const noexcept {
        std::uint8_t field_size{};
        std::uint8_t byte_offset{};
        std::uint8_t byte_mask{};
        std::uint8_t field_mask{};
        if (property.type != "BoolProperty" || property.array_dim != 1 ||
            property.element_size != 1 || property.offset < 0 ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.fieldSize"),
                field_size) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.byteOffset"),
                byte_offset) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.byteMask"),
                byte_mask) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.fieldMask"),
                field_mask) ||
            field_size == 0 || byte_offset >= field_size || byte_mask == 0 ||
            field_mask == 0 || (byte_mask & field_mask) != byte_mask ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >= parms_size ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >
                (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        parameter = {
            static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(property.offset) + byte_offset),
            field_mask,
            byte_mask};
        return true;
    }

    [[nodiscard]] bool ResolveVehicleFloatPathLocked(
        const std::uintptr_t root,
        const std::span<const std::string_view> path,
        std::uintptr_t& address) const noexcept {
        if (root == 0 || path.empty()) return false;
        std::uintptr_t current_address = root;
        std::uintptr_t structure{};
        if (!ReadPointerAt(*memory, root, Layout(profile, "object.class"), structure)) return false;
        for (std::size_t index{}; index < path.size(); ++index) {
            ReflectedPropertyInfo property;
            if (!FindReflectedPropertyLocked(structure, path[index], property, true) || property.offset < 0) {
                return false;
            }
            const auto field_address = current_address + static_cast<std::uintptr_t>(property.offset);
            if (index + 1U == path.size()) {
                if (property.type != "FloatProperty" || property.element_size != 4) return false;
                address = field_address;
                return true;
            }
            if (property.type == "ObjectProperty" && property.element_size == 8) {
                if (!ReadValue(*memory, field_address, current_address) || current_address == 0 ||
                    !ReadPointerAt(*memory, current_address, Layout(profile, "object.class"), structure)) {
                    return false;
                }
            } else if (property.type == "StructProperty" && property.element_size > 0 && property.structure != 0) {
                current_address = field_address;
                structure = property.structure;
            } else {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool BuildVehicleBindingLocked(
        const std::uintptr_t function,
        const std::string_view expected_name,
        const std::string_view mode,
        VehicleFunctionBinding& binding) const {
        try {
            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != expected_name) return false;
            std::uintptr_t function_class{};
            std::uintptr_t outer{};
            std::string function_class_name;
            std::string outer_name;
            if (!ReadPointerAt(*memory, function, Layout(profile, "object.class"), function_class) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer) ||
                !ReadReflectedObjectNameLocked(function_class, function_class_name) ||
                !ReadReflectedObjectNameLocked(outer, outer_name) ||
                function_class_name != "Function") return false;
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            std::uintptr_t property{};
            if (!ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"), return_offset) ||
                !ReadPointerAt(*memory, function, Layout(profile, "ustruct.propertyLink"), property)) return false;
            const bool object_return = mode == "ObjectReturn";
            const bool float_return = mode == "FloatReturn";
            const bool float_input = mode == "FloatInput";
            const bool bool_input = mode == "BoolInput";
            const bool no_args = mode == "NoArgs";
            if (no_args) {
                if (num_parms != 0 || parms_size != 0 || return_offset != 0xFFFFu || property != 0) return false;
                binding = {function, 0, 0, 0xFFFFu, false};
                return true;
            } else if (object_return || float_return) {
                const std::uint16_t expected_size = object_return ? 8u : 4u;
                if (num_parms != 1 || parms_size != expected_size || return_offset == 0xFFFFu) return false;
            } else if (float_input || bool_input) {
                const std::uint16_t expected_size = float_input ? 4u : 1u;
                if (num_parms != 1 || parms_size != expected_size || return_offset != 0xFFFFu) return false;
            } else {
                return false;
            }
            ReflectedPropertyInfo info;
            if (!ReadReflectedPropertyLocked(property, info) || info.array_dim != 1 ||
                info.offset < 0 || info.next != 0 || info.name.empty()) {
                // The property-link chain normally ends at null. If the chain has
                // more than one property, reject this narrow vehicle ABI rather than guessing.
                return false;
            }
            const bool expected_return =
                ((object_return && info.type == "ObjectProperty" && info.element_size == 8) ||
                 (float_return && info.type == "FloatProperty" && info.element_size == 4)) &&
                static_cast<std::uint16_t>(info.offset) == return_offset;
            const bool expected_input =
                (float_input && info.type == "FloatProperty" && info.element_size == 4) ||
                (bool_input && info.type == "BoolProperty" && info.element_size == 1);
            if (!expected_return && !expected_input) return false;
            binding = {function, parms_size,
                static_cast<std::uint16_t>(info.offset), return_offset,
                object_return || float_return};
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool FindVehicleFunctionLocked(
        const std::string_view name,
        const std::span<const std::string_view> outers,
        const std::string_view mode,
        VehicleFunctionBinding& binding) const {
        for (const auto outer : outers) {
            std::wstring path = L"/Script/HTGame.";
            path.reserve(path.size() + outer.size() + 1U + name.size());
            for (char c : outer) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            path.push_back(L'.');
            for (char c : name) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            std::uintptr_t function{};
            if (FindExactObjectLocked(path.c_str(), function) &&
                BuildVehicleBindingLocked(function, name, mode, binding)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool RefreshVehicleLocked() noexcept {
        try {
            if (!NteVehicleProfileAvailable() || player_controller == 0 || !process_event_invoker) {
                vehicle_valid = false;
                current_vehicle_object = 0;
                return false;
            }
            if (vehicle_bindings.object_generation != object_generation) {
                vehicle_bindings = {};
                vehicle_bindings.object_generation = object_generation;
            }
            static constexpr std::array<std::string_view, 2> controller_outers{
                "HTPlayerController", "HTPlayerCharacter"};
            static constexpr std::array<std::string_view, 3> vehicle_outers{
                "HTWheeledVehicle", "HTWheeledVehicleDrivable", "HTVehicleMovementComponent"};
            if (vehicle_bindings.current_vehicle.function == 0 &&
                !FindVehicleFunctionLocked("BP_GetCurrentDriveVehicle", controller_outers, "ObjectReturn",
                    vehicle_bindings.current_vehicle)) {
                static constexpr std::array<std::string_view, 2> fallback{
                    "HTPlayerController", "HTPlayerCharacter"};
                if (!FindVehicleFunctionLocked("BP_GetCurrentDirvingVehicle", fallback, "ObjectReturn",
                        vehicle_bindings.current_vehicle)) return false;
            }
            if (vehicle_bindings.speed_kmh.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("GetForwardSpeedKmH", vehicle_outers, "FloatReturn", vehicle_bindings.speed_kmh));
            }
            // Tokky's speed mutation is a reflected SetMaxEngineTorque call on the
            // active ChaosWheeledVehicleMovementComponent.
            if (vehicle_bindings.set_top_speed_ratio.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked(
                    "SetMaxEngineTorque", vehicle_outers, "FloatInput",
                    vehicle_bindings.set_top_speed_ratio));
            }
            if (vehicle_bindings.summon_vehicle.function == 0) {
                static constexpr std::array<std::string_view, 2> summon_outers{
                    "HTPlayerController", "HTPlayerCharacter"};
                if (!FindVehicleFunctionLocked("TestSummonVehicle", summon_outers, "NoArgs", vehicle_bindings.summon_vehicle)) {
                    static_cast<void>(FindVehicleFunctionLocked("CheatSpawnVehicle", summon_outers, "NoArgs", vehicle_bindings.summon_vehicle));
                }
            }
            if (vehicle_bindings.set_wheel_friction.function == 0) {
                static_cast<void>(FindVehicleFunctionLocked("SetEnableWheelFriction", vehicle_outers, "BoolInput", vehicle_bindings.set_wheel_friction));
            }
            alignas(8) std::array<std::uint8_t, 8> out{};
            if (!InvokeProcessEventGuarded(process_event_invoker, player_controller,
                    vehicle_bindings.current_vehicle.function, out.data(), out.size())) return false;
            std::uintptr_t vehicle{};
            std::memcpy(&vehicle, out.data() + vehicle_bindings.current_vehicle.return_offset, sizeof(vehicle));
            if (vehicle == 0 || !ReadableRange(*memory, vehicle, 0x20U)) {
                vehicle_valid = false;
                current_vehicle_object = 0;
                return false;
            }
            current_vehicle_object = vehicle;
            // Tokky's target build reads the active movement component from the vehicle
            // instance and uses its validated base torque as the speed-mutation baseline.
            std::uintptr_t movement_component{};
            if (ReadPointerAt(*memory, current_vehicle_object,
                    Layout(profile, "vehicle.movementComponent"), movement_component)) {
                vehicle_base_movement_component = movement_component;
                float torque{};
                if (ReadValue(*memory, movement_component +
                        Layout(profile, "vehicle.maxEngineTorque"), torque) &&
                    std::isfinite(torque)) {
                    vehicle_base_engine_torque = torque;
                    vehicle_base_engine_torque_valid = true;
                }
            }
            vehicle_valid = true;
            if (vehicle_bindings.speed_kmh.function != 0) {
                alignas(8) std::array<std::uint8_t, 8> speed_bytes{};
                if (InvokeProcessEventGuarded(process_event_invoker, vehicle,
                        vehicle_bindings.speed_kmh.function, speed_bytes.data(), speed_bytes.size())) {
                    float speed{};
                    std::memcpy(&speed, speed_bytes.data() + vehicle_bindings.speed_kmh.return_offset, sizeof(speed));
                    if (std::isfinite(speed)) vehicle_speed_kmh = speed;
                }
            }
            return true;
        } catch (...) {
            vehicle_valid = false;
            current_vehicle_object = 0;
            return false;
        }
    }

    static std::string VehicleLower(std::string value) {
        for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        return value;
    }

    bool FindVehicleClassFromObjectLocked(
        std::uintptr_t object, std::uintptr_t& vehicle_class, std::uint32_t depth = 0) const noexcept {
        vehicle_class = 0;
        if (object == 0 || depth > 2) return false;
        std::uintptr_t object_class{};
        std::string class_name;
        if (!ReadPointerAt(*memory, object, Layout(profile, "object.class"), object_class) ||
            !ReadReflectedObjectNameLocked(object_class, class_name)) return false;
        const std::string lc = VehicleLower(class_name);
        if (lc == "class" || lc == "blueprintgeneratedclass") {
            vehicle_class = object;
            return true;
        }
        std::uintptr_t property{};
        std::size_t count{};
        // For a UObject/data asset, follow only semantically interesting object/class
        // properties. This avoids treating arbitrary references as vehicle classes.
        if (!ReadPointerAt(*memory, object_class, Layout(profile, "ustruct.propertyLink"), property))
            return false;
        while (property != 0 && count++ < 128) {
            ReflectedPropertyInfo info;
            if (!ReadReflectedPropertyLocked(property, info)) break;
            const std::string n = VehicleLower(info.name);
            if ((n.find("vehicle") != std::string::npos || n.find("class") != std::string::npos ||
                 n.find("blueprint") != std::string::npos) &&
                (info.type == "ObjectProperty" || info.type == "ClassProperty") &&
                info.offset >= 0 && info.element_size == 8) {
                std::uintptr_t candidate{};
                if (ReadValue(*memory, object + static_cast<std::uintptr_t>(info.offset), candidate) &&
                    candidate != 0 && FindVehicleClassFromObjectLocked(candidate, vehicle_class, depth + 1))
                    return true;
            }
            property = info.next;
        }
        return false;
    }

    bool RefreshVehicleCatalogLocked() noexcept {
        try {
            if (vehicle_catalog_generation == object_generation && !vehicle_catalog.empty()) return true;
            vehicle_catalog.clear();
            if (object_registry.items == 0 || object_registry.count == 0) return false;
            for (std::uint32_t index{}; index < object_registry.count; ++index) {
                std::uintptr_t object{};
                std::uint32_t serial{};
                if (!ReadObjectSlot(*memory, object_registry, index, object, serial) || object == 0) continue;
                std::string object_name;
                std::uintptr_t object_class{};
                std::string class_name;
                if (!ReadReflectedObjectNameLocked(object, object_name) ||
                    VehicleLower(object_name).find("vehicle") == std::string::npos ||
                    !ReadPointerAt(*memory, object, Layout(profile, "object.class"), object_class) ||
                    !ReadReflectedObjectNameLocked(object_class, class_name) ||
                    VehicleLower(class_name) != "datatable") continue;
                std::uintptr_t row_map{};
                if (!AddAddress(object, Layout(profile, "dataTable.rowMap"), row_map)) continue;
                std::uintptr_t data{};
                std::int32_t count{}, capacity{};
                if (!ReadValue(*memory, row_map + Layout(profile, "dataTable.rowMapData"), data) ||
                    !ReadValue(*memory, row_map + Layout(profile, "dataTable.rowMapNum"), count) ||
                    !ReadValue(*memory, row_map + Layout(profile, "dataTable.rowMapMax"), capacity) ||
                    data == 0 || count <= 0 || count > 4096 || capacity < count) continue;
                const auto stride = Layout(profile, "dataTable.rowMapElementStride", 24);
                const auto row_offset = Layout(profile, "dataTable.rowMapRowOffset", 8);
                for (std::int32_t row_index{}; row_index < count; ++row_index) {
                    const std::uintptr_t entry = data +
                        static_cast<std::uintptr_t>(row_index) * static_cast<std::uintptr_t>(stride);
                    std::uint32_t comparison_index{}, number{};
                    std::uintptr_t row{};
                    if (!ReadValue(*memory, entry, comparison_index) ||
                        !ReadValue(*memory, entry + 4, number) ||
                        !ReadValue(*memory, entry + static_cast<std::uintptr_t>(row_offset), row) ||
                        row == 0 || comparison_index == 0) continue;
                    std::string id = ResolveNameSnapshotLocked(comparison_index);
                    if (number != 0) id += "_" + std::to_string(number - 1U);
                    if (!id.empty()) vehicle_catalog.push_back({std::move(id), row, object});
                }
            }
            std::sort(vehicle_catalog.begin(), vehicle_catalog.end(),
                [](const VehicleCatalogEntry& a, const VehicleCatalogEntry& b) { return a.id < b.id; });
            vehicle_catalog.erase(std::unique(vehicle_catalog.begin(), vehicle_catalog.end(),
                [](const VehicleCatalogEntry& a, const VehicleCatalogEntry& b) { return a.id == b.id; }),
                vehicle_catalog.end());
            vehicle_catalog_generation = object_generation;
            ++vehicle_catalog_sequence;
            if (selected_vehicle_id.empty() && !vehicle_catalog.empty())
                selected_vehicle_id = vehicle_catalog.front().id;
            if (!selected_vehicle_id.empty() &&
                std::none_of(vehicle_catalog.begin(), vehicle_catalog.end(),
                    [&](const VehicleCatalogEntry& e) { return e.id == selected_vehicle_id; }))
                selected_vehicle_id.clear();
            return !vehicle_catalog.empty();
        } catch (...) {
            vehicle_catalog.clear();
            return false;
        }
    }

    bool ResolveSelectedVehicleClassLocked(std::uintptr_t& vehicle_class) noexcept {
        vehicle_class = 0;
        if (!RefreshVehicleCatalogLocked() || selected_vehicle_id.empty()) return false;
        const auto it = std::find_if(vehicle_catalog.begin(), vehicle_catalog.end(),
            [&](const VehicleCatalogEntry& e) { return e.id == selected_vehicle_id; });
        if (it == vehicle_catalog.end()) return false;
        std::uintptr_t row_struct{};
        std::uintptr_t property{};
        if (!ReadPointerAt(*memory, it->table, Layout(profile, "dataTable.rowStruct"), row_struct) ||
            !ReadPointerAt(*memory, row_struct, Layout(profile, "ustruct.propertyLink"), property))
            return false;
        std::size_t count{};
        while (property != 0 && count++ < 128) {
            ReflectedPropertyInfo info;
            if (!ReadReflectedPropertyLocked(property, info)) break;
            const std::string n = VehicleLower(info.name);
            if ((n.find("vehicle") != std::string::npos || n.find("class") != std::string::npos ||
                 n.find("asset") != std::string::npos) &&
                (info.type == "ObjectProperty" || info.type == "ClassProperty") &&
                info.offset >= 0 && info.element_size == 8) {
                std::uintptr_t candidate{};
                if (ReadValue(*memory, it->row + static_cast<std::uintptr_t>(info.offset), candidate) &&
                    candidate != 0 && FindVehicleClassFromObjectLocked(candidate, vehicle_class))
                    return true;
            }
            property = info.next;
        }
        return false;
    }

    bool BuildVehicleTransformLocked(
        const std::array<double, 3>& location,
        std::vector<std::uint8_t>& transform) const noexcept {
        transform.assign(0x60, 0);
        const double rotation[4]{0.0, 0.0, 0.0, 1.0};
        const double scale[4]{1.0, 1.0, 1.0, 0.0};
        std::memcpy(transform.data(), rotation, sizeof(rotation));
        std::memcpy(transform.data() + 0x20, location.data(), sizeof(double) * 3);
        std::memcpy(transform.data() + 0x40, scale, sizeof(scale));
        return true;
    }

    bool InvokeStaticSpawnLocked(
        std::string_view name,
        const std::vector<std::uint8_t>& transform,
        std::uintptr_t actor_class,
        std::uintptr_t actor,
        std::uintptr_t owner,
        std::uintptr_t& result) noexcept {
        result = 0;
        std::wstring path = L"/Script/Engine.GameplayStatics.";
        for (const char c : name) path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        std::uintptr_t function{}, library_class{}, receiver{};
        if (!FindExactObjectLocked(path.c_str(), function) ||
            !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), library_class) ||
            !ReadPointerAt(*memory, library_class, Layout(profile, "uclass.classDefaultObject"), receiver))
            return false;
        std::uint16_t parms_size{}, return_offset{};
        std::uint8_t num_parms{};
        std::uintptr_t property{};
        if (!ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"), return_offset) ||
            !ReadPointerAt(*memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
            parms_size > 4096 || num_parms > 16) return false;
        std::vector<std::uint8_t> parameters(parms_size);
        std::size_t seen{};
        while (property != 0 && seen++ < num_parms + 2U) {
            ReflectedPropertyInfo info;
            if (!ReadReflectedPropertyLocked(property, info) ||
                info.offset < 0 || info.element_size <= 0) return false;
            const std::size_t offset = static_cast<std::size_t>(info.offset);
            if (offset + static_cast<std::size_t>(info.element_size) > parameters.size()) return false;
            const std::string n = VehicleLower(info.name);
            if (n.find("worldcontextobject") != std::string::npos && info.element_size == 8)
                std::memcpy(parameters.data() + offset, &player_controller, 8);
            else if (n == "actorclass" && info.element_size == 8)
                std::memcpy(parameters.data() + offset, &actor_class, 8);
            else if (n.find("spawntransform") != std::string::npos &&
                     info.element_size == transform.size())
                std::memcpy(parameters.data() + offset, transform.data(), transform.size());
            else if (n.find("collisionhandlingoverride") != std::string::npos && info.element_size >= 1)
                parameters[offset] = 1;
            else if (n == "owner" && info.element_size == 8)
                std::memcpy(parameters.data() + offset, &owner, 8);
            else if (n.find("transformscalemethod") != std::string::npos && info.element_size >= 1)
                parameters[offset] = 0;
            else if (n == "actor" && info.element_size == 8)
                std::memcpy(parameters.data() + offset, &actor, 8);
            property = info.next;
        }
        if (!InvokeProcessEventGuarded(process_event_invoker, receiver, function,
                parameters.data(), parameters.size())) return false;
        if (return_offset != 0xFFFFu && static_cast<std::size_t>(return_offset) + 8 <= parameters.size())
            std::memcpy(&result, parameters.data() + return_offset, 8);
        return true;
    }

    AnomalyStatusV1 VehicleSummon() noexcept {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle summon must run on Game thread");
        std::scoped_lock lock(mutex);
        if (!NteVehicleProfileAvailable() || player_controller == 0 || player_pawn == 0 ||
            !process_event_invoker)
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "vehicle summon host state is unavailable");
        if (!RefreshVehicleCatalogLocked() || selected_vehicle_id.empty())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "Vehicle table is unavailable or no vehicle is selected");
        std::uintptr_t actor_class{};
        if (!ResolveSelectedVehicleClassLocked(actor_class))
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "selected Vehicle row has no validated actor class");
        std::uintptr_t root{};
        std::array<double, 3> player_location{};
        if (!ReadPointerAt(*memory, player_pawn, Layout(profile, "actor.rootComponent"), root) ||
            !ReadValue(*memory, root + Layout(profile, "sceneComponent.location"), player_location) ||
            !std::all_of(player_location.begin(), player_location.end(),
                [](double v) { return std::isfinite(v) && std::abs(v) < 50000000.0; }))
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "player location is unavailable");
        const std::array<double, 3> spawn_location{
            player_location[0] - 2000.0,
            player_location[1] + 2000.0,
            player_location[2] + 2000.0};
        std::vector<std::uint8_t> transform;
        if (!BuildVehicleTransformLocked(spawn_location, transform))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle spawn transform construction failed");
        std::uintptr_t spawned{};
        if (!InvokeStaticSpawnLocked("BeginDeferredActorSpawnFromClass", transform,
                actor_class, 0, player_controller, spawned) || spawned == 0)
            return Status(ANOMALY_STATUS_V1_FAILED, "Tokky-compatible deferred vehicle spawn failed");
        std::uintptr_t finished{};
        if (!InvokeStaticSpawnLocked("FinishSpawningActor", transform,
                actor_class, spawned, player_controller, finished))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle FinishSpawningActor failed");
        if (finished != 0) spawned = finished;
        std::uintptr_t set_owner{};
        if (!FindExactObjectLocked(L"/Script/Engine.Actor.SetOwner", set_owner))
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::SetOwner was not validated");
        std::uint16_t owner_size{};
        std::uint8_t owner_num{};
        std::uintptr_t owner_property{};
        if (!ReadValue(*memory, set_owner + Layout(profile, "ufunction.parmsSize"), owner_size) ||
            !ReadValue(*memory, set_owner + Layout(profile, "ufunction.numParms"), owner_num) ||
            !ReadPointerAt(*memory, set_owner, Layout(profile, "ustruct.propertyLink"), owner_property) ||
            owner_num != 1 || owner_size < 8)
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::SetOwner reflection ABI is invalid");
        ReflectedPropertyInfo owner_info;
        if (!ReadReflectedPropertyLocked(owner_property, owner_info) ||
            owner_info.offset < 0 || owner_info.element_size != 8)
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::SetOwner parameter ABI is invalid");
        std::vector<std::uint8_t> owner_parameters(owner_size);
        std::memcpy(owner_parameters.data() + owner_info.offset, &player_controller, 8);
        if (!InvokeProcessEventGuarded(process_event_invoker, spawned, set_owner,
                owner_parameters.data(), owner_parameters.size()))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle owner assignment failed");

        std::uintptr_t get_owner{};
        if (!FindExactObjectLocked(L"/Script/Engine.Actor.GetOwner", get_owner))
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::GetOwner was not validated");
        std::uint16_t owner_out_size{}, owner_return_offset{};
        std::uint8_t owner_out_num{};
        std::uintptr_t owner_out_property{};
        if (!ReadValue(*memory, get_owner + Layout(profile, "ufunction.parmsSize"), owner_out_size) ||
            !ReadValue(*memory, get_owner + Layout(profile, "ufunction.numParms"), owner_out_num) ||
            !ReadValue(*memory, get_owner + Layout(profile, "ufunction.returnValueOffset"), owner_return_offset) ||
            !ReadPointerAt(*memory, get_owner, Layout(profile, "ustruct.propertyLink"), owner_out_property) ||
            owner_out_num != 1 || owner_out_size < 8 || owner_return_offset == 0xFFFFu)
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::GetOwner reflection ABI is invalid");
        std::vector<std::uint8_t> owner_out(owner_out_size);
        if (!InvokeProcessEventGuarded(process_event_invoker, spawned, get_owner,
                owner_out.data(), owner_out.size()))
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::GetOwner ProcessEvent failed");
        std::uintptr_t verified_owner{};
        if (static_cast<std::size_t>(owner_return_offset) + 8 > owner_out.size())
            return Status(ANOMALY_STATUS_V1_FAILED, "AActor::GetOwner return offset is invalid");
        std::memcpy(&verified_owner, owner_out.data() + owner_return_offset, 8);
        if (verified_owner != player_controller)
            return Status(ANOMALY_STATUS_V1_FAILED, "spawned vehicle Owner is not the current PlayerController");

        std::string spawned_class_name;
        std::uintptr_t spawned_class{};
        if (!ReadPointerAt(*memory, spawned, Layout(profile, "object.class"), spawned_class) ||
            !ReadReflectedObjectNameLocked(spawned_class, spawned_class_name) ||
            VehicleLower(spawned_class_name).find("vehicle") == std::string::npos)
            return Status(ANOMALY_STATUS_V1_FAILED, "spawned object did not validate as a Vehicle actor");
        current_vehicle_object = spawned;
        vehicle_valid = true;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleIdCount(std::uint32_t* count) noexcept {
        if (!count) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle catalog must run on Game thread");
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleCatalogLocked()) { *count = 0; return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "Vehicle data table is unavailable"); }
        *count = static_cast<std::uint32_t>((std::min)(vehicle_catalog.size(), std::size_t{4096}));
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleIdAt(std::uint32_t index, char* destination, std::size_t* inout_size) noexcept {
        if (!inout_size) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle catalog must run on Game thread");
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleCatalogLocked() || index >= vehicle_catalog.size())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "vehicle id not found");
        return CopyString(vehicle_catalog[index].id, destination, inout_size);
    }

    AnomalyStatusV1 VehicleSetSummonVehicleId(AnomalyStringViewV1 id) noexcept {
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle selection must run on Game thread");
        std::scoped_lock lock(mutex);
        const std::string value(id.data ? id.data : "", id.size);
        if (!RefreshVehicleCatalogLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "Vehicle data table is unavailable");
        const auto it = std::find_if(vehicle_catalog.begin(), vehicle_catalog.end(),
            [&](const VehicleCatalogEntry& e) { return e.id == value; });
        if (it == vehicle_catalog.end())
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "vehicle id is not in Vehicle table");
        selected_vehicle_id = value;
        return Status(ANOMALY_STATUS_V1_OK);
    }

        AnomalyStatusV1 VehicleSnapshot(AnomalyNteVehicleSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot))
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle snapshot must run on Game thread");
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked()) {
            *snapshot = {sizeof(*snapshot), 0, {}, 0.0, vehicle_top_speed_ratio, vehicle_wheel_friction_enabled ? 1u : 0u};
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "current driving vehicle is unavailable");
        }
        AnomalyGenerationHandleV1 handle{};
        static_cast<void>(ObjectHandleLocked(current_vehicle_object, handle));
        snapshot->struct_size = sizeof(*snapshot);
        snapshot->flags = ANOMALY_NTE_VEHICLE_V1_VALID;
        if (vehicle_bindings.speed_kmh.function != 0) snapshot->flags |= ANOMALY_NTE_VEHICLE_V1_HAS_SPEED;
        static constexpr std::array<std::string_view, 3> top_speed_path{
            "Vehicle", "SetTopSpeedRatio", "Base"};
        std::uintptr_t top_speed_address{};
        if (ResolveVehicleFloatPathLocked(current_vehicle_object, top_speed_path, top_speed_address)) {
            float ratio{};
            if (ReadValue(*memory, top_speed_address, ratio) && std::isfinite(ratio)) {
                vehicle_top_speed_ratio = ratio;
                snapshot->flags |= ANOMALY_NTE_VEHICLE_V1_HAS_TOP_SPEED_RATIO;
            }
        }
        if (vehicle_bindings.set_wheel_friction.function != 0) snapshot->flags |= ANOMALY_NTE_VEHICLE_V1_HAS_WHEEL_FRICTION;
        if (vehicle_bindings.summon_vehicle.function != 0) snapshot->flags |= ANOMALY_NTE_VEHICLE_V1_HAS_SUMMON;
        snapshot->vehicle = handle;
        snapshot->speed_kmh = vehicle_speed_kmh;
        snapshot->top_speed_ratio = vehicle_top_speed_ratio;
        snapshot->wheel_friction_enabled = vehicle_wheel_friction_enabled ? 1u : 0u;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    AnomalyStatusV1 VehicleSetTopSpeedRatio(float ratio) noexcept {
        if (!std::isfinite(ratio) || ratio < 0.05F || ratio > 20.0F)
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "top speed ratio must be 0.05..20.0");
        if (GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire))
            return Status(ANOMALY_STATUS_V1_FAILED, "vehicle mutation must run on Game thread");
        std::scoped_lock lock(mutex);
        if (!RefreshVehicleLocked())
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "current driving vehicle is unavailable");
        // Tokky multiplies the original movement-component torque and sends the
        // resulting float through SetMaxEngineTorque; do not accumulate ratios.
        if (!vehicle_base_engine_torque_valid || vehicle_base_movement_component == 0 ||
            vehicle_bindings.set_top_speed_ratio.function == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                "Tokky SetMaxEngineTorque ABI or torque baseline is unavailable");
        }
        const float torque = vehicle_base_engine_torque * ratio;
        std::array<std::uint8_t, 4> parameters{};
        std::memcpy(parameters.data() +
                vehicle_bindings.set_top_speed_ratio.parameter_offset,
            &torque, sizeof(torque));
        if (!InvokeProcessEventGuarded(process_event_invoker,
                vehicle_base_movement_component,
                vehicle_bindings.set_top_speed_ratio.function,
                parameters.data(), parameters.size())) {
            return Status(ANOMALY_STATUS_V1_FAILED,
                "SetMaxEngineTorque ProcessEvent failed");
        }
        vehicle_top_speed_ratio = ratio;
        return Status(ANOMALY_STATUS_V1_OK);
    }


