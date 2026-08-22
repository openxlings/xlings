module;

#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <io.h>
#define NOMINMAX
#include <windows.h>
#endif

export module xlings.platform;

import std;

export import :linux;
export import :macos;
export import :windows;
// Shared POSIX implementations (linux + macos). Empty TU on Windows.
export import :unix;

namespace xlings {
namespace platform {

    export using platform_impl::PATH_SEPARATOR;
    export using platform_impl::OS_NAME;
    export using platform_impl::clear_console;
    export using platform_impl::get_home_dir;
    export using platform_impl::get_executable_path;
    export using platform_impl::set_env_variable;
    export using platform_impl::make_files_executable;
    export using platform_impl::create_directory_link;
    export using platform_impl::println;
    export using platform_impl::init_console_output;
    export using platform_impl::supports_rewrite_output;
    export using platform_impl::stderr_is_terminal;
    export using platform_impl::stdin_is_terminal;
    export using platform_impl::get_pid;
    export using platform_impl::is_process_alive;
    export using platform_impl::query_terminal_is_light;
#if !defined(_WIN32)
    // POSIX-only building blocks of query_terminal_is_light(), exposed for
    // unit testing without a controlling tty (see #368). No Windows stub —
    // read_terminal_query_reply() operates on a POSIX fd.
    export using platform_impl::parse_terminal_bg_is_light;
    export using platform_impl::read_terminal_query_reply;
#endif
    export using platform_impl::ProcessHandle;
    export using platform_impl::spawn_command;
    export using platform_impl::wait_or_kill;
    export using platform_impl::displace_locked_file;
    export using platform_impl::atomic_replace_executable;
    export using platform_impl::atomic_swap_paths;
    export using platform_impl::FileLock;

    // ── Execution identity (root / sudo awareness) ──────────────────
    // Single source of truth for "who am I / who should own the files I
    // create". Replaces ad-hoc geteuid()/SUDO_* reads and hardcoded
    // "sudo " strings scattered across the codebase.
    //
    // Safety invariant: on the existing (non-root) path every helper
    // returns exactly what the old hardcoded behavior produced, so
    // Linux-non-root / macOS / Windows are byte-for-byte unaffected. The
    // new behavior (chown-back, sudo-aware home, warnings) is gated
    // strictly on the root+SUDO_* path that no current user reaches.
    // Design: .agents/docs/2026-06-21-root-privilege-identity-design.md

    export using platform_impl::is_root;

    // The real user behind a `sudo xlings ...` launch.
    export struct SudoInvoker {
        unsigned int uid;
        unsigned int gid;
        std::string  user;   // SUDO_USER (may be empty)
    };

    // Pure parse of SUDO_UID / SUDO_GID / SUDO_USER. Does NOT check euid,
    // so it is directly unit-testable on every platform. Returns nullopt
    // unless both numeric ids are present and well-formed.
    export [[nodiscard]] std::optional<SudoInvoker> parse_sudo_env();

    // Shell-command prefix for privileged ops (mount/umount/chown):
    // "" when already root (sudo is redundant and often absent in minimal
    // root containers), "sudo " otherwise — identical to the historical
    // hardcoded string for the non-root case.
    export [[nodiscard]] std::string priv_prefix();

    // The invoking user iff launched via sudo (root EUID + SUDO_* set).
    // nullopt for pure root (no demotion target) and unprivileged runs.
    export [[nodiscard]] std::optional<SudoInvoker> sudo_invoker();

    // Home directory that user-facing files (rc lines, ~/.xlings) should
    // belong to. Under sudo this is the invoking user's home (from the
    // passwd db, falling back to /home/<user>), NOT root's $HOME — which
    // fixes the "real user never gets PATH" split-brain. Otherwise it's
    // the ordinary $HOME, unchanged.
    export [[nodiscard]] std::string target_home();

    // Restore ownership of files created while running under sudo back to
    // the invoking user, so a later non-sudo run isn't locked out of its
    // own ~/.xlings. No-op unless launched via sudo (pure root / non-root
    // → returns immediately, zero filesystem traversal).
    export void chown_to_invoker(const std::filesystem::path& path,
                                 bool recursive = true);

    export [[nodiscard]] std::string get_rundir();

    export void set_rundir(const std::string& dir);

    export [[nodiscard]] std::string get_system_language();

    export std::pair<int, std::string> run_command_capture(const std::string& cmd);

    // The host's C library version ("2.39"), probed at subos creation and
    // recorded in the subos manifest (`subos_info.host_glibc`). Rule A of the
    // closure contract -- our_glibc >= host_glibc whenever any host object can
    // enter a process -- needs the right-hand side as of when the subos was
    // laid down; probing later answers about a host that may have moved.
    //
    // Absolute path on purpose: `getconf` resolved through PATH can be a shim
    // into a payload (the payload's `ldd` demonstrably is), and a probe that
    // answers with our own glibc's version defeats its reason to exist.
    // Empty means "unknown" -- non-glibc hosts, missing getconf, any parse
    // surprise -- and callers must treat unknown as unprovable, never as a
    // number to compare against.
    export [[nodiscard]] std::string host_glibc_version();

