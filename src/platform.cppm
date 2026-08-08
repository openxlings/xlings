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

    static std::string gRundir = std::filesystem::current_path().string();

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
    export [[nodiscard]] std::optional<SudoInvoker> parse_sudo_env() {
        const char* su = std::getenv("SUDO_UID");
        const char* sg = std::getenv("SUDO_GID");
        if (!su || !sg || *su == '\0' || *sg == '\0') return std::nullopt;
        char* end = nullptr;
        unsigned long uid = std::strtoul(su, &end, 10);
        if (end == su || *end != '\0') return std::nullopt;
        end = nullptr;
        unsigned long gid = std::strtoul(sg, &end, 10);
        if (end == sg || *end != '\0') return std::nullopt;
        const char* user = std::getenv("SUDO_USER");
        return SudoInvoker{ static_cast<unsigned int>(uid),
                            static_cast<unsigned int>(gid),
                            user ? std::string{user} : std::string{} };
    }

    // Shell-command prefix for privileged ops (mount/umount/chown):
    // "" when already root (sudo is redundant and often absent in minimal
    // root containers), "sudo " otherwise — identical to the historical
    // hardcoded string for the non-root case.
    export [[nodiscard]] std::string priv_prefix() {
        return platform_impl::is_root() ? std::string{} : std::string{"sudo "};
    }

    // The invoking user iff launched via sudo (root EUID + SUDO_* set).
    // nullopt for pure root (no demotion target) and unprivileged runs.
    export [[nodiscard]] std::optional<SudoInvoker> sudo_invoker() {
        if (!platform_impl::is_root()) return std::nullopt;
        return parse_sudo_env();
    }

    // Home directory that user-facing files (rc lines, ~/.xlings) should
    // belong to. Under sudo this is the invoking user's home (from the
    // passwd db, falling back to /home/<user>), NOT root's $HOME — which
    // fixes the "real user never gets PATH" split-brain. Otherwise it's
    // the ordinary $HOME, unchanged.
    export [[nodiscard]] std::string target_home() {
        if (auto inv = sudo_invoker()) {
            if (auto h = platform_impl::home_for_user_(inv->user); !h.empty())
                return h;
            if (!inv->user.empty()) return "/home/" + inv->user;
        }
        return platform_impl::get_home_dir();
    }

    // Restore ownership of files created while running under sudo back to
    // the invoking user, so a later non-sudo run isn't locked out of its
    // own ~/.xlings. No-op unless launched via sudo (pure root / non-root
    // → returns immediately, zero filesystem traversal).
    export void chown_to_invoker(const std::filesystem::path& path,
                                 bool recursive = true) {
        auto inv = sudo_invoker();
        if (!inv) return;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        platform_impl::lchown_path_(path, inv->uid, inv->gid);
        if (recursive && std::filesystem::is_directory(path, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     path,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 !ec && it != std::default_sentinel; it.increment(ec)) {
                platform_impl::lchown_path_(it->path(), inv->uid, inv->gid);
            }
        }
    }

    export [[nodiscard]] std::string get_rundir() {
        return gRundir;
    }

    export void set_rundir(const std::string& dir) {
        gRundir = dir;
    }

    export [[nodiscard]] std::string get_system_language() {
        try {
            auto loc = std::locale("");
            auto name = loc.name();

            if (name.empty() || name == "C" || name == "POSIX") {
                return "en";
            }

            if (auto pos = name.find_first_of("_-.@"); pos != std::string::npos) {
                return name.substr(0, pos);
            }

            return name;
        } catch (const std::runtime_error&) {
            return "en";
        }
    }

    export std::pair<int, std::string> run_command_capture(const std::string& cmd) {
        return platform_impl::run_command_capture(cmd);
    }

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
    export [[nodiscard]] std::string host_glibc_version() {
#if defined(__linux__)
        auto [rc, out] = run_command_capture(
            "/usr/bin/getconf GNU_LIBC_VERSION 2>/dev/null");
        if (rc != 0) return {};
        const auto pos = out.find("glibc ");
        if (pos == std::string::npos) return {};
        std::string v = out.substr(pos + 6);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r'
                              || v.back() == ' '))
            v.pop_back();
        if (v.empty()
            || v.find_first_not_of("0123456789.") != std::string::npos)
            return {};
        return v;
#else
        return {};
