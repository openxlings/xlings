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
    StateLock(StateLock&&) = default;
    StateLock& operator=(StateLock&&) = default;

    // True when the lock is genuinely held. Also true for the bypass, so
    // callers cannot accidentally treat the escape hatch as a failure.
    [[nodiscard]] bool held() const { return held_; }
    [[nodiscard]] bool bypassed() const { return bypassed_; }

private:
    friend std::expected<StateLock, std::string> acquire_state_lock(
        const std::filesystem::path&, std::chrono::milliseconds);

    platform::FileLock lock_;
    bool held_ { false };
    bool bypassed_ { false };
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

    lock.held_ = true;
    return lock;
}

}  // namespace xlings::xvm
