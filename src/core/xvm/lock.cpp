module xlings.core.xvm.lock;

import std;
import xlings.platform;
import xlings.core.log;
import xlings.core.utils;

namespace xlings::xvm {

std::string lock_scope_key(const std::filesystem::path& home) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(home, ec);
    return (ec ? home : canonical).string();
}

std::filesystem::path state_lock_path(const std::filesystem::path& home) {
    return home / ".xlings.lock";
}

std::expected<StateLock, std::string> acquire_state_lock(const std::filesystem::path& home, std::chrono::milliseconds timeout) {
    StateLock lock;

    if (utils::get_env_or_default(std::string(no_lock_env())) == "1") {
        log::warn(
            "{}=1 — running without the state lock; a concurrent xlings can "
            "silently discard this command's changes",
            no_lock_env());
        lock.bypassed_ = true;
        lock.held_ = true;
        return lock;
    }

    const auto scope = lock_scope_key(home);
    const auto marker = utils::get_env_or_default(std::string(reentry_env()));
    if (!marker.empty() && marker == scope) {
        // An ancestor already holds this home. Waiting would deadlock: the
        // holder cannot finish until this child does.
        lock.inherited_ = true;
        lock.held_ = true;
        return lock;
    }

    std::error_code ec;
    std::filesystem::create_directories(home, ec);

    const auto path = state_lock_path(home);
    std::string error;
    if (!lock.lock_.acquire(path, timeout, {}, error)) {
        return std::unexpected(std::format(
            "another xlings process is changing this installation; waited "
            "{}s. Wait for it to finish, or set {}=1 to proceed anyway "
            "(which can discard one of the two commands' changes)",
            std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
            no_lock_env()));
    }

    // Children spawned from here on inherit this and skip locking -- but
    // only for this home.
    lock.previousMarker_ = marker;
    platform::set_env_variable(std::string(reentry_env()), scope);
    lock.ownsMarker_ = true;
    lock.held_ = true;
    return lock;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xvm {

StateLock::StateLock(StateLock&& other) noexcept : lock_(std::move(other.lock_)),
      held_(std::exchange(other.held_, false)),
      bypassed_(std::exchange(other.bypassed_, false)),
      inherited_(std::exchange(other.inherited_, false)),
      ownsMarker_(std::exchange(other.ownsMarker_, false)),
      previousMarker_(std::move(other.previousMarker_)) {}

StateLock& StateLock::operator=(StateLock&& other) noexcept {
    if (this != &other) {
        clear_marker_();
        lock_ = std::move(other.lock_);
        held_ = std::exchange(other.held_, false);
        bypassed_ = std::exchange(other.bypassed_, false);
        inherited_ = std::exchange(other.inherited_, false);
        ownsMarker_ = std::exchange(other.ownsMarker_, false);
        previousMarker_ = std::move(other.previousMarker_);
    }
    return *this;
}

StateLock::~StateLock() { clear_marker_(); }

[[nodiscard]] bool StateLock::held() const { return held_; }

[[nodiscard]] bool StateLock::bypassed() const { return bypassed_; }

[[nodiscard]] bool StateLock::inherited() const { return inherited_; }

void StateLock::clear_marker_() {
    if (!ownsMarker_) return;
    // Restore whatever was there rather than clearing outright: locking
    // a second home while holding the first must not erase the first
    // one's marker, or a later child would try to re-lock it and hang.
    platform::set_env_variable(std::string(reentry_env()), previousMarker_);
    ownsMarker_ = false;
}

} // namespace xlings::xvm
