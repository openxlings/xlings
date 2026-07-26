export module xlings.core.xvm.lock;

import std;

import xlings.platform;
import xlings.core.log;
import xlings.core.utils;

// Serialize the commands that mutate xlings state.
//
// install, remove and use all read the version database and the workspace,
// decide something, and write both back. Nothing stopped two of them running
// at once, so the second writer erased the first one's work.
//
// Before provider-scoped binding groups that cost you a stale entry. Now the
// two writes are halves of one release: losing one of them leaves a group
// manifest naming a member the other process removed, and the selection layer
// refuses the whole toolchain. The failure went from "one package is out of
// date" to "gcc no longer switches", which is why this is worth a lock rather
// than a note in the docs.
//
// Scope is the home, not the subos: the version database is shared across
// every subos under it, so a per-subos lock would not protect it.
//
// Only mutating commands take it. Reads -- list, show, shim dispatch -- stay
// lock-free and accept seeing state from a moment ago; making shim dispatch
// block on an install would be a far worse trade.
export namespace xlings::xvm {

// Set to 1 to skip locking entirely. Diagnostic escape hatch for a wedged
// lock file; using it while another xlings is running reintroduces exactly
// the lost update this exists to prevent, so it warns.
// Functions rather than inline constexpr variables: GCC's module support
// emits an undefined reference for an ODR-used inline constexpr variable
// crossing a module boundary (here, as a default argument).
constexpr std::string_view no_lock_env() { return "XLINGS_NO_LOCK"; }

// Carries the home whose lock an ancestor in this process tree already
// holds. Recipes legitimately shell out to xlings from their hooks --
// d2mcpp's config hook runs `xlings install` -- and the child inherits
// this, so it works under the lock its parent holds instead of waiting 30s
// for a lock that cannot be released until the child finishes.
//
// It carries the home rather than a bare flag because the lock is
// per-home. A child operating on a *different* home would otherwise
// inherit the marker and skip locking a home nobody holds -- silently
// reintroducing the lost update this exists to prevent.
constexpr std::string_view reentry_env() { return "XLINGS_STATE_LOCK_HELD"; }

std::string lock_scope_key(const std::filesystem::path& home) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(home, ec);
    return (ec ? home : canonical).string();
}

constexpr std::chrono::milliseconds default_lock_timeout() {
    return std::chrono::seconds{30};
}

std::filesystem::path state_lock_path(const std::filesystem::path& home) {
    return home / ".xlings.lock";
}

class StateLock {
public:
    StateLock() = default;
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
    StateLock(StateLock&& other) noexcept
        : lock_(std::move(other.lock_)),
          held_(std::exchange(other.held_, false)),
          bypassed_(std::exchange(other.bypassed_, false)),
          inherited_(std::exchange(other.inherited_, false)),
          ownsMarker_(std::exchange(other.ownsMarker_, false)),
          previousMarker_(std::move(other.previousMarker_)) {}
    StateLock& operator=(StateLock&& other) noexcept {
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
    ~StateLock() { clear_marker_(); }

    // True when the command may proceed. Also true for the bypass and for a
    // lock inherited from an ancestor, so callers cannot accidentally treat
    // either as a failure.
    [[nodiscard]] bool held() const { return held_; }
    [[nodiscard]] bool bypassed() const { return bypassed_; }
    // Covered by an ancestor's lock rather than holding one of its own.
    [[nodiscard]] bool inherited() const { return inherited_; }

private:
    friend std::expected<StateLock, std::string> acquire_state_lock(
        const std::filesystem::path&, std::chrono::milliseconds);

    void clear_marker_() {
        if (!ownsMarker_) return;
        // Restore whatever was there rather than clearing outright: locking
        // a second home while holding the first must not erase the first
        // one's marker, or a later child would try to re-lock it and hang.
        platform::set_env_variable(std::string(reentry_env()), previousMarker_);
        ownsMarker_ = false;
    }

    platform::FileLock lock_;
    bool held_ { false };
    bool bypassed_ { false };
    bool inherited_ { false };
    bool ownsMarker_ { false };
    std::string previousMarker_;
};

// Acquire the home-wide state lock, or explain why not.
//
// flock is released by the kernel when the holder exits, so a crashed xlings
// cannot wedge the lock and there is no staleness to detect or clean up.
//
// The lock file's *contents* are deliberately never written. It would be
// useful for a timeout message to name the holding pid, but
// platform::write_string_to_file now replaces files by rename, and rename
// swaps in a new inode. flock is held on the open descriptor of the old one,
// so writing to the lock file through that path would leave the lock name
// pointing at an unlocked inode -- and the next process would acquire it
// successfully while this one still believes it holds the lock. The lock file
// exists only as something to flock.
std::expected<StateLock, std::string> acquire_state_lock(
        const std::filesystem::path& home,
        std::chrono::milliseconds timeout = default_lock_timeout()) {
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

}  // namespace xlings::xvm