#endif
    }

    // When true, a TUI exclusively owns the terminal — suppress all stdout/stderr
    // from child processes, log output, download renderers, etc.
    inline std::atomic<bool> tui_mode_{false};

    export void set_tui_mode(bool enabled) {
        tui_mode_.store(enabled, std::memory_order_relaxed);
    }

    export bool is_tui_mode() {
        return tui_mode_.load(std::memory_order_relaxed);
    }

    export int exec(const std::string& cmd) {
        std::string actual_cmd = cmd;
        if (tui_mode_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            actual_cmd += " >NUL 2>&1";
#else
            actual_cmd += " >/dev/null 2>&1";
#endif
        }
        int status = std::system(actual_cmd.c_str());
#if !defined(_WIN32)
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return status;
#else
        return status;
#endif
    }

    export [[nodiscard]] std::string shell_quote(const std::string& arg);

    export std::vector<std::string> shell_command_argv(
        std::string_view shell, std::string_view command, bool interactive) {
        std::vector<std::string> argv{std::string(shell)};
        if (interactive) {
#if !defined(_WIN32)
            argv.push_back("-i");
#endif
        } else if (shell.find("powershell") != std::string_view::npos
                   || shell.find("pwsh") != std::string_view::npos) {
            argv.insert(argv.end(), {"-NoLogo", "-NonInteractive", "-Command",
                                     std::string(command)});
        } else if (shell.find("cmd") != std::string_view::npos) {
            argv.insert(argv.end(), {"/d", "/s", "/c", std::string(command)});
        } else {
            argv.insert(argv.end(), {"-c", std::string(command)});
        }
        return argv;
    }

    // The shells to try, most preferred first. One priority chain for both
    // families: `XLINGS_SHELL` wins everywhere, then the platform's own
    // notion of the user's shell, then a guaranteed fallback. Windows used to
    // honour `XLINGS_SHELL` while POSIX read only `SHELL`, which is the kind
    // of split a caller cannot see and cannot work around.
    export std::vector<std::string> shell_candidates() {
        if (const auto* configured = std::getenv("XLINGS_SHELL");
            configured && *configured) {
            return {std::string(configured)};
        }
#if defined(_WIN32)
        return {"pwsh.exe", "powershell.exe", "cmd.exe"};
#elif defined(__APPLE__)
        const auto* shell = std::getenv("SHELL");
        return {shell && *shell ? std::string(shell) : std::string("/bin/zsh"),
                "/bin/zsh", "/bin/sh"};
#else
        const auto* shell = std::getenv("SHELL");
        return {shell && *shell ? std::string(shell) : std::string("/bin/sh"),
                "/bin/sh"};
#endif
    }

    // The shell a caller should name when it reports what it is about to run.
    // Resolving it here rather than at each call site is what keeps an event
    // payload and the process that actually starts from naming different
    // shells -- the macOS sandbox reported `/bin/zsh` while `run_shell` went
    // on to re-read the environment and exec `/bin/sh`.
    export std::string resolve_shell() { return shell_candidates().front(); }

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
    export int exec_replace_interactive_shell() {
        std::cout.flush();
        std::cerr.flush();
        for (const auto& shell : shell_candidates()) {
            ::execl(shell.c_str(), shell.c_str(), "-i",
                    static_cast<char*>(nullptr));
        }
        return 127;  // only reached when every candidate failed to exec
    }
#endif

    // Run one command through a shell and return its exit code. Used for
    // `--cmd` on every platform, and for interactive entry on Windows.
    export int run_shell_command(std::string_view command, bool interactive) {
        std::cout.flush();
        std::cerr.flush();
#if defined(_WIN32)
        for (const auto& shell : shell_candidates()) {
            auto argv = shell_command_argv(shell, command, interactive);
            std::string commandLine;
            if (shell.find("cmd") != std::string::npos && !interactive) {
                // cmd.exe does not use CRT backslash escaping for the source
                // following /c. /s deliberately strips this one outer quote
                // pair and leaves quotes/metacharacters inside the command to
                // cmd's own grammar.
                commandLine = shell_quote(shell)
                    + " /d /s /c \"" + std::string(command) + "\"";
            } else {
                for (const auto& arg : argv) {
                    if (!commandLine.empty()) commandLine += ' ';
                    commandLine += shell_quote(arg);
                }
            }
            STARTUPINFOA startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!::CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr,
                                  TRUE, 0, nullptr, nullptr,
                                  &startup, &process)) {
                continue;
            }
            ::WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 127;
            ::GetExitCodeProcess(process.hProcess, &exitCode);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            return static_cast<int>(exitCode);
        }
        return 127;
