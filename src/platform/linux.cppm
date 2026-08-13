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

    export std::filesystem::path get_executable_path();

    export std::pair<int, std::string> run_command_capture(const std::string& cmd);

    // ─── Cancellable process management ───

    export struct ProcessHandle {
        int pid{-1};
        int pipe_fd{-1};
    };

    export auto spawn_command(const std::string& cmd) -> ProcessHandle;

    export auto wait_or_kill(const ProcessHandle& h,
                             CancellationToken* cancel,
                             std::chrono::milliseconds timeout) -> std::pair<int, std::string>;

    export void clear_console();

    export std::string get_home_dir();

    export void set_env_variable(const std::string& key, const std::string& value);

    export void make_files_executable(const std::filesystem::path& dir);

    export bool create_directory_link(const std::filesystem::path& link,
                                      const std::filesystem::path& target);

    // No-op on Linux — terminal generally supports ANSI natively.
    export void init_console_output();

    // Check if stdout is a TTY (supports cursor save/restore).
    export bool supports_rewrite_output();

    // Check if stderr is a TTY. Separate from stdout because warnings and
    // errors go to stderr: `xlings install > log` still has an interactive
    // stderr, and `2>&1 | cat` has neither.
    export bool stderr_is_terminal();

    // Check if stdin is a TTY — i.e. whether there is someone there to answer
    // an interactive prompt.
    export bool stdin_is_terminal();

    export template<typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        std::println(fmt, std::forward<Args>(args)...);
    }

    export inline void println(const std::string& msg) {
        std::println("{}", msg);
    }

    export int get_pid();

    export bool is_process_alive(int pid);

    // query_terminal_is_light() lives in :unix — shared with macOS.


} // namespace platform_impl
}

#endif // defined(__linux__)
