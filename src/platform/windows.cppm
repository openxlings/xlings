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

    export constexpr char PATH_SEPARATOR = ';';
    export constexpr std::string_view OS_NAME = "windows";

    export std::filesystem::path get_executable_path() {
        wchar_t buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0) return {};
        return std::filesystem::path(buf);
    }

    export std::pair<int, std::string> run_command_capture(const std::string& cmd) {
        std::string full = cmd + " 2>&1";
        FILE* pipe = ::_popen(full.c_str(), "r");
        if (!pipe) {
            return {-1, std::string{}};
        }
        std::string output;
        std::array<char, 256> buffer{};
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = ::_pclose(pipe);
        return {status, output};
    }

    // ─── Cancellable process management ───

    export struct ProcessHandle {
        int pid{-1};
        int pipe_fd{-1};
        // Windows-specific handles stored as opaque values
        void* hProcess{nullptr};
        void* hReadPipe{nullptr};
    };

    export auto spawn_command(const std::string& cmd) -> ProcessHandle {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE hReadPipe, hWritePipe;
        if (!::CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
            return {};
        }
        ::SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi{};
        std::string cmdline = "cmd /c " + cmd;

        BOOL ok = ::CreateProcessA(
            nullptr, cmdline.data(), nullptr, nullptr, TRUE,
            CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);

        ::CloseHandle(hWritePipe);

        if (!ok) {
            ::CloseHandle(hReadPipe);
            return {};
        }

        ::CloseHandle(pi.hThread);

        ProcessHandle h;
        h.pid = static_cast<int>(pi.dwProcessId);
        h.hProcess = pi.hProcess;
        h.hReadPipe = hReadPipe;
        return h;
    }

    export auto wait_or_kill(const ProcessHandle& h,
                             CancellationToken* cancel,
                             std::chrono::milliseconds timeout) -> std::pair<int, std::string> {
        if (!h.hProcess) return {-1, ""};

        std::string output;
        std::array<char, 4096> buf{};
        auto deadline = std::chrono::steady_clock::now() + timeout;
        HANDLE hProc = static_cast<HANDLE>(h.hProcess);
        HANDLE hPipe = static_cast<HANDLE>(h.hReadPipe);

        while (true) {
            // Read available output
            DWORD avail = 0;
            while (::PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD bytesRead = 0;
                DWORD toRead = std::min(avail, static_cast<DWORD>(buf.size()));
                if (::ReadFile(hPipe, buf.data(), toRead, &bytesRead, nullptr) && bytesRead > 0) {
                    output.append(buf.data(), bytesRead);
                } else break;
            }

            // Check if process exited
            DWORD exitCode = 0;
            if (::WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0) {
                // Read remaining
                while (::PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD bytesRead = 0;
                    DWORD toRead = std::min(avail, static_cast<DWORD>(buf.size()));
                    if (::ReadFile(hPipe, buf.data(), toRead, &bytesRead, nullptr) && bytesRead > 0)
                        output.append(buf.data(), bytesRead);
                    else break;
                }
                ::GetExitCodeProcess(hProc, &exitCode);
                ::CloseHandle(hPipe);
                ::CloseHandle(hProc);
                return {static_cast<int>(exitCode), output};
            }

            if (cancel && cancel->is_cancelled()) break;
            if (std::chrono::steady_clock::now() >= deadline) break;

            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        // Terminate: Ctrl+Break to process group, then hard kill after grace period
        ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(h.pid));
        if (::WaitForSingleObject(hProc, 2000) != WAIT_OBJECT_0) {
            ::TerminateProcess(hProc, 1);
            ::WaitForSingleObject(hProc, 1000);
        }
        ::CloseHandle(hPipe);
        ::CloseHandle(hProc);
        return {-1, output};
    }

    export void clear_console() {
        std::system("cls");
    }

    export std::string get_home_dir() {
        if (const char* home = std::getenv("USERPROFILE")) return home;
        return ".";
    }

    export void set_env_variable(const std::string& key, const std::string& value) {
        ::_putenv_s(key.c_str(), value.c_str());
    }

    export void make_files_executable(const std::filesystem::path&) {
        // No-op: Windows does not use Unix file permissions
    }

    export bool create_directory_link(const std::filesystem::path& link,
                                      const std::filesystem::path& target) {
        std::error_code ec;
        if (std::filesystem::is_symlink(link)) {
            std::filesystem::remove(link, ec);
        } else if (std::filesystem::exists(link)) {
            std::filesystem::remove_all(link, ec);
        }
        auto canonTarget = std::filesystem::canonical(target, ec);
        if (ec) canonTarget = target;
        std::string cmd = "cmd /c mklink /J \"" + link.string() +
                          "\" \"" + canonTarget.string() + "\" >nul 2>&1";
        return std::system(cmd.c_str()) == 0;
    }

    // Enable UTF-8 code pages and VT processing for ANSI escape sequences.
    // Call once at program startup.
    export void init_console_output() {
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (::GetConsoleMode(hOut, &mode)) {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                ::SetConsoleMode(hOut, mode);
            }
        }
    }

    // Check if stdout supports cursor save/restore for in-place rewriting.
    export bool supports_rewrite_output() {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!::GetConsoleMode(hOut, &mode)) return false;
        return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

    export template<typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        std::println(fmt, std::forward<Args>(args)...);
    }

    export inline void println(const std::string& msg) {
        std::println("{}", msg);
    }

    export int get_pid() {
        return static_cast<int>(::GetCurrentProcessId());
    }

    export bool is_process_alive(int pid) {
        if (pid <= 0) return false;
        HANDLE hProcess = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (hProcess == NULL) return false;
        DWORD exitCode = 0;
        bool alive = ::GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
        ::CloseHandle(hProcess);
        return alive;
    }

    // OSC-11 background-color query is a POSIX-tty trick (open /dev/tty,
    // termios raw mode, blocking read with timeout). Windows conhost +
    // legacy terminals don't have a clean equivalent, so we don't try.
    // Modern Windows Terminal users can still set XLINGS_THEME explicitly,
    // and the COLORFGBG fallback in ui::theme is platform-agnostic.
    export std::optional<bool> query_terminal_is_light() {
        return std::nullopt;
    }

    // ── Execution identity primitives (no POSIX uid/sudo model) ─────
    // Windows has no euid/sudo concept; these stubs keep the
    // cross-platform identity API in platform.cppm compiling and inert.
    export bool is_root() {
        return false;
    }
    export void lchown_path_(const std::filesystem::path&,
                             unsigned int, unsigned int) {
    }
    export std::string home_for_user_(const std::string&) {
        return {};
    }

    export struct Icon {
        static constexpr auto pending    = "o";
        static constexpr auto running    = "*";
        static constexpr auto done       = "+";
        static constexpr auto failed     = "x";
        static constexpr auto skipped    = "-";
        static constexpr auto turn       = ">";
        static constexpr auto reply      = "#";
        static constexpr auto exec       = "*";
        static constexpr auto thinking   = "~";
        static constexpr auto approval   = "!";
        static constexpr auto download   = "v";
        static constexpr auto upload     = "^";
        static constexpr auto extracting = ">";
        static constexpr auto arrow      = ">";
        static constexpr auto package    = "#";
        static constexpr auto info       = ">";
    };

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
                                          const std::filesystem::path& dst) {
        namespace fs = std::filesystem;
        std::error_code ec;

        if (!fs::exists(src, ec) || ec) return false;

        fs::create_directories(dst.parent_path(), ec);
        ec.clear();

        // Easy case: dst doesn't exist yet — just copy.
        if (!fs::exists(dst, ec)) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            return !ec;
        }
        ec.clear();

        auto old_path = dst;
        old_path += ".xlings.old";

        // Best-effort cleanup of a leftover .old (may fail if still locked).
        if (fs::exists(old_path, ec)) {
            ::DeleteFileW(old_path.wstring().c_str());
            ec.clear();
        }

        // Step 1: rename live binary out of the way (works even if running).
        if (!::MoveFileExW(dst.wstring().c_str(),
                           old_path.wstring().c_str(),
                           MOVEFILE_REPLACE_EXISTING)) {
            return false;
        }

        // Step 2: copy new binary into the now-free dst path.
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            // Try to restore old binary so we don't leave the user broken.
            ::MoveFileExW(old_path.wstring().c_str(),
                          dst.wstring().c_str(),
                          MOVEFILE_REPLACE_EXISTING);
            return false;
        }

        // Step 3: best-effort cleanup. If the .old file is still locked
        // (because the running process is mapping it), schedule for
        // deletion at next reboot.
        if (!::DeleteFileW(old_path.wstring().c_str())) {
            ::MoveFileExW(old_path.wstring().c_str(), nullptr,
                          MOVEFILE_DELAY_UNTIL_REBOOT);
        }

        return true;
    }

} // namespace platform_impl
}

#endif // defined(_WIN32)
