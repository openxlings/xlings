module;

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

module xlings.platform;

import std;
import xlings.runtime.cancellation;

#if defined(_WIN32)

namespace xlings {

namespace platform_impl {

std::string user_ui_language() {
        wchar_t buf[LOCALE_NAME_MAX_LENGTH] {};
        const int n = ::GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
        if (n <= 0) return {};
        // The tag is ASCII by definition, so a narrowing copy is exact and
        // avoids dragging a codepage conversion in for four characters.
        std::string out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n && buf[i] != L'\0'; ++i) {
            out += static_cast<char>(buf[i] & 0x7F);
        }
        return out;
    }

std::filesystem::path get_executable_path() {
        // A buffer that grows: an install under a long path (longPathAware)
        // has a module name past MAX_PATH, which a fixed buffer truncates.
        std::vector<wchar_t> buf(MAX_PATH);
        for (;;) {
            const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0) return {};
            if (n < buf.size()) return std::filesystem::path(std::wstring(buf.data(), n));
            if (buf.size() >= 32768) return {};
            buf.resize(buf.size() * 2);
        }
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

        // stdout unbuffered, on Windows only.
        //
        // MSVC's CRT has no line buffering: `_IOLBF` is accepted and behaves
        // as `_IOFBF`, so stdout to a console accumulates in a 4 KB buffer
        // and flushes at whatever byte boundary fills it -- including the
        // middle of a line. stderr is unbuffered, so `log::warn` overtook
        // `log::info` written earlier, and a half-flushed stdout line was
        // completed after it. Both were part of the reported "换行很奇怪".
        //
        // On glibc stdout to a terminal is already line-buffered and the
        // ordering is correct, which is why this is not done there: it would
        // trade a fix for nothing against extra write syscalls.
        //
        // The output paths flush explicitly as well (see log.cpp); this is
        // the belt to that braces, and it also covers anything writing
        // through stdio that this repository does not own.
        ::setvbuf(stdout, nullptr, _IONBF, 0);

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

bool can_write(const std::filesystem::path&) {
        return true;
    }

namespace {
// A path the wide API takes at any length.
std::wstring long_form_(const std::filesystem::path& p) {
        auto w = std::filesystem::absolute(p).lexically_normal().wstring();
        if (w.size() >= MAX_PATH - 12 && !w.starts_with(L"\\\\?\\")) {
            if (w.starts_with(L"\\\\")) w = L"\\\\?\\UNC\\" + w.substr(2);
            else w = L"\\\\?\\" + w;
        }
        return w;
    }
}  // namespace

// A junction (a mount-point reparse point): what a directory link is on
// Windows without the symlink privilege. The reparse data is the documented
// MountPointReparseBuffer layout, written by hand (it lives in ntifs.h).
bool create_dir_link(const std::filesystem::path& target, const std::filesystem::path& link, std::string& error) {
        std::error_code ec;
        std::filesystem::create_directory(link, ec);
        if (ec) { error = ec.message(); return false; }
        auto substitute = L"\\??\\" + std::filesystem::absolute(target).lexically_normal().wstring();
        auto print = std::filesystem::absolute(target).lexically_normal().wstring();
        while (substitute.size() > 7 && (substitute.back() == L'\\' || substitute.back() == L'/')) substitute.pop_back();
        while (print.size() > 3 && (print.back() == L'\\' || print.back() == L'/')) print.pop_back();
        const auto subBytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
        const auto printBytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
        const auto pathBytes = static_cast<USHORT>(subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t));
        std::vector<unsigned char> buf(8 + 8 + pathBytes, 0);
        const auto put16 = [&](std::size_t at, USHORT v) { std::memcpy(buf.data() + at, &v, 2); };
        const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        std::memcpy(buf.data(), &tag, 4);
        put16(4, static_cast<USHORT>(8 + pathBytes));                  // ReparseDataLength
        put16(8, 0);                                                    // SubstituteNameOffset
        put16(10, subBytes);                                            // SubstituteNameLength
        put16(12, static_cast<USHORT>(subBytes + sizeof(wchar_t)));     // PrintNameOffset
        put16(14, printBytes);                                          // PrintNameLength
        std::memcpy(buf.data() + 16, substitute.data(), subBytes);
        std::memcpy(buf.data() + 16 + subBytes + sizeof(wchar_t), print.data(), printBytes);
        const HANDLE h = ::CreateFileW(long_form_(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error = std::format("cannot open {} ({})", link.string(), ::GetLastError());
            std::filesystem::remove(link, ec);
            return false;
        }
        DWORD bytes = 0;
        const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()),
                                          nullptr, 0, &bytes, nullptr);
        const DWORD why = ::GetLastError();
        ::CloseHandle(h);
        if (!ok) {
            error = std::format("cannot make a junction at {} ({})", link.string(), why);
            std::filesystem::remove(link, ec);
            return false;
        }
        return true;
    }

