module;

#include <cstdio>
#include <cstdlib>
#if defined(__linux__)
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

export module xlings.platform:linux;

import std;
import xlings.runtime.cancellation;

#if defined(__linux__)

namespace xlings {
namespace platform_impl {

    export constexpr char PATH_SEPARATOR = ':';
    export constexpr std::string_view OS_NAME = "linux";

    export std::filesystem::path get_executable_path() {
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n == -1) return {};
        buf[n] = '\0';
        return std::filesystem::path(buf);
    }

    export std::pair<int, std::string> run_command_capture(const std::string& cmd) {
        std::string full = cmd + " 2>&1";
        FILE* pipe = ::popen(full.c_str(), "r");
        if (!pipe) {
            return {-1, std::string{}};
        }
        std::string output;
        std::array<char, 256> buffer{};
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = ::pclose(pipe);
        return {status, output};
    }

    // ─── Cancellable process management ───

    export struct ProcessHandle {
        int pid{-1};
        int pipe_fd{-1};
    };

    export auto spawn_command(const std::string& cmd) -> ProcessHandle {
        int pipefd[2];
        if (::pipe(pipefd) == -1) return {-1, -1};

        pid_t pid = ::fork();
        if (pid == -1) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return {-1, -1};
        }

        if (pid == 0) {
            // Child: new process group so we can kill the whole group
            ::setpgid(0, 0);
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[1]);
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            ::_exit(127);
        }

        // Parent
        ::close(pipefd[1]);
        // Set read end to non-blocking
        int flags = ::fcntl(pipefd[0], F_GETFL, 0);
        if (flags != -1) ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        return {static_cast<int>(pid), pipefd[0]};
    }

    export auto wait_or_kill(const ProcessHandle& h,
                             CancellationToken* cancel,
                             std::chrono::milliseconds timeout) -> std::pair<int, std::string> {
        if (h.pid <= 0) return {-1, ""};

        std::string output;
        std::array<char, 4096> buf{};
        auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true) {
            // Read any available output (non-blocking)
            while (true) {
                ssize_t n = ::read(h.pipe_fd, buf.data(), buf.size());
                if (n > 0) {
                    output.append(buf.data(), static_cast<std::size_t>(n));
                } else {
                    break;
                }
            }

            // Check if child exited
            int status = 0;
            pid_t result = ::waitpid(h.pid, &status, WNOHANG);
            if (result == h.pid) {
                // Read remaining output
                while (true) {
                    ssize_t n = ::read(h.pipe_fd, buf.data(), buf.size());
                    if (n > 0) output.append(buf.data(), static_cast<std::size_t>(n));
                    else break;
                }
                ::close(h.pipe_fd);
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                return {code, output};
            }

            // Check cancellation
            if (cancel && cancel->is_cancelled()) {
                break;
            }

            // Check timeout
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            // Sleep briefly before next poll
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        // Kill the process group: SIGTERM first
        ::kill(-h.pid, SIGTERM);

        // Grace period: 2 seconds
        auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < kill_deadline) {
            int status = 0;
            pid_t result = ::waitpid(h.pid, &status, WNOHANG);
            if (result == h.pid) {
                ::close(h.pipe_fd);
                return {-1, output};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        // Force kill
        ::kill(-h.pid, SIGKILL);
        int status = 0;
        ::waitpid(h.pid, &status, 0);
        ::close(h.pipe_fd);
        return {-1, output};
    }

    export void clear_console() {
        std::system("clear");
    }

    export std::string get_home_dir() {
        if (const char* home = std::getenv("HOME")) return home;
        return ".";
    }

    export void set_env_variable(const std::string& key, const std::string& value) {
        ::setenv(key.c_str(), value.c_str(), 1);
    }

    export void make_files_executable(const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir)) return;
        for (auto it = std::filesystem::directory_iterator(dir); it != std::default_sentinel; ++it) {
            if (it->is_regular_file())
                ::chmod(it->path().c_str(), 0755);
        }
    }

    export bool create_directory_link(const std::filesystem::path& link,
                                      const std::filesystem::path& target) {
        std::error_code ec;
        if (std::filesystem::is_symlink(link)) {
            std::filesystem::remove(link, ec);
        } else if (std::filesystem::exists(link)) {
            std::filesystem::remove_all(link, ec);
        }
        auto linkTarget = target;
        auto rel = std::filesystem::relative(target, link.parent_path(), ec);
        if (!ec && !rel.empty()) linkTarget = rel;
        ec.clear();
        std::filesystem::create_directory_symlink(linkTarget, link, ec);
        return !static_cast<bool>(ec);
    }

    // No-op on Linux — terminal generally supports ANSI natively.
    export void init_console_output() {}

    // Check if stdout is a TTY (supports cursor save/restore).
    export bool supports_rewrite_output() {
        return ::isatty(STDOUT_FILENO) != 0;
    }

    export template<typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        std::println(fmt, std::forward<Args>(args)...);
    }

    export inline void println(const std::string& msg) {
        std::println("{}", msg);
    }

    export int get_pid() {
        return static_cast<int>(::getpid());
    }

    export bool is_process_alive(int pid) {
        if (pid <= 0) return false;
        // Check /proc/<pid> existence
        auto proc_path = std::filesystem::path("/proc") / std::to_string(pid);
        return std::filesystem::exists(proc_path);
    }

    // query_terminal_is_light() lives in :unix — shared with macOS.

    export struct Icon {
        static constexpr auto pending    = "\xe2\x97\x8b";   // ○
        static constexpr auto running    = "\xe2\x9f\xb3";   // ⟳
        static constexpr auto done       = "\xe2\x9c\x93";   // ✓
        static constexpr auto failed     = "\xe2\x9c\x97";   // ✗
        static constexpr auto skipped    = "\xe2\x96\xb7";   // ▷
        static constexpr auto turn       = "\xe2\x8f\xb5";   // ⏵
        static constexpr auto reply      = "\xe2\x97\x86";   // ◆
        static constexpr auto exec       = "\xe2\x9a\x99";   // ⚙
        static constexpr auto thinking   = "\xe2\x97\x87";   // ◇
        static constexpr auto approval   = "\xe2\x9a\xa0";   // ⚠
        static constexpr auto download   = "\xe2\x86\x93";   // ↓
        static constexpr auto upload     = "\xe2\x86\x91";   // ↑
        static constexpr auto extracting = "\xe2\x9f\x90";   // ⟐
        static constexpr auto arrow      = "\xe2\x96\xb8";   // ▸
        static constexpr auto package    = "\xe2\x97\x86";   // ◆
        static constexpr auto info       = "\xe2\x80\xba";   // ›
    };

} // namespace platform_impl
}

#endif // defined(__linux__)
