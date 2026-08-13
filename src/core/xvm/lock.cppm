export module xlings.core.xvm.lock;

import std;

import xlings.platform;

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

// Override the wait, in seconds. `0` means wait indefinitely.
//
// Automation that would rather fail than queue can set it low; a machine that
// does long installs can set it high or to 0. The default below is what an
// interactive user gets.
constexpr std::string_view lock_timeout_env() { return "XLINGS_LOCK_TIMEOUT"; }

std::string lock_scope_key(const std::filesystem::path& home);

// How long to wait for another xlings before giving up.
//
// Was 30 seconds, which is shorter than almost any real install: the holder is
// downloading, extracting, and running hooks under this lock, so the second
// command reliably lost the race and reported failure for something that was
// merely busy. Ten minutes covers a realistic concurrent install and is still
// bounded, so a script cannot block forever on a wedged machine.
//
// The wait is no longer silent (see acquire_state_lock), which is what
// actually fixes "the second install hangs" -- the length only decides when
// waiting becomes failing.
constexpr std::chrono::milliseconds default_lock_timeout() {
    return std::chrono::minutes{10};
}

std::filesystem::path state_lock_path(const std::filesystem::path& home);

// Who holds the lock, as a line fit to print. Empty when unknown.
//
// Read from a SIDECAR file next to the lock, never from the lock file itself:
// writing the lock file goes through a rename, rename swaps the inode, and
// flock lives on the descriptor of the old one -- so writing there would leave
// the lock name pointing at an unlocked inode while this process still
// believed it held the lock.
//
// A HINT, not a fact. The holder may have died between writing it and this
// read; a pid may have been reused. It is only ever used to make a waiting
// message more useful, and every caller must behave identically when it is
// empty.
std::string lock_holder_hint(const std::filesystem::path& home);

std::filesystem::path state_lock_owner_path(const std::filesystem::path& home);

// What this process will call itself in the sidecar. Set once from main.
//
// A bare pid is a poor message -- the waiting user wants to know whether the
// thing blocking them is the install they started in another terminal or a
// background job they forgot about. Kept out of acquire_state_lock's signature
// so the twenty existing call sites do not all have to grow an argument for a
// diagnostic string.
void set_lock_command_hint(std::string command);

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

    void clear_owner_();

    platform::FileLock lock_;
    bool held_ { false };
    bool bypassed_ { false };
    bool inherited_ { false };
    bool ownsMarker_ { false };
    std::string previousMarker_;
    // Removed on release. Best effort: a killed process leaves it behind, and
    // that is why the reader treats it as a hint. The flock is what is
    // authoritative, and the kernel always releases that one.
    std::filesystem::path ownerFile_;
};

// Acquire the home-wide state lock, or explain why not.
//
// flock is released by the kernel when the holder exits, so a crashed xlings
// cannot wedge the lock and there is no staleness to detect or clean up.
//
// The lock file's *contents* are still deliberately never written -- see
// lock_holder_hint for why the holder's identity goes in a sidecar instead.
// The lock file exists only as something to flock.
//
// WAITING IS ANNOUNCED. It used to poll in silence for the whole timeout and
// only then report, so a second `xlings install` produced no output at all
// while it queued. That is indistinguishable from a hang, and it was reported
// as one. The first failed attempt now prints who holds the lock and how long
// this has been waiting, and it keeps saying so about once a second.
std::expected<StateLock, std::string> acquire_state_lock(
        const std::filesystem::path& home,
        std::chrono::milliseconds timeout = default_lock_timeout());

}  // namespace xlings::xvm
