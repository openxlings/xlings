export module xlings.runtime.cancellation;

import std;

namespace xlings {

export struct CancelledException : std::runtime_error {
    CancelledException();
};

export struct PausedException : std::runtime_error {
    PausedException();
};

// State: 0=Active, 1=Paused, 2=Cancelled
// Uses atomic<int> instead of atomic<enum> to avoid GCC 15 module ICE.
export class CancellationToken {
    std::atomic<int> state_{0};
    std::mutex mtx_;
    std::condition_variable cv_;

public:
    void pause();

    void resume();

    void cancel();

    void reset();

    bool is_active() const;

    bool is_paused() const;

    bool is_cancelled() const;

    void throw_if_cancelled();

    // Wait on a condition variable while also checking for cancellation/pause and timeout.
    // Returns true if predicate was satisfied, false if cancelled/paused or timed out.
    template<typename Pred>
    bool wait_or_cancel(std::unique_lock<std::mutex>& lock,
                        std::condition_variable& cv, Pred pred,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
        auto deadline = (timeout.count() > 0)
            ? std::chrono::steady_clock::now() + timeout
            : std::chrono::steady_clock::time_point::max();

        while (!pred()) {
            if (!is_active()) return false;

            auto wait_until = std::min(
                deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds{100});

            cv.wait_until(lock, wait_until);

            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
        return true;
    }
};

} // namespace xlings
