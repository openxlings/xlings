module;
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

module xlings.platform.duplex;
import std;

namespace xlings::platform::duplex {

#if defined(_WIN32)

namespace {

std::wstring wide_(std::string_view text) {
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring quote_(std::wstring_view value) {
    // Quoted only when it must be: a program that parses its own command line
    // (wsl.exe does) reads `"--status"` as a word, not as its option.
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
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

void close_(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

}  // namespace

struct Child::Impl {
    HANDLE in { nullptr };        // what we write: the child's standard input
    HANDLE out { nullptr };       // what we read: its standard output
    HANDLE process { nullptr };
    HANDLE job { nullptr };
    std::mutex writing;
    ~Impl() {
        close_(in);
        close_(out);
        if (job) TerminateJobObject(job, 1);
        close_(process);
        close_(job);
    }
};

std::unique_ptr<Child> Child::start(const std::vector<std::string>& argv, const std::filesystem::path& log, std::string& error) {
    if (argv.empty()) { error = "nothing to start"; return nullptr; }
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    auto impl = std::make_unique<Impl>();
    HANDLE child_in = nullptr, child_out = nullptr;
    if (!CreatePipe(&child_in, &impl->in, &inherit, 0) || !CreatePipe(&impl->out, &child_out, &inherit, 0)) {
        error = "no pipe for the child";
        return nullptr;
    }
    SetHandleInformation(impl->in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl->out, HANDLE_FLAG_INHERIT, 0);
    HANDLE child_err = log.empty()
        ? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
        : CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring command;
    for (const auto& word : argv) {
        if (!command.empty()) command += L' ';
        command += quote_(wide_(word));
    }
    impl->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (impl->job) SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_in;
    startup.hStdOutput = child_out;
    startup.hStdError = child_err;
    PROCESS_INFORMATION info{};
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                        nullptr, nullptr, &startup, &info);
    close_(child_in);
    close_(child_out);
    close_(child_err);
    if (!started) {
        error = std::format("{} did not start (Windows error {})", argv.front(), GetLastError());
        return nullptr;
    }
    impl->process = info.hProcess;
    if (impl->job) AssignProcessToJobObject(impl->job, impl->process);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    return std::unique_ptr<Child>(new Child(std::move(impl)));
}

bool Child::write(std::string_view bytes) {
    std::lock_guard l(impl_->writing);
    while (!bytes.empty()) {
        if (!impl_->in) return false;
        DWORD wrote = 0;
        if (!WriteFile(impl_->in, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 20)), &wrote, nullptr))
            return false;
        bytes.remove_prefix(wrote);
    }
    return true;
}

std::optional<std::string> Child::read(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(impl_->out, nullptr, 0, nullptr, &available, nullptr)) return std::nullopt;   // its end
        if (available > 0) {
            std::string buf(std::min<DWORD>(available, 1u << 16), '\0');
            DWORD got = 0;
            if (!ReadFile(impl_->out, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) return std::nullopt;
            buf.resize(got);
            return buf;
        }
        if (std::chrono::steady_clock::now() >= deadline) return std::string{};
        Sleep(4);
    }
}

void Child::close_input() {
    std::lock_guard l(impl_->writing);
    close_(impl_->in);
}

std::optional<int> Child::exit_code(bool wait) {
    if (!impl_->process) return std::nullopt;
    if (WaitForSingleObject(impl_->process, wait ? INFINITE : 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(impl_->process, &code);
    return static_cast<int>(code);
}

void Child::terminate() {
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    else if (impl_->process) TerminateProcess(impl_->process, 1);
}

#else

struct Child::Impl {
    int in { -1 };
    int out { -1 };
    int pid { -1 };
    std::optional<int> code;
    std::mutex writing;
    ~Impl() {
        if (in >= 0) ::close(in);
        if (out >= 0) ::close(out);
        if (pid > 0 && !code) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

std::unique_ptr<Child> Child::start(const std::vector<std::string>& argv, const std::filesystem::path& log, std::string& error) {
    if (argv.empty()) { error = "nothing to start"; return nullptr; }
    int to[2], from[2];
    if (::pipe(to) != 0) { error = "no pipe for the child"; return nullptr; }
    if (::pipe(from) != 0) { ::close(to[0]); ::close(to[1]); error = "no pipe for the child"; return nullptr; }
    const int err = log.empty() ? ::open("/dev/null", O_WRONLY | O_CLOEXEC) : ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    std::vector<std::string> words = argv;
    std::vector<char*> args;
    for (auto& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    const int pid = ::fork();
    if (pid == 0) {
        ::dup2(to[0], 0);
        ::dup2(from[1], 1);
        if (err >= 0) ::dup2(err, 2);
        for (int fd : {to[0], to[1], from[0], from[1]}) ::close(fd);
        ::signal(SIGPIPE, SIG_DFL);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::close(to[0]);
    ::close(from[1]);
    if (err >= 0) ::close(err);
    if (pid < 0) {
        ::close(to[1]);
        ::close(from[0]);
        error = "the child could not be started";
        return nullptr;
    }
    ::fcntl(to[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(from[0], F_SETFD, FD_CLOEXEC);
    auto impl = std::make_unique<Impl>();
    impl->in = to[1];
    impl->out = from[0];
    impl->pid = pid;
    return std::unique_ptr<Child>(new Child(std::move(impl)));
}

bool Child::write(std::string_view bytes) {
    std::lock_guard l(impl_->writing);
    while (!bytes.empty()) {
        if (impl_->in < 0) return false;
        const auto n = ::write(impl_->in, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::optional<std::string> Child::read(int timeout_ms) {
    pollfd p{impl_->out, POLLIN, 0};
    const int r = ::poll(&p, 1, timeout_ms);
    if (r == 0) return std::string{};
    if (r < 0) return errno == EINTR ? std::optional<std::string>(std::string{}) : std::nullopt;
    std::string buf(1u << 16, '\0');
    for (;;) {
        const auto n = ::read(impl_->out, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt;
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }
}

void Child::close_input() {
    std::lock_guard l(impl_->writing);
    if (impl_->in >= 0) ::close(impl_->in);
    impl_->in = -1;
}

std::optional<int> Child::exit_code(bool wait) {
    if (impl_->code) return impl_->code;
    int status = 0;
    const int r = ::waitpid(impl_->pid, &status, wait ? 0 : WNOHANG);
    if (r != impl_->pid) return std::nullopt;
    impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return impl_->code;
}

void Child::terminate() {
    if (impl_->pid > 0 && !impl_->code) ::kill(impl_->pid, SIGTERM);
}

#endif

Child::Child(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Child::~Child() = default;

}  // namespace xlings::platform::duplex
