module xlings.runtime.cancellation;

import std;


// ── out-of-line class members ──────────────────────────────────

namespace xlings {

CancelledException::CancelledException() : std::runtime_error("operation cancelled") {}

PausedException::PausedException() : std::runtime_error("operation paused") {}

void CancellationToken::pause() {
    state_.store(1, std::memory_order_release);
    cv_.notify_all();
}

void CancellationToken::resume() {
    state_.store(0, std::memory_order_release);
    cv_.notify_all();
}

void CancellationToken::cancel() {
    state_.store(2, std::memory_order_release);
    cv_.notify_all();
}

void CancellationToken::reset() {
    state_.store(0, std::memory_order_release);
}

bool CancellationToken::is_active() const {
    return state_.load(std::memory_order_acquire) == 0;
}

bool CancellationToken::is_paused() const {
    return state_.load(std::memory_order_acquire) == 1;
}

bool CancellationToken::is_cancelled() const {
    return state_.load(std::memory_order_acquire) == 2;
}

void CancellationToken::throw_if_cancelled() {
    auto s = state_.load(std::memory_order_acquire);
    if (s == 2) throw CancelledException{};
    if (s == 1) throw PausedException{};
}

} // namespace xlings