std::optional<std::filesystem::path> dir_link_target(const std::filesystem::path& p) {
        const DWORD attributes = ::GetFileAttributesW(long_form_(p).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return std::nullopt;
        const HANDLE h = ::CreateFileW(long_form_(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::nullopt;
        std::vector<unsigned char> buf(16 * 1024);
        DWORD bytes = 0;
        const BOOL ok = ::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                                          static_cast<DWORD>(buf.size()), &bytes, nullptr);
        ::CloseHandle(h);
        if (!ok || bytes < 16) return std::nullopt;
        DWORD tag = 0;
        std::memcpy(&tag, buf.data(), 4);
        USHORT subOffset = 0, subLength = 0;
        std::memcpy(&subOffset, buf.data() + 8, 2);
        std::memcpy(&subLength, buf.data() + 10, 2);
        std::size_t pathStart = 16;
        if (tag == IO_REPARSE_TAG_SYMLINK) pathStart = 20;             // its Flags field
        else if (tag != IO_REPARSE_TAG_MOUNT_POINT) return std::nullopt;
        if (pathStart + subOffset + subLength > bytes) return std::nullopt;
        std::wstring name(subLength / sizeof(wchar_t), L'\0');
        std::memcpy(name.data(), buf.data() + pathStart + subOffset, subLength);
        if (name.starts_with(L"\\??\\")) name = name.substr(4);
        std::filesystem::path target(name);
        if (target.is_relative()) target = p.parent_path() / target;
        return target.lexically_normal();
    }

// Owners are POSIX: nothing to read or change here.
std::optional<unsigned int> owner_uid(const std::filesystem::path&) { return std::nullopt; }
bool chown_tree(const std::filesystem::path&, unsigned int, unsigned int) { return false; }

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

std::optional<FileIdentity> file_identity(const std::filesystem::path& p) {
        // FILE_READ_ATTRIBUTES with every share mode: identity must be
        // observable for a file another process is running or writing.
        HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE
                                     | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::nullopt;
        BY_HANDLE_FILE_INFORMATION info {};
        const BOOL ok = ::GetFileInformationByHandle(h, &info);
        ::CloseHandle(h);
        if (!ok) return std::nullopt;
        return FileIdentity{
            .device = static_cast<std::uint64_t>(info.dwVolumeSerialNumber),
            .index  = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32)
                      | info.nFileIndexLow,
        };
    }

std::optional<int> handoff_exec(const std::filesystem::path& target,
                                int, char*[]) {
        // The command line is passed through as the OS gave it, not rebuilt
        // from argv: re-quoting is where arguments get mangled, and argv[0]
        // must stay the shim's own spelling for dispatch to see its name.
        std::wstring cmdline = ::GetCommandLineW();
        std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
        buf.push_back(L'\0');

        // Explicit standard handles, made inheritable: without
        // STARTF_USESTDHANDLES a redirected stdout (a pipe from CI or an
        // IDE) is not guaranteed to reach the child.
        STARTUPINFOW si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = ::GetStdHandle(STD_ERROR_HANDLE);
        for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError}) {
            if (h != nullptr && h != INVALID_HANDLE_VALUE) {
                ::SetHandleInformation(h, HANDLE_FLAG_INHERIT,
                                       HANDLE_FLAG_INHERIT);
            }
        }

        ::SetEnvironmentVariableW(L"XLINGS_HANDOFF", L"1");
        PROCESS_INFORMATION pi {};
        if (!::CreateProcessW(target.c_str(), buf.data(), nullptr, nullptr,
                              TRUE, 0, nullptr, nullptr, &si, &pi)) {
            ::SetEnvironmentVariableW(L"XLINGS_HANDOFF", nullptr);
            return std::nullopt;
        }
        // The child shares this console and receives Ctrl+C itself; this
        // process only waits for it, so it must not die first and return the
        // shell a prompt while the tool is still running.
        ::SetConsoleCtrlHandler(nullptr, TRUE);
        ::CloseHandle(pi.hThread);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

bool atomic_swap_paths(const std::filesystem::path& a,
                                  const std::filesystem::path& b) {
        (void)a; (void)b;
        return false;
    }

}

}

#endif // defined(_WIN32)


// ── out-of-line class members ──────────────────────────────────

#if (defined(_WIN32))

namespace xlings::platform_impl {

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    }
    return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::try_acquire(const std::filesystem::path& path, std::string& error) {
    release();
    error.clear();
    handle_ = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) return true;
    const auto code = ::GetLastError();
    if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION)
        error = std::format("failed to lock {}: Windows error {}", path.string(), code);
    return false;
}

bool FileLock::acquire(const std::filesystem::path& path,
                     std::chrono::milliseconds timeout,
                     const std::function<bool()>& cancelled,
                     std::string& error,
                     const std::function<void(std::chrono::milliseconds)>& onWait) {
    release();
    const auto started = std::chrono::steady_clock::now();
    const bool bounded = timeout.count() > 0;
    const auto deadline = started + timeout;
    bool announced = false;
    auto lastSpoke = started;
    while (true) {
        handle_ = ::CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) return true;
        auto code = ::GetLastError();
        if (code != ERROR_SHARING_VIOLATION
            && code != ERROR_LOCK_VIOLATION) {
            error = std::format(
                "failed to lock {}: Windows error {}",
                path.string(), code);
            return false;
        }
        if (cancelled && cancelled()) {
            error = "cancelled while waiting for cache lock";
            return false;
        }
        if (bounded && std::chrono::steady_clock::now() >= deadline) {
            error = std::format("timed out waiting for cache lock {}",
                                path.string());
            return false;
        }
        if (onWait) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
            // First tick immediately; see the note on the declaration.
            if (!announced || now - lastSpoke >= std::chrono::seconds{1}) {
                announced = true;
                lastSpoke = now;
                onWait(elapsed);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

void FileLock::release() {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    ::CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}

} // namespace xlings::platform_impl

#endif
