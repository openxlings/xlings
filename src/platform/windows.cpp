module;

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

module xlings.platform;

import std;
import xlings.runtime.cancellation;

#if defined(_WIN32)

namespace xlings {

namespace platform_impl {

std::filesystem::path get_executable_path() {
        wchar_t buf[MAX_PATH];
        DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0) return {};
        return std::filesystem::path(buf);
    }

std::pair<int, std::string> run_command_capture(const std::string& cmd) {
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

auto spawn_command(const std::string& cmd) -> ProcessHandle {
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

auto wait_or_kill(const ProcessHandle& h,
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

void clear_console() {
        std::system("cls");
    }

std::string get_home_dir() {
        if (const char* home = std::getenv("USERPROFILE")) return home;
        return ".";
    }

void set_env_variable(const std::string& key, const std::string& value) {
        ::_putenv_s(key.c_str(), value.c_str());
    }

void make_files_executable(const std::filesystem::path&) {
        // No-op: Windows does not use Unix file permissions
    }

bool create_directory_link(const std::filesystem::path& link,
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

void init_console_output() {
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        // Both handles. Only stdout used to be switched into VT mode, but
        // `log::warn` and `log::error` write to stderr — so on a console
        // their SGR was emitted to a handle that does not interpret it, and
        // the user read the escape bytes instead of a colored `[warn]`.
        for (DWORD which : { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE }) {
            HANDLE h = ::GetStdHandle(which);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD mode = 0;
            if (::GetConsoleMode(h, &mode)) {
                mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                ::SetConsoleMode(h, mode);
            }
        }
    }

bool supports_rewrite_output() {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!::GetConsoleMode(hOut, &mode)) return false;
        return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

bool stderr_is_terminal() {
        HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
        if (hErr == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!::GetConsoleMode(hErr, &mode)) return false;
        return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

bool stdin_is_terminal() {
        HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
        if (hIn == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        return ::GetConsoleMode(hIn, &mode) != 0;
    }

int get_pid() {
        return static_cast<int>(::GetCurrentProcessId());
    }

bool is_process_alive(int pid) {
        if (pid <= 0) return false;
        HANDLE hProcess = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (hProcess == NULL) return false;
        DWORD exitCode = 0;
        bool alive = ::GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
        ::CloseHandle(hProcess);
        return alive;
    }

std::optional<bool> query_terminal_is_light() {
        return std::nullopt;
    }

bool is_root() {
        return false;
    }

void lchown_path_(const std::filesystem::path&,
                             unsigned int, unsigned int) {
    }

std::string home_for_user_(const std::string&) {
        return {};
    }

bool atomic_replace_executable(const std::filesystem::path& src,
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

bool displace_locked_file(const std::filesystem::path& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(path, ec) && !fs::is_symlink(path, ec)) return true;
        ec.clear();

        fs::remove(path, ec);
        if (!ec && !fs::exists(path, ec)) return true;
        ec.clear();

        // Still there: it is open somewhere. Rename it aside. A distinct
        // suffix per attempt so a home that has been updated repeatedly, with
        // earlier casualties still mapped, does not collide with itself.
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto aside = path;
            aside += attempt == 0
                ? std::string(".xlings.old")
                : ".xlings.old" + std::to_string(attempt);
            if (fs::exists(aside, ec)) {
                ec.clear();
                if (!::DeleteFileW(aside.wstring().c_str())) continue;
            }
            ec.clear();
            if (::MoveFileExW(path.wstring().c_str(), aside.wstring().c_str(),
                              MOVEFILE_REPLACE_EXISTING)) {
                // The displaced copy is still mapped by the running process,
                // so it cannot be deleted now. Let the OS drop it at reboot
                // rather than leaving it to accumulate forever.
                ::MoveFileExW(aside.wstring().c_str(), nullptr,
                              MOVEFILE_DELAY_UNTIL_REBOOT);
                return true;
            }
        }
        return false;
    }

bool atomic_swap_paths(const std::filesystem::path& a,
                                  const std::filesystem::path& b) {
        (void)a; (void)b;
        return false;
    }

}

}

#endif // defined(_WIN32)
