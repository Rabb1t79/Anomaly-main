#pragma once

#include "anomaly/hook_manager.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace anomaly {

// AActor::ProcessEvent ingress, separate from the shared UObject detour.
// The pre-callback observes live parameter memory before native code can mutate it;
// the post-callback preserves the existing AHUD dispatch behavior.
class Ue5ActorProcessEventHook final {
public:
    using Callback = std::function<void(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters)>;
    using PreCallback = std::function<void(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters)>;

    explicit Ue5ActorProcessEventHook(
        Callback callback, PreCallback pre_callback = {});
    Ue5ActorProcessEventHook(
        std::unique_ptr<HookBackend> backend, Callback callback,
        PreCallback pre_callback = {});
    ~Ue5ActorProcessEventHook();

    Ue5ActorProcessEventHook(const Ue5ActorProcessEventHook&) = delete;
    Ue5ActorProcessEventHook& operator=(const Ue5ActorProcessEventHook&) = delete;

    [[nodiscard]] bool Start(void* target);
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::vector<HookRecordView> Snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
