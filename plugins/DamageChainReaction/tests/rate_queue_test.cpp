#include "plugins/DamageChainReaction/rate_queue.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

} // namespace

int main() {
    using Queue = anomaly::damage_chain::RateQueue;
    using namespace std::chrono;

    const Queue::TimePoint start{};

    Queue empty;
    empty.Start({0, 100.0}, start);
    Check(!empty.Active(), "zero extra count must not start a queue");
    Check(!empty.Poll(start), "empty queue must not release work");

    Queue queue;
    queue.Start({100, 100.0}, start);
    Check(queue.Active(), "non-zero extra count starts a queue");
    Check(queue.Remaining() == 100, "configured count is additional work");
    Check(queue.Interval() == duration_cast<Queue::Clock::duration>(milliseconds(10)),
        "100 applications/second means a 10 ms interval");
    Check(queue.Poll(start), "first additional application is immediately eligible");

    queue.CommitSuccess(start);
    Check(queue.Remaining() == 99, "one confirmed success consumes exactly one item");
    Check(!queue.Poll(start + milliseconds(9)), "rate limit prevents early next item");
    Check(queue.Poll(start + milliseconds(10)), "next item becomes due at 10 ms");

    // A stall does not expire the queue, but successful work resets the next due
    // point from the current time so missed intervals cannot trigger a catch-up burst.
    const auto after_stall = start + milliseconds(500);
    Check(queue.Poll(after_stall), "pending work remains eligible after a stall");
    queue.CommitSuccess(after_stall);
    Check(queue.Remaining() == 98, "stall does not discard pending count");
    Check(!queue.Poll(after_stall), "no second item is released in the same instant");
    Check(queue.NextDue() == after_stall + milliseconds(10),
        "next cadence starts from successful completion after a stall");

    const auto deferred_to = after_stall + milliseconds(30);
    queue.Defer(deferred_to);
    Check(queue.Remaining() == 98, "defer must not count as success");
    Check(!queue.Poll(after_stall + milliseconds(20)), "defer delays work");
    Check(queue.Poll(deferred_to), "deferred item becomes due at retry time");

    Queue upper_clamp;
    upper_clamp.Start({1, 2000.0}, start);
    Check(upper_clamp.Interval() == duration_cast<Queue::Clock::duration>(milliseconds(1)),
        "rate is capped at 1000 applications/second");

    Queue lower_clamp;
    lower_clamp.Start({1, 0.01}, start);
    Check(lower_clamp.Interval() == duration_cast<Queue::Clock::duration>(seconds(10)),
        "rate is floored at 0.1 applications/second");

    Queue non_finite;
    non_finite.Start({1, std::numeric_limits<double>::quiet_NaN()}, start);
    Check(non_finite.Interval() == duration_cast<Queue::Clock::duration>(milliseconds(10)),
        "non-finite rate safely falls back to 100 applications/second");

    queue.Cancel();
    Check(!queue.Active(), "cancel deactivates the queue");
    Check(queue.Remaining() == 0, "cancel clears pending work");
    Check(!queue.Poll(deferred_to), "cancelled queue cannot release more work");

    std::cout << "RateQueue tests passed" << std::endl;
    return 0;
}
