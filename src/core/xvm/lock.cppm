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

std::string lock_scope_key(const std::filesystem::path& home);

constexpr std::chrono::milliseconds default_lock_timeout() {
    return std::chrono::seconds{30};
}

std::filesystem::path state_lock_path(const std::filesystem::path& home);

class StateLock {
public:
    StateLock() = default;
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
    StateLock(StateLock&& other) noexcept;
    StateLock& operator=(StateLock&& other) noexcept;
    ~StateLock();

    // True when the command may proceed. Also true for the bypass and for a
    // lock inherited from an ancestor, so callers cannot accidentally treat
    // either as a failure.
    [[nodiscard]] bool held() const;
    [[nodiscard]] bool bypassed() const;
    // Covered by an ancestor's lock rather than holding one of its own.
    [[nodiscard]] bool inherited() const;

private:
    friend std::expected<StateLock, std::string> acquire_state_lock(
        const std::filesystem::path&, std::chrono::milliseconds);

    void clear_marker_();

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
        std::chrono::milliseconds timeout = default_lock_timeout());

}  // namespace xlings::xvm