#else
        const auto candidates = shell_candidates();
        const auto pid = ::fork();
        if (pid < 0) return 127;
        if (pid == 0) {
            for (const auto& shell : candidates) {
                // Built through the same helper the unit test pins, so what is
                // asserted about the argv is what the child actually execs.
                const auto argv = shell_command_argv(shell, command, interactive);
                std::vector<char*> raw;
                raw.reserve(argv.size() + 1);
                for (const auto& arg : argv) {
                    raw.push_back(const_cast<char*>(arg.c_str()));
                }
                raw.push_back(nullptr);
                ::execv(shell.c_str(), raw.data());
            }
            ::_exit(127);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) return 127;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 127;
#endif
    }

    // Entry point for `subos use`: interactive entry replaces this process on
    // POSIX, everything else spawns and waits.
    export int run_shell(std::string_view command, bool interactive) {
#if !defined(_WIN32)
        if (interactive) return exec_replace_interactive_shell();
#endif
        return run_shell_command(command, interactive);
    }

    // Escape a single argument for safe embedding in a shell command string.
    export [[nodiscard]] std::string shell_quote(const std::string& arg) {
#if defined(_WIN32)
        if (!arg.empty() && arg.find_first_of(" \t\n\"") == std::string::npos)
            return arg;
        // MSVC CRT argv quoting: wrap in double quotes, escape \ before " and trailing \.
        std::string result = "\"";
        for (auto it = arg.begin(); ; ++it) {
            std::size_t num_backslashes = 0;
            while (it != arg.end() && *it == '\\') {
                ++it;
                ++num_backslashes;
            }
            if (it == arg.end()) {
                result.append(num_backslashes * 2, '\\');
                break;
            } else if (*it == '"') {
                result.append(num_backslashes * 2 + 1, '\\');
                result += '"';
            } else {
                result.append(num_backslashes, '\\');
                result += *it;
            }
        }
        result += '"';
        return result;
#else
        // POSIX sh: single-quote wrapping neutralises all special characters.
        std::string result = "'";
        for (char c : arg) {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        result += "'";
        return result;
#endif
    }

    export [[nodiscard]] std::string read_file_to_string(const std::string& filepath) {
        std::FILE* fp = std::fopen(filepath.c_str(), "rb");
        if (!fp) {
            throw std::runtime_error("Failed to open file: " + filepath);
        }
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        std::string content(static_cast<std::size_t>(sz), '\0');
        std::fread(content.data(), 1, content.size(), fp);
        std::fclose(fp);
        return content;
    }

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

    export void write_file_atomic(const std::string& filepath, const std::string& content) {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Resolve symlinks and write through to the real file. Shell rc files
        // are commonly symlinks into a dotfiles repo; renaming over the link
        // would replace it with a regular file and silently detach the user's
        // dotfiles. The hop limit keeps a symlink cycle from spinning.
        fs::path target(filepath);
        for (int hops = 0; hops < 16; ++hops) {
            ec.clear();
            if (!fs::is_symlink(target, ec) || ec) break;
            auto next = fs::read_symlink(target, ec);
            if (ec) break;
            target = next.is_absolute() ? next : target.parent_path() / next;
        }

        auto dir = target.parent_path();
        if (dir.empty()) dir = fs::path(".");

        auto staging = dir / ("." + target.filename().string() + ".xlings-tmp." +
                              std::to_string(platform_impl::get_pid()));

        std::FILE* fp = std::fopen(staging.string().c_str(), "wb");
        if (!fp) {
            throw std::runtime_error("Failed to write file: " + filepath);
        }
        bool ok = std::fwrite(content.data(), 1, content.size(), fp) == content.size();
        if (ok) ok = std::fflush(fp) == 0;
        if (ok) ok = sync_file_handle_(fp);
        std::fclose(fp);
        if (!ok) {
            ec.clear();
            fs::remove(staging, ec);
            throw std::runtime_error("Failed to write file: " + filepath);
        }

        // Carry over the destination's mode. A fresh staging file is created
        // under the current umask, so without this an overwrite would quietly
        // drop an executable bit or widen the mode of the file it replaces.
        ec.clear();
        if (auto st = fs::status(target, ec); !ec && fs::exists(st)) {
            std::error_code permEc;
            fs::permissions(staging, st.permissions(),
                            fs::perm_options::replace, permEc);
        }

        ec.clear();
        fs::rename(staging, target, ec);
        if (ec) {
            std::error_code rmEc;
            fs::remove(staging, rmEc);
            throw std::runtime_error("Failed to write file: " + filepath);
        }
        sync_directory_(dir);
    }

    export void write_string_to_file(const std::string& filepath, const std::string& content) {
        write_file_atomic(filepath, content);
    }

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
