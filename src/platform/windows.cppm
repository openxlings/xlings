module;

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

export module xlings.platform:windows;

import std;
import xlings.runtime.cancellation;

#if defined(_WIN32)

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

        // See unix.cppm for the contract; both platforms must agree because
        // the state lock's waiting message is written once, above them.
        bool acquire(const std::filesystem::path& path,
                     std::chrono::milliseconds timeout,
                     const std::function<bool()>& cancelled,
                     std::string& error,
                     const std::function<void(std::chrono::milliseconds)>&
                         onWait = {});

        void release();

    private:
        HANDLE handle_ { INVALID_HANDLE_VALUE };
    };

    export constexpr char PATH_SEPARATOR = ';';
    export constexpr std::string_view OS_NAME = "windows";

    export std::filesystem::path get_executable_path();

    export std::pair<int, std::string> run_command_capture(const std::string& cmd);

    // ─── Cancellable process management ───

    export struct ProcessHandle {
        int pid{-1};
        int pipe_fd{-1};
        // Windows-specific handles stored as opaque values
        void* hProcess{nullptr};
        void* hReadPipe{nullptr};
    };

    export auto spawn_command(const std::string& cmd) -> ProcessHandle;

    export auto wait_or_kill(const ProcessHandle& h,
                             CancellationToken* cancel,
                             std::chrono::milliseconds timeout) -> std::pair<int, std::string>;

    export void clear_console();

    export std::string get_home_dir();

    export void set_env_variable(const std::string& key, const std::string& value);

    export void make_files_executable(const std::filesystem::path&);

    export bool create_directory_link(const std::filesystem::path& link,
                                      const std::filesystem::path& target);

    // Enable UTF-8 code pages and VT processing for ANSI escape sequences.
    // Call once at program startup.
    export void init_console_output();

    // Check if stdout supports cursor save/restore for in-place rewriting.
    export bool supports_rewrite_output();

    // Check if stderr is a console. Separate from stdout because warnings
    // and errors go to stderr: `xlings install > log` still has an
    // interactive stderr, and `2>&1 | cat` has neither.
    export bool stderr_is_terminal();

    // Check if stdin is a console — i.e. whether there is someone there to
    // answer an interactive prompt.
    export bool stdin_is_terminal();

    export template<typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        std::println(fmt, std::forward<Args>(args)...);
    }

    export inline void println(const std::string& msg) {
        std::println(stdout, "{}", msg);
    }

    export int get_pid();

    export bool is_process_alive(int pid);

    // OSC-11 background-color query is a POSIX-tty trick (open /dev/tty,
    // termios raw mode, blocking read with timeout). Windows conhost +
    // legacy terminals don't have a clean equivalent, so we don't try.
    // Modern Windows Terminal users can still set XLINGS_THEME explicitly,
    // and the COLORFGBG fallback in ui::theme is platform-agnostic.
    export std::optional<bool> query_terminal_is_light();

    // ── Execution identity primitives (no POSIX uid/sudo model) ─────
    // Windows has no euid/sudo concept; these stubs keep the
    // cross-platform identity API in platform.cppm compiling and inert.
    export bool is_root();
    export void lchown_path_(const std::filesystem::path&,
                             unsigned int, unsigned int);
    export std::string home_for_user_(const std::string&);


    // Atomically replace `dst` with the contents of `src`, even when `dst`
    // is a currently running executable.
    //
    // Windows semantics: a running .exe is locked for delete and direct
    // overwrite — but RENAME of a locked file is allowed. So the canonical
    // pattern is "move old out of the way, then install new":
    //   1. MoveFileEx(dst -> "<dst>.xlings.old")  — succeeds even when locked
    //   2. CopyFile(src -> dst)                   — destination path is now
    //                                                free, no lock conflict
    //   3. DeleteFile(.old) best-effort, falling back to MOVEFILE_DELAY_UNTIL_REBOOT
    //      if the file is still locked by the running process.
    //
    // Pattern used in production by Chrome auto-updater, VS Code,
    // rustup self-update, etc.
    //
    // Returns true on success.
    export bool atomic_replace_executable(const std::filesystem::path& src,
                                          const std::filesystem::path& dst);

    // Free a path that another process may be holding open, so something new
    // can be written there.
    //
    // On Windows you cannot delete or overwrite a running executable, but you
    // CAN rename it: the open file object follows the name, the running
    // process is unaffected, and the original path becomes free immediately.
    // `xlings self update` needs exactly this -- the shim it has to rewrite is
    // the .exe currently executing it, and a plain remove-then-copy fails with
    // "the process cannot access the file because it is being used by another
    // process" (issue #473).
    //
    // Returns true when the path is free afterwards.
    export bool displace_locked_file(const std::filesystem::path& path);

    // Windows has no single-call directory swap; report unsupported so the
    // caller uses its portable manual-swap fallback. (Symmetry with the POSIX
    // atomic_swap_paths used by the index-artifact installer.)
    export bool atomic_swap_paths(const std::filesystem::path& a,
                                  const std::filesystem::path& b);

} // namespace platform_impl
}

#endif // defined(_WIN32)
