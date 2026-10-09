module;

// The primary interface declares; it includes nothing. System headers live
// in the implementation unit that calls them (AGENTS.md, "System headers").

export module xlings.platform;

import std;

export import :linux;
export import :macos;
export import :windows;
// Shared POSIX implementations (linux + macos). Empty TU on Windows.
export import :unix;
// Processes, signals, descriptors and local sockets (a supervisor's needs).
export import :process;
export import :tcp;
export import :network;
export import :net_notify;
export import :asset_paths;
export import :machine_etc;
export import :domain_mount;
// The kernel's isolation mechanisms: namespaces, Landlock, seccomp, beneath.
export import :isolation;

namespace xlings {
namespace platform {

    // The platform this binary was built for, as constants: `if constexpr`
    // rather than `#if` wherever both branches compile on every platform.
    // `#if` stays for what only exists on one (headers, system calls).
#if defined(_WIN32)
    export inline constexpr bool is_windows = true;
#else
    export inline constexpr bool is_windows = false;
#endif
#if defined(__APPLE__)
    export inline constexpr bool is_macos = true;
#else
    export inline constexpr bool is_macos = false;
#endif
#if defined(__linux__)
    export inline constexpr bool is_linux = true;
#else
    export inline constexpr bool is_linux = false;
#endif
    export inline constexpr bool is_posix = !is_windows;
    // What a file name needs to be an executable here: ".exe" on Windows.
    export inline constexpr std::string_view exe_suffix = is_windows ? ".exe" : "";
    // The null device, as a shell redirection target.
    export inline constexpr std::string_view null_device = is_windows ? "NUL" : "/dev/null";
    // The calendar fields of `t` in the local time zone (localtime_r / _s).
    export std::tm local_time(std::time_t t);
    export bool remove_empty_directory(const std::filesystem::path& path);

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
    export using platform_impl::FileIdentity;
    export using platform_impl::file_identity;
    export using platform_impl::handoff_exec;
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

    // ── Real stdout, decoupled from whatever fd 1 currently means ──────
    //
    // The interface protocol (xlings.interface) writes NDJSON to fd 1 and
    // forbids anything else there. In-process code this program does not
    // control -- a Lua build script run through a vendored package-index
    // loader, for the one measured case -- can still put raw text straight
    // onto the C library's `stdout`, which is fd 1 underneath regardless of
    // who wrote it. `StdoutCapture` below redirects fd 1 away from the real
    // terminal for as long as such code might run; a writer that must keep
    // reaching the real terminal throughout (the NDJSON writer itself) has
    // to hold ITS OWN duplicate of fd 1, taken with dup_stdout_fd() BEFORE
    // any capture starts, so the redirection cannot swallow the very output
    // it exists to protect.

    // A duplicate of the process's current fd 1, usable with write_fd() /
    // close_fd() regardless of what fd 1 is later redirected to. -1 on
    // failure (descriptor limit, or an unsupported platform).
    export int dup_stdout_fd();

    // Write `data` in full to `fd`, retrying across short writes and EINTR.
    // Returns false on a hard I/O error (fd closed, broken pipe, ...).
    export bool write_fd(int fd, std::string_view data);

    export void close_fd(int fd);

    // RAII: while alive, redirects the real fd 1 into an internal pipe and
    // delivers whatever is written there to `onLine`, one line at a time --
    // split on '\n' AND on a bare '\r', since a self-refreshing terminal
    // progress line never sends '\n' until it is done. Restores the
    // original fd 1 on destruction and joins the reader thread.
    //
    // Mechanical only: it does not parse what it captures. Not reentrant --
    // at most one instance should be alive per process, since a nested one's
    // destructor would hand fd 1 back to the outer capture's pipe rather than
    // to the real terminal.
    export class StdoutCapture {
    public:
        explicit StdoutCapture(std::function<void(std::string_view)> onLine);
        ~StdoutCapture();
        StdoutCapture(const StdoutCapture&) = delete;
        StdoutCapture& operator=(const StdoutCapture&) = delete;
    private:
        int savedFd_   { -1 };
        int pipeRead_  { -1 };
        std::thread reader_;
    };

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

    // Run an argv -- no shell, nothing to quote for one -- with this process's
    // stdio and environment, and return its exit code: 127 when the program
    // is not found, 126 when it cannot be executed, 128+n for signal n (the
    // `subos exec` exit-code table, design §12.2).
    export int run_argv(const std::vector<std::string>& argv);

    // Run `argv` with administrator rights, waiting for it (SubOS design
    // part 3 §6.4, "Elevation"): directly when this process already has
    // them; otherwise `sudo` on Linux and macOS (the terminal asks), and the
    // UAC prompt on Windows (ShellExecuteEx "runas"). No shell: argv is
    // passed as given. 126 when elevation could not be started, 127 when the
    // program does not exist. Callers record what they elevated
    // (xlings.subos.elevation); this function only runs it.
    export int run_elevated(const std::vector<std::string>& argv);

    // Whether this process already runs with administrator rights.
    export bool is_elevated();

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

    // Flush a file's data to stable storage (fsync; _commit on Windows).
    // Without it a rename can reach the disk before the staged contents do,
    // which on a power loss yields an atomically-renamed *empty* file.
    export bool sync_file(const std::filesystem::path& path);

    // Flush a directory so a rename or link created in it is durable.
    // Best-effort: Windows has no directory-handle fsync, and some file
    // systems refuse it; neither makes the result less correct than the
    // non-durable write it hardens.
    export void sync_directory(const std::filesystem::path& dir);

    // What changes whenever an entry's own metadata or a directory's entries
    // change: inode and ctime, not following a final symlink (lstat). ctime
    // cannot be set back by a user, so an equal stamp means "not touched
    // since". nullopt where it cannot be read, and always on Windows (no
    // root projection exists there).
    export struct ChangeStamp {
        std::uint64_t inode { 0 };
        std::int64_t seconds { 0 };
        std::int64_t nanoseconds { 0 };
        bool operator==(const ChangeStamp&) const = default;
    };
    export std::optional<ChangeStamp> change_stamp(const std::filesystem::path& path);

    // Publish a same-filesystem scratch atomically, refusing every existing destination.
    // No copy or check-then-rename fallback when the OS cannot enforce this.
    export std::expected<void, std::string> rename_no_replace(
        const std::filesystem::path& from, const std::filesystem::path& to);

    export void write_file_atomic(const std::string& filepath, const std::string& content);
    // Acquire a link target once; macOS may return EINVAL while its symlink
    // vnode is being replaced. Persistent errors remain errors.
    export std::filesystem::path read_symlink(const std::filesystem::path& path, std::error_code& error);

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
