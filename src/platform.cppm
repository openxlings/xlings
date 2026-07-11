module;

#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
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
            auto end = std::filesystem::recursive_directory_iterator{};
            for (auto it = std::filesystem::recursive_directory_iterator(
                     path,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 !ec && it != end; it.increment(ec)) {
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

    export void write_string_to_file(const std::string& filepath, const std::string& content) {
        std::FILE* fp = std::fopen(filepath.c_str(), "w");
        if (!fp) {
            throw std::runtime_error("Failed to write file: " + filepath);
        }
        std::fwrite(content.data(), 1, content.size(), fp);
        std::fclose(fp);
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
