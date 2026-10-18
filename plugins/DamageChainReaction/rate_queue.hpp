#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace anomaly::damage_chain {

// Scheduling only. This class does not call game functions and must not be
// treated as a damage application API. One item can be released per Poll()
// call; a delayed frame never causes a catch-up burst.
class RateQueue final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Config {
        std::uint32_t extra_count{};
        double applications_per_second{100.0};
    };

    void Start(const Config config, const TimePoint now) noexcept {
        remaining_ = config.extra_count;
        const double rate = std::clamp(
            config.applications_per_second, 0.1, 1000.0);
        interval_ = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / rate));
        if (interval_ <= Clock::duration::zero()) {
            interval_ = Clock::duration{1};
        }
        next_due_ = now; // first queued application is eligible immediately
        active_ = remaining_ != 0;
    }

    // Returns true for exactly one due item. The caller must only decrement
    // its own successful-application count after the native adapter confirms
    // success; use CommitSuccess() for that purpose.
    bool Poll(const TimePoint now) const noexcept {
        return active_ && remaining_ != 0 && now >= next_due_;
    }

    // Advance the schedule only after a successful native application. A
    // failed application leaves the same item pending and due for retry.
    void CommitSuccess(const TimePoint now) noexcept {
        if (!active_ || remaining_ == 0) return;
        --remaining_;
        if (remaining_ == 0) {
            active_ = false;
            return;
        }
        // Do not replay missed intervals after a stall. Rate controls cadence;
        // elapsed time is not an expiry condition.
        next_due_ = now + interval_;
    }

    void Defer(const TimePoint retry_at) noexcept {
        if (active_ && remaining_ != 0 && retry_at > next_due_) {
            next_due_ = retry_at;
        }
    }

    void Cancel() noexcept {
        active_ = false;
        remaining_ = 0;
    }

    bool Active() const noexcept { return active_; }
    std::uint32_t Remaining() const noexcept { return remaining_; }
    Clock::duration Interval() const noexcept { return interval_; }
    TimePoint NextDue() const noexcept { return next_due_; }

private:
    std::uint32_t remaining_{};
    Clock::duration interval_{Clock::duration{1}};
    TimePoint next_due_{};
    bool active_{};
};

} // namespace anomaly::damage_chain
