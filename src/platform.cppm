module;

#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <io.h>
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
    export using platform_impl::Icon;
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
