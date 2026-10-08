module;
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

module xlings.platform.stream;
import std;

namespace xlings::platform::stream {
namespace {
#if defined(_WIN32)
struct Handle {
    HANDLE value{nullptr};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
std::wstring wide(std::string_view text) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!size && !text.empty()) throw std::runtime_error("invalid UTF-8 process argument");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}
std::wstring quote(std::wstring_view value) {
    std::wstring result{L"\""};
    std::size_t slashes{};
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#else
struct Descriptor {
    int value{-1};
    ~Descriptor() { close(); }
    void close() { if (value >= 0) ::close(value); value = -1; }
};
#endif
}

int run(const std::vector<std::string>& argv,
        const std::function<void(std::string_view, std::string_view)>& output,
        const std::function<bool()>& cancelled) {
    if (argv.empty() || argv.front().empty() || !output) return 125;
    for (const auto& word : argv) if (word.find('\0') != std::string::npos) return 125;
    std::array<char, 4096> buffer{};
    bool was_cancelled{};
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle out, out_write, err, err_write, input, process, thread, job;
    if (!CreatePipe(&out.value, &out_write.value, &attributes, 0)
        || !CreatePipe(&err.value, &err_write.value, &attributes, 0)) return 125;
    if (!SetHandleInformation(out.value, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(err.value, HANDLE_FLAG_INHERIT, 0)) return 125;
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) return 125;
    std::wstring command;
    for (const auto& word : argv) {
        if (!command.empty()) command += L' ';
        command += quote(wide(word));
    }
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> storage(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return 125;
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Attributes() { DeleteProcThreadAttributeList(list); }
    } attribute_owner{list};
    std::array<HANDLE, 3> inherited{input.value, out_write.value, err_write.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), sizeof(inherited), nullptr, nullptr)) return 125;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = out_write.value;
    startup.StartupInfo.hStdError = err_write.value;
    startup.lpAttributeList = list;
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) return 125;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &info))
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 127 : 126;
    process.value = info.hProcess;
    thread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 125);
        WaitForSingleObject(process.value, INFINITE);
        return 125;
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.value, 125);
        WaitForSingleObject(process.value, INFINITE);
        return 125;
    }
    out_write.close(); err_write.close(); input.close(); thread.close();
    try {
        for (;;) {
            for (auto [handle, name] : {std::pair{out.value, "stdout"}, std::pair{err.value, "stderr"}}) {
                for (int round = 0; round < 8; ++round) {
                    DWORD available{}, count{};
                    if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) || !available) break;
                    if (!ReadFile(handle, buffer.data(), std::min<DWORD>(available, buffer.size()), &count, nullptr)
                        || !count) break;
                    output(name, {buffer.data(), count});
                }
            }
            if (cancelled && cancelled()) {
                was_cancelled = true;
                TerminateJobObject(job.value, 130);
            }
            const auto waited = WaitForSingleObject(process.value, 10);
            if (waited == WAIT_FAILED) return 125;
            if (waited == WAIT_OBJECT_0) {
                // Descendants cannot keep an output pipe open after this invocation.
                TerminateJobObject(job.value, was_cancelled ? 130 : 0);
                for (auto [handle, name] : {std::pair{out.value, "stdout"}, std::pair{err.value, "stderr"}}) {
                    for (;;) {
                        DWORD available{}, count{};
                        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) || !available) break;
                        if (!ReadFile(handle, buffer.data(), std::min<DWORD>(available, buffer.size()), &count, nullptr)
                            || !count) break;
                        output(name, {buffer.data(), count});
                    }
                }
                DWORD code{};
                if (!GetExitCodeProcess(process.value, &code)) return 125;
                return was_cancelled ? 130 : static_cast<int>(code);
            }
        }
    } catch (...) {
        TerminateJobObject(job.value, 125);
        WaitForSingleObject(process.value, INFINITE);
        throw;
    }
#else
    int out_pipe[2], err_pipe[2];
    if (::pipe(out_pipe) != 0) return 125;
    Descriptor out{out_pipe[0]}, out_write{out_pipe[1]};
    if (::pipe(err_pipe) != 0) return 125;
    Descriptor err{err_pipe[0]}, err_write{err_pipe[1]};
    Descriptor input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
    if (input.value < 0) return 125;
    for (const auto fd : {out.value, out_write.value, err.value, err_write.value})
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return 125;
    for (const auto fd : {out.value, err.value})
        if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) return 125;
    std::vector<char*> arguments;
    for (const auto& word : argv) arguments.push_back(const_cast<char*>(word.c_str()));
    arguments.push_back(nullptr);
    const auto limit = ::sysconf(_SC_OPEN_MAX);
    if (limit < 0) return 125;
    const auto pid = ::fork();
    if (pid < 0) return 125;
    if (pid == 0) {
        ::setpgid(0, 0);
        if (::dup2(input.value, 0) < 0 || ::dup2(out_write.value, 1) < 0 || ::dup2(err_write.value, 2) < 0)
            ::_exit(125);
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0) != 0)
#endif
            for (int fd = 3; fd < limit; ++fd) ::close(fd);
        ::execvp(arguments.front(), arguments.data());
        ::_exit(errno == ENOENT ? 127 : 126);
    }
    ::setpgid(pid, pid);
    out_write.close(); err_write.close(); input.close();
    struct Child {
        int pid;
        bool reaped{};
        ~Child() {
            if (!reaped) {
                ::kill(-pid, SIGKILL);
                int status{};
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    } child{pid};
    int status{};
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    for (;;) {
        for (auto [descriptor, name] : {std::pair{&out, "stdout"}, std::pair{&err, "stderr"}}) {
            for (int round = 0; descriptor->value >= 0 && round < 8; ++round) {
                const auto count = ::read(descriptor->value, buffer.data(), buffer.size());
                if (count > 0) output(name, {buffer.data(), static_cast<std::size_t>(count)});
                else if (count == 0) { descriptor->close(); break; }
                else if (errno == EINTR) continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else return 125;
            }
        }
        if (!child.reaped && cancelled && cancelled()) {
            was_cancelled = true;
            ::kill(-pid, SIGKILL);
        }
        if (!child.reaped) {
            const auto waited = ::waitpid(pid, &status, WNOHANG);
            if (waited < 0 && errno != EINTR) return 125;
            if (waited == pid) {
                child.reaped = true;
                ::kill(-pid, SIGKILL);
                drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }
        if (child.reaped && out.value < 0 && err.value < 0) break;
        if (drain_deadline && std::chrono::steady_clock::now() >= *drain_deadline) break;
        std::array<pollfd, 2> watched{{{out.value, POLLIN, 0}, {err.value, POLLIN, 0}}};
        if (::poll(watched.data(), watched.size(), 20) < 0 && errno != EINTR) return 125;
    }
    if (was_cancelled) return 130;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 125;
#endif
}
}
