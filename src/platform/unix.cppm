module;

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/file.h>
#include <pwd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)   // <linux/fs.h>; defined here to avoid header clashes
#endif
#endif

export module xlings.platform:unix;

import std;

#if defined(__linux__) || defined(__APPLE__)

namespace xlings {
namespace platform_impl {

    export class FileLock {
    public:
        FileLock() = default;
        FileLock(const FileLock&) = delete;
        FileLock& operator=(const FileLock&) = delete;
        FileLock(FileLock&& other) noexcept;
        FileLock& operator=(FileLock&& other) noexcept;
        ~FileLock();

        // `onWait` is called once as soon as the lock is known to be held by
        // somebody else, and then about every second while the wait
        // continues, with the elapsed time.
        //
        // It exists because the wait used to be silent. A 50ms poll loop for
        // the whole timeout produces no output at all, so a second `xlings
        // install` looked hung rather than queued -- which is exactly what it
        // was reported as. A timeout of zero means wait indefinitely; flock is
        // released by the kernel when its holder exits, so there is no stale
        // lock to wait forever on.
        bool acquire(const std::filesystem::path& path,
                     std::chrono::milliseconds timeout,
                     const std::function<bool()>& cancelled,
                     std::string& error,
                     const std::function<void(std::chrono::milliseconds)>&
                         onWait = {});

        void release();

    private:
        int fd_ { -1 };
    };

    // Parse an accumulated terminal reply buffer for an OSC-11 background
    // color and classify it as light (true) or dark (false). Returns
    // std::nullopt when no well-formed `rgb:RRRR/GGGG/BBBB` reply is present.
    //
    // Pure and side-effect free so the classification logic is unit-testable
    // without a controlling tty (see tests/unit/test_terminal_query.cpp).
    // The buffer may contain unrelated leading bytes (e.g. a CPR fence) and
    // may be terminated by either ST (\e\\) or BEL (\a) — parsing keys off
    // the `rgb:` marker, not the terminator.
    export std::optional<bool> parse_terminal_bg_is_light(std::string_view s);

    // Read a terminal query reply from `fd` as a framed byte stream: loop
    // select()+read() against ONE monotonic deadline, accumulating fragments
    // into a bounded buffer, and stop as soon as the DSR reply's terminator
    // 'R' (a CPR: ESC[<row>;<col>R) is seen. The caller writes an OSC-11
    // query IMMEDIATELY followed by a DSR (ESC[6n); because terminals process
    // input in order, the CPR necessarily arrives after the OSC-11 reply (if
    // any). Seeing it is a deterministic "terminal is done" fence — no valid
    // OSC-11 reply body contains 'R', so the first 'R' uniquely marks it.
    //
    // This fixes #368: the old single-50ms-select + single-read gave up on a
    // slow (Tabby/Electron/xterm.js) or fragmented reply, restored ECHO, and
    // let the reply leak into the parent shell. Reading through the CPR
    // consumes the whole reply while ECHO is still disabled, so nothing leaks.
    export std::string read_terminal_query_reply(int fd,
                                                 std::chrono::milliseconds timeout);

    // Query the controlling terminal for its background color via the
    // OSC-11 sequence (xterm spec, supported by xterm / iTerm2 / Alacritty
    // / Kitty / WezTerm / modern Windows Terminal). Returns std::nullopt
    // if there's no controlling tty, the terminal doesn't respond within
    // the query deadline, or the reply is malformed.
    //
    // Lives in the shared `unix` partition because the implementation
    // (open /dev/tty + termios raw mode + framed read + parse the hex reply)
    // is identical across Linux and macOS — both inherit the POSIX terminal
    // API. windows.cppm provides its own stub. Putting it here means
    // linux.cppm and macos.cppm carry no copy of this code.
    export std::optional<bool> query_terminal_is_light();

    // Atomically replace `dst` with the contents of `src`, even when `dst`
    // is the currently running executable.
    //
    // POSIX semantics: rename(2) is atomic and the kernel keeps the old inode
    // alive for the running process's executable mapping. New invocations
    // resolve the path to the new inode; the running process continues to
    // execute from the old one until it exits. This is the canonical
    // self-update pattern used by apt / brew / rustup / et al.
    //
    // Steps:
    //   1. copy src -> "<dst>.xlings.new" (staged on the same FS as dst so
    //      the rename below is intra-FS and atomic)
    //   2. chmod 0755
    //   3. rename(tmp, dst) — atomic
    //
    // Returns true on success.
    // Free a path so something new can be written there. POSIX unlinks by
    // name, so a running executable's path is released immediately and the
    // process keeps its inode -- no displacement needed. Present so callers
    // do not have to know which platform they are on.
    export bool displace_locked_file(const std::filesystem::path& path);

    export bool atomic_replace_executable(const std::filesystem::path& src,
                                          const std::filesystem::path& dst);

    // Atomically swap two existing paths in a single, uninterruptible step.
    // Both paths must already exist. Returns true on a real atomic exchange
    // (there is never a moment where either path is missing — so a crash/SIGKILL
    // mid-swap cannot strand the live directory), false if the kernel lacks the
    // primitive (the caller must then fall back to a manual rename sequence).
    //
    // The index-artifact installer uses this to close the kill-window in its
    // dir swap (old → backup → new): without it, a kill between the two renames
    // leaves the live index dir missing, which on the next run degrades the
    // official index to a destructive git clone. See
    // .agents/docs/2026-06-30-index-artifact-git-regression-analysis.md.
    export bool atomic_swap_paths(const std::filesystem::path& a,
                                  const std::filesystem::path& b);

    // ── Execution identity primitives (root / sudo awareness) ───────
    // Only the OS primitives live here; all policy (sudo parsing, priv
    // prefix, chown-back) is cross-platform in platform.cppm. See
    // .agents/docs/2026-06-21-root-privilege-identity-design.md.

    export bool is_root();

    // Single-path lchown (don't follow symlinks). Best-effort: ownership
    // restoration must never abort an otherwise-successful operation, so
    // the return value is intentionally ignored.
    export void lchown_path_(const std::filesystem::path& p,
                             unsigned int uid, unsigned int gid);

    // Resolve a user's home dir from the passwd db. Empty on lookup miss.
    export std::string home_for_user_(const std::string& name);

} // namespace platform_impl
} // namespace xlings

#endif // defined(__linux__) || defined(__APPLE__)
