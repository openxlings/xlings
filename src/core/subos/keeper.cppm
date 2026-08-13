// Auto-keeper for high-frequency sandbox exec (M4 — Linux only).
//
// Problem: each `xlings subos use <name> --sandbox --cmd ...` invocation
// spins up bwrap from scratch — for tmpfs/image storage this means
// mount setup on every call, ~100-500ms overhead. An agent running 100
// commands burns 10-50 seconds on mount overhead alone.
//
// Solution: keep one bwrap process alive in the background after the
// first sandbox use; record its PID. Subsequent `subos use --cmd ...`
// invocations nsenter into that bwrap's mount namespace instead of
// remounting from scratch.
//
// Lifecycle:
//   - First use: spawn bwrap with a long-running keeper script;
//                write PID to <subos>/.keeper.pid.
//   - Subsequent: detect alive keeper → nsenter --mount=/proc/<pid>/ns/mnt
//                 -- sh -c <cmd>. Touch <subos>/.keeper.lastused.
//   - Idle TTL : keeper's own polling loop exits if .lastused not bumped
//                for `ttl_sec`; bwrap exits → mount namespace gone.
//   - Stop     : `xlings subos stop <name>` sends SIGTERM, cleans state.
//   - Stale    : if PID file exists but process is dead, treat as
//                no-keeper and respawn.
//
// Auto-trigger (per design): storage=image|tmpfs + --sandbox + Linux.
// In this MVP the keeper is OPT-IN via --keep flag; the auto-default
// trigger is a one-line toggle in use_sandbox_mode_ that we leave off
// pending broader e2e soak. M5 adds --no-keep / --ttl / --keep
// overrides.
//
// Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M4, D9).
module;

// POSIX headers used by the Linux keeper primitives must live in the
// global module fragment so they don't collide with `import std;` later.
// Same pattern as subos.cppm itself uses.
#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

export module xlings.core.subos.keeper;

import std;
import xlings.core.config;

export namespace xlings::subos::keeper {

namespace fs = std::filesystem;

// Default idle TTL in seconds — the keeper self-exits when no
// .keeper.lastused activity is observed within this window. 5 min is
// the design default (D9): long enough that an agent's multi-step
// workflow doesn't re-mount between steps, short enough that idle
// keepers don't pile up on the system.
constexpr int DEFAULT_TTL_SEC = 300;

// Special value passed via --keep: keeper never expires on idle. Caller
// must `subos stop` explicitly. Encoded as a very large int (10 years).
constexpr int TTL_NEVER = 60 * 60 * 24 * 365 * 10;

struct KeeperState {
    fs::path pidFile;       // <home>/subos/<name>/.keeper.pid
    fs::path lastUsedFile;  // <home>/subos/<name>/.keeper.lastused
};

inline KeeperState state_for(const std::string& subosName) {
    auto& p = Config::paths();
    auto dir = p.homeDir / "subos" / subosName;
    return { dir / ".keeper.pid", dir / ".keeper.lastused" };
}

// Touch the last-used timestamp. Called by every exec that hits the
// keeper so its idle countdown resets.
inline void touch_activity(const std::string& subosName) {
    auto s = state_for(subosName);
    std::ofstream ofs(s.lastUsedFile, std::ios::trunc);
    if (ofs) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        ofs << secs;
    }
}

// Is there a live keeper for this subos? Stale PID files are cleaned
// up here so callers can treat false as a definitive "no keeper".
inline bool is_alive(const std::string& subosName) {
#if !defined(__linux__)
    (void)subosName;
    return false;
#else
    auto s = state_for(subosName);
    if (!fs::exists(s.pidFile)) return false;
    std::ifstream ifs(s.pidFile);
    pid_t pid = -1;
    ifs >> pid;
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        // Stale → clean up so the next caller respawns fresh
        std::error_code ec;
        fs::remove(s.pidFile, ec);
        fs::remove(s.lastUsedFile, ec);
        return false;
    }
    return true;
#endif
}

// Record a freshly-spawned keeper's PID. Called by the sandbox-entry
// path after fork()+exec(bwrap, ...) returns the child PID.
inline void register_pid(const std::string& subosName, int pid) {
    auto s = state_for(subosName);
    std::error_code ec;
    fs::create_directories(s.pidFile.parent_path(), ec);
    {
        std::ofstream ofs(s.pidFile, std::ios::trunc);
        if (ofs) ofs << pid;
    }
    touch_activity(subosName);
}

// nsenter into the keeper's mount namespace and run cmd via sh -c.
// Returns the wrapped command's exit code, or -1 on failure to launch.
// On non-Linux: returns -1 (caller falls back to fresh-sandbox path).
inline int nsenter_and_exec(const std::string& subosName,
                            const std::string& cmd) {
#if !defined(__linux__)
    (void)subosName; (void)cmd;
    return -1;
#else
    auto s = state_for(subosName);
    std::ifstream ifs(s.pidFile);
    pid_t pid = -1;
    ifs >> pid;
    if (pid <= 0) return -1;

    touch_activity(subosName);

    // Build nsenter cmdline. We can't pass cmd directly to /bin/sh -c
    // through system() without escaping its single quotes — wrap and
    // escape so embedded ' chars survive.
    std::string escaped;
    escaped.reserve(cmd.size() + 8);
    for (char c : cmd) {
        if (c == '\'') escaped += "'\\''";  // close, escape, reopen
        else escaped += c;
    }
    auto full = std::format("nsenter --mount=/proc/{}/ns/mnt -- /bin/sh -c '{}'",
                            pid, escaped);
    auto rc = std::system(full.c_str());
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return rc;
#endif
}

// Stop the keeper (sent by `xlings subos stop <name>` or by lifecycle
// hooks like subos remove). SIGTERM gives the keeper a chance to clean
// up; if it doesn't die in 2s we follow with SIGKILL.
inline int stop_keeper(const std::string& subosName) {
    auto s = state_for(subosName);
    if (!fs::exists(s.pidFile)) return 0;
#if defined(__linux__)
    std::ifstream ifs(s.pidFile);
    pid_t pid = -1;
    ifs >> pid;
    if (pid > 0 && ::kill(pid, 0) == 0) {
        ::kill(pid, SIGTERM);
        // Best-effort wait
        for (int i = 0; i < 20; ++i) {
            if (::kill(pid, 0) != 0) break;
            ::usleep(100'000);  // 100ms
        }
        if (::kill(pid, 0) == 0) ::kill(pid, SIGKILL);
    }
#endif
    std::error_code ec;
    fs::remove(s.pidFile, ec);
    fs::remove(s.lastUsedFile, ec);
    return 0;
}

// Trigger predicate for auto-spawn — defined per D9: only meaningful
// for image/tmpfs storage in sandbox mode on Linux.
inline bool should_auto_keeper(const std::string& storage, bool sandbox) {
#if !defined(__linux__)
    (void)storage; (void)sandbox;
    return false;
#else
    if (!sandbox) return false;
    return storage == "image" || storage == "tmpfs";
#endif
}

} // namespace xlings::subos::keeper