    // When true, a TUI exclusively owns the terminal — suppress all stdout/stderr
    // from child processes, log output, download renderers, etc.
    inline std::atomic<bool> tui_mode_{false};

    export void set_tui_mode(bool enabled);

    export bool is_tui_mode();

    export int exec(const std::string& cmd);

    export [[nodiscard]] std::string shell_quote(const std::string& arg);

    export std::vector<std::string> shell_command_argv(
        std::string_view shell, std::string_view command, bool interactive);

    // The shells to try, most preferred first. One priority chain for both
    // families: `XLINGS_SHELL` wins everywhere, then the platform's own
    // notion of the user's shell, then a guaranteed fallback. Windows used to
    // honour `XLINGS_SHELL` while POSIX read only `SHELL`, which is the kind
    // of split a caller cannot see and cannot work around.
    export std::vector<std::string> shell_candidates();

    // The shell a caller should name when it reports what it is about to run.
    // Resolving it here rather than at each call site is what keeps an event
    // payload and the process that actually starts from naming different
    // shells -- the macOS sandbox reported `/bin/zsh` while `run_shell` went
    // on to re-read the environment and exec `/bin/sh`.
    export std::string resolve_shell();

    // Replace this process with an interactive shell. POSIX only, and it does
    // not return on success.
    //
    // This is not an optimisation. exec(2) hands the terminal to the shell
    // outright: it becomes the foreground process group leader, job control
    // and Ctrl-Z work, Ctrl-C reaches only the shell, and `exit` returns
    // straight to the parent shell with the original environment. Running the
    // same shell as a forked child that xlings then waits on leaves xlings in
    // the foreground process group, so SIGINT is delivered to it as well and
    // it can die on its default disposition while the child still owns the
    // tty -- two readers on one terminal.
    //
    // Windows has no exec, so `subos use` there really does have to park on
    // WaitForSingleObject; that is a genuine platform difference and the only
    // reason the two families diverge here.
#if !defined(_WIN32)
    export int exec_replace_interactive_shell();
#endif

    // Run one command through a shell and return its exit code. Used for
    // `--cmd` on every platform, and for interactive entry on Windows.
    export int run_shell_command(std::string_view command, bool interactive);

    // Entry point for `subos use`: interactive entry replaces this process on
    // POSIX, everything else spawns and waits.
    export int run_shell(std::string_view command, bool interactive);

    // Escape a single argument for safe embedding in a shell command string.
    export [[nodiscard]] std::string shell_quote(const std::string& arg);

    export [[nodiscard]] std::string read_file_to_string(const std::string& filepath);

    // ── Atomic state-file replace ────────────────────────────────────
    //
    // Every persisted xlings state file is written through here: the whole
    // version database in ~/.xlings.json, each subos workspace, profile
    // generations, and the shell rc files xself edits by read-modify-write.
    // All of them are full-content rewrites, so a plain fopen("w") truncates
    // the destination before the first new byte lands — an interruption at
    // that point does not corrupt the file, it empties it.
    //
    // Write to a staging file in the same directory, flush it to stable
    // storage, then rename over the destination. rename(2) within a
    // directory is atomic, so a reader sees either the whole old file or
    // the whole new one, and an interruption before the rename leaves the
    // old content untouched.

    // fsync the data we just wrote. Without it the rename can reach the disk
    // before the staging file's contents do, which on a power loss yields an
    // atomically-renamed *empty* file — the exact failure we are removing.
    inline bool sync_file_handle_(std::FILE* fp) {
#if defined(_WIN32)
        return ::_commit(::_fileno(fp)) == 0;
#else
        return ::fsync(::fileno(fp)) == 0;
#endif
    }

    // fsync the directory so the rename itself is durable. Best-effort: not
    // all filesystems support it, and failing here does not make the result
    // any less correct than the non-atomic write it replaces.
    inline void sync_directory_(const std::filesystem::path& dir) {
#if !defined(_WIN32)
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) return;
        ::fsync(fd);
        ::close(fd);
#else
        (void)dir;  // no directory-handle fsync equivalent on Windows
#endif
    }

    export void write_file_atomic(const std::string& filepath, const std::string& content);

    export void write_string_to_file(const std::string& filepath, const std::string& content);

    // Wrap directory_iterator for range-for compatibility across compilers.
    // Clang/libc++ 20 only provides operator==(default_sentinel_t) on directory_iterator,
    // which breaks range-for loops that compare two directory_iterator objects.
    export [[nodiscard]] auto dir_entries(const std::filesystem::path& p) {
        return std::ranges::subrange(
            std::filesystem::directory_iterator(p),
            std::default_sentinel
        );
    }

} // namespace platform
} // namespace xlings
