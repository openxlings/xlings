module;
#include <cstdio>
#if defined(_WIN32)
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
module xlings.platform.worker;
import std;
import xlings.platform;
namespace xlings::platform::worker {
namespace {
constexpr std::size_t kMessageLimit = std::size_t{64} << 20;
std::optional<std::uintptr_t> number(std::string_view s) {
    std::uintptr_t n{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return n;
}
bool read_all(int fd, void* data, std::size_t size) {
    auto* p = static_cast<char*>(data);
    while (size) {
#if defined(_WIN32)
        auto n = ::_read(fd, p, static_cast<unsigned>(std::min(size, std::size_t{1} << 20)));
#else
        auto n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}
bool write_all(int fd, const void* data, std::size_t size) {
#if !defined(_WIN32)
    struct PipeSignalGuard {
        sigset_t blocked{}, previous{};
        bool was_pending{false};
        PipeSignalGuard() {
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, &previous);
            sigset_t pending{};
            sigpending(&pending);
            was_pending = sigismember(&pending, SIGPIPE) == 1;
        }
        ~PipeSignalGuard() {
            sigset_t pending{};
            sigpending(&pending);
            if (!was_pending && sigismember(&pending, SIGPIPE) == 1) {
                int signal{};
                sigwait(&blocked, &signal);
            }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
    } guard;
#endif

    const auto* p = static_cast<const char*>(data);
    while (size) {
#if defined(_WIN32)
        auto n = ::_write(fd, p, static_cast<unsigned>(std::min(size, std::size_t{1} << 20)));
#else
        auto n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}
#if !defined(_WIN32)
void close_except(std::initializer_list<int> descriptors) {
    std::vector<int> keep{0, 1, 2};
    keep.insert(keep.end(), descriptors.begin(), descriptors.end());
    std::ranges::sort(keep);
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
#if defined(__linux__)
    unsigned next = 3;
    bool supported = true;
    for (int fd : keep) {
        if (fd < 3)
            continue;
        if (next < static_cast<unsigned>(fd) &&
            ::syscall(436, next, static_cast<unsigned>(fd) - 1, 0) != 0)
            supported = false;
        next = static_cast<unsigned>(fd) + 1;
    }
    if (::syscall(436, next, std::numeric_limits<unsigned>::max(), 0) != 0)
        supported = false;
    if (supported)
        return;
#endif
    const auto limit = ::sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < limit; ++fd)
        if (!std::binary_search(keep.begin(), keep.end(), fd))
            ::close(fd);
}
#endif
#if defined(_WIN32)
std::wstring wide(std::string_view s) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (!count && !s.empty())
        throw std::runtime_error("invalid UTF-8 worker argument");
    std::wstring v(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        v.data(), count);
    return v;
}
std::wstring quote(const std::wstring& s) {
    std::wstring r = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        r.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        slashes = 0;
        r += c;
    }
    r.append(2 * slashes, L'\\');
    r += L'"';
    return r;
}
#endif
} // namespace
std::optional<Channel> attach(std::string_view r, std::string_view w) {
    auto a = number(r), b = number(w);
    if (!a || !b || *a == *b)
        return std::nullopt;
#if defined(_WIN32)
    auto rh = reinterpret_cast<HANDLE>(*a), wh = reinterpret_cast<HANDLE>(*b);
    SetHandleInformation(rh, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(wh, HANDLE_FLAG_INHERIT, 0);
    int rd = ::_open_osfhandle(static_cast<std::intptr_t>(*a), _O_RDONLY | _O_BINARY);
    int wr = ::_open_osfhandle(static_cast<std::intptr_t>(*b), _O_WRONLY | _O_BINARY);
    if (rd < 0 || wr < 0) {
        if (rd >= 0)
            ::_close(rd);
        if (wr >= 0)
            ::_close(wr);
        return std::nullopt;
    }
    return Channel{rd, wr};
#else
    if (*a > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        *b > static_cast<unsigned>(std::numeric_limits<int>::max()))
        return std::nullopt;
    Channel c{static_cast<int>(*a), static_cast<int>(*b)};
    if (!platform::set_inheritable(c.read, false) || !platform::set_inheritable(c.write, false))
        return std::nullopt;
    return c;
#endif
}
void close(Channel& c) {
    for (auto fd : {c.read, c.write})
        if (fd >= 0)
            platform::close_fd(fd);
    c = {};
}
std::expected<std::string, std::string> receive(Channel c) {
    std::array<unsigned char, 4> header{};
    if (!read_all(c.read, header.data(), header.size()))
        return std::unexpected("Lua worker control channel closed");
    std::size_t size = 0;
    for (int i = 0; i < 4; ++i)
        size |= static_cast<std::size_t>(header[i]) << (i * 8);
    if (size > kMessageLimit)
        return std::unexpected("Lua worker message exceeds limit");
    std::string message(size, '\0');
    if (!read_all(c.read, message.data(), size))
        return std::unexpected("Lua worker response truncated");
    return message;
}
bool send(Channel c, std::string_view message) {
    if (message.size() > kMessageLimit)
        return false;
    std::array<unsigned char, 4> header{};
    for (int i = 0; i < 4; ++i)
        header[i] = static_cast<unsigned char>(message.size() >> (i * 8));
    return write_all(c.write, header.data(), header.size()) &&
           write_all(c.write, message.data(), message.size());
}
bool install_trace(std::string_view channel) {
    auto socket = number(channel);
    if (!socket || *socket > static_cast<unsigned>(std::numeric_limits<int>::max()))
        return false;
    const auto fd = static_cast<int>(*socket);
    platform::set_inheritable(fd, false);
    std::promise<std::array<int, 2>> listeners;
    auto ready = listeners.get_future();
    bool transferred = false;
    // This helper must exist before either filter: its one SCM_RIGHTS handoff
    // must never wait for the network listener it is still handing to the host.
    std::jthread handoff([&] {
        const auto fds = ready.get();
        if (fds[0] >= 0 && fds[1] >= 0)
            transferred = platform::send_message(fd, "trace-listeners", fds);
        for (int listener : fds)
            platform::close_fd(listener);
        platform::close_fd(fd);
    });
    const auto net_listener = platform::net_notify::listener();
    const auto exec_listener = platform::seccomp::exec_listener();
    listeners.set_value({exec_listener, net_listener});
    handoff.join();
    return transferred;
}
std::expected<std::string, std::string> read_log(const std::filesystem::path& root,
                                                 std::string_view name) {
    if (name.empty() || name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos || name == "." || name == "..")
        return std::unexpected("invalid recipe log name");
#if defined(_WIN32)
    const auto path = root / std::filesystem::path(name);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected("cannot read recipe log");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(file);
        return std::unexpected("recipe log must be a regular file");
    }
    int fd = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(file), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(file);
        return std::unexpected("cannot attach recipe log");
    }
#else
    int directory = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return std::unexpected("cannot open owned recipe directory");
    int fd = ::openat(directory, std::string(name).c_str(),
                      O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ::close(directory);
    if (fd < 0)
        return std::unexpected("cannot read owned recipe log");
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return std::unexpected("recipe log must be a regular file");
    }
#endif
    struct Cleanup {
        int fd;
        ~Cleanup() {
            platform::close_fd(fd);
        }
    } cleanup{fd};
    std::string contents;
    std::array<char, 16384> buffer{};
    while (true) {
#if defined(_WIN32)
        auto count = ::_read(fd, buffer.data(), static_cast<unsigned>(buffer.size()));
#else
        auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
#endif
        if (count == 0)
            break;
        if (count < 0)
            return std::unexpected("cannot read recipe output");
        if (contents.size() + static_cast<std::size_t>(count) > (std::size_t{32} << 20))
            return std::unexpected("recipe output exceeds 32 MiB log limit");
        contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return contents;
}
bool prepare_log(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) &&
              !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CloseHandle(file);
    return ok;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    struct stat info{};
    bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    ::close(fd);
    return ok;
#endif
}
struct OutputCapture::State {
    int saved_out{-1};
    int saved_error{-1};
    int destination{-1};
    ~State() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
#if defined(_WIN32)
        if (saved_out >= 0)
            ::_dup2(saved_out, 1);
        if (saved_error >= 0)
            ::_dup2(saved_error, 2);
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(1)));
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(2)));
#else
        if (saved_out >= 0)
            ::dup2(saved_out, 1);
        if (saved_error >= 0)
            ::dup2(saved_error, 2);
#endif
        for (int fd : {saved_out, saved_error, destination})
            platform::close_fd(fd);
    }
};
OutputCapture::OutputCapture(const std::filesystem::path& path)
    : state_(std::make_unique<State>()) {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
#if defined(_WIN32)
    state_->saved_out = ::_dup(1);
    state_->saved_error = ::_dup(2);
    state_->destination =
        ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (state_->saved_out < 0 || state_->saved_error < 0 || state_->destination < 0 ||
        ::_dup2(state_->destination, 1) != 0 || ::_dup2(state_->destination, 2) != 0)
        throw std::runtime_error("cannot capture recipe output");
    SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(1)));
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(2)));
#else
    state_->saved_out = ::fcntl(1, F_DUPFD_CLOEXEC, 6);
    state_->saved_error = ::fcntl(2, F_DUPFD_CLOEXEC, 6);
    state_->destination =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (state_->saved_out < 0 || state_->saved_error < 0 || state_->destination < 0 ||
        ::dup2(state_->destination, 1) < 0 || ::dup2(state_->destination, 2) < 0)
        throw std::runtime_error("cannot capture recipe output");
#endif
}
OutputCapture::~OutputCapture() = default;
struct Process::State {
    Channel channel;
#if defined(_WIN32)
    HANDLE process{};
#else
    int pid{-1};
    int pasta_pidfd{-1};
    int exec_listener{-1};
    int net_listener{-1};
    int proxy_bridge{-1};
    std::atomic<bool> stopping{false};
    std::thread observer;
#endif
    ~State() {
        close(channel);
#if defined(_WIN32)
        if (process) {
            if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 125);
                WaitForSingleObject(process, INFINITE);
            }
            CloseHandle(process);
        }
#else
        stopping.store(true);
        if (observer.joinable())
            observer.join();
        if (exec_listener >= 0)
            platform::close_fd(exec_listener);
        platform::close_fd(net_listener);
        platform::close_fd(proxy_bridge);
#if defined(__linux__)
        if (pasta_pidfd >= 0) {
            (void)::syscall(424, pasta_pidfd, SIGTERM, nullptr, 0);
            platform::close_fd(pasta_pidfd);
        }
#endif
        if (pid > 0) {
            int status{};
            if (::waitpid(pid, &status, WNOHANG) == 0) {
                ::kill(-pid, SIGTERM);
                ::kill(pid, SIGTERM);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
#endif
    }
};
Process::Process(std::unique_ptr<State> s) : state_(std::move(s)) {
}
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;
Process::~Process() = default;
std::expected<Process, std::string> Process::launch(const std::vector<std::string>& input,
                                                    const std::map<std::string, std::string>& env,
                                                    const Network& network, const Trace& trace) {
    if (input.empty())
        return std::unexpected("empty Lua worker command");
    auto state = std::make_unique<State>();
    auto argv = input;
#if defined(_WIN32)
    if (!network.pasta.empty() || network.proxy || trace.enabled)
        return std::unexpected("worker kernel isolation mechanisms are unavailable");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE cr{}, pw{}, pr{}, cw{};
    if (!CreatePipe(&cr, &pw, &sa, 0))
        return std::unexpected("cannot create Lua worker request pipe");
    if (!CreatePipe(&pr, &cw, &sa, 0)) {
        CloseHandle(cr);
        CloseHandle(pw);
        return std::unexpected("cannot create Lua worker response pipe");
    }
    SetHandleInformation(pw, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pr, HANDLE_FLAG_INHERIT, 0);
    for (auto& a : argv) {
        if (a == kReadToken)
            a = std::to_string(reinterpret_cast<std::uintptr_t>(cr));
        if (a == kWriteToken)
            a = std::to_string(reinterpret_cast<std::uintptr_t>(cw));
    }
    std::wstring command, environment;
    for (const auto& a : argv) {
        if (!command.empty())
            command += L' ';
        command += quote(wide(a));
    }
    for (const auto& [k, v] : env) {
        environment += wide(k + "=" + v);
        environment += L'\0';
    }
    environment += L'\0';
    if (env.empty())
        environment += L'\0';
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const auto exe = wide(argv.front());
    bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &si, &pi);
    CloseHandle(cr);
    CloseHandle(cw);
    if (!ok) {
        CloseHandle(pw);
        CloseHandle(pr);
        return std::unexpected("cannot launch Lua worker: " + std::to_string(GetLastError()));
    }
    CloseHandle(pi.hThread);
    state->process = pi.hProcess;
    state->channel = {
        ::_open_osfhandle(reinterpret_cast<std::intptr_t>(pr), _O_RDONLY | _O_BINARY),
        ::_open_osfhandle(reinterpret_cast<std::intptr_t>(pw), _O_WRONLY | _O_BINARY)};
    if (state->channel.read < 0 || state->channel.write < 0)
        return std::unexpected("cannot attach Lua worker pipes");
#else
    auto request = platform::make_pipe(), response = platform::make_pipe();
    if (!request)
        return std::unexpected("cannot create Lua worker request pipe");
    if (!response) {
        platform::close_fd((*request)[0]);
        platform::close_fd((*request)[1]);
        return std::unexpected("cannot create Lua worker response pipe");
    }
    std::optional<std::array<int, 2>> trace_channel;
    if (trace.enabled) {
        trace_channel = platform::unix_pair();
        if (!trace_channel) {
            for (int fd : {(*request)[0], (*request)[1], (*response)[0], (*response)[1]})
                platform::close_fd(fd);
            return std::unexpected("cannot create worker exec audit channel");
        }
    }
    std::optional<std::array<int, 2>> proxy_bridge;
    if (network.proxy) {
        proxy_bridge = platform::unix_pair();
        if (!proxy_bridge || !network.gateway || !network.accept_client || !network.relay_tick ||
            !network.relay_fds) {
            for (int fd : {(*request)[0], (*request)[1], (*response)[0], (*response)[1]})
                platform::close_fd(fd);
            if (trace_channel)
                for (int fd : *trace_channel)
                    platform::close_fd(fd);
            if (proxy_bridge)
                for (int fd : *proxy_bridge)
                    platform::close_fd(fd);
            return std::unexpected("proxy worker relay is unavailable");
        }
    }
    std::optional<std::array<int, 2>> ready, go;
    if (!network.pasta.empty() || network.proxy) {
        ready = platform::make_pipe();
        go = platform::make_pipe();
    }
    if ((!network.pasta.empty() || network.proxy) && (!ready || !go)) {
        for (int fd : {(*request)[0], (*request)[1], (*response)[0], (*response)[1]})
            platform::close_fd(fd);
        if (ready)
            for (int fd : *ready)
                platform::close_fd(fd);
        if (go)
            for (int fd : *go)
                platform::close_fd(fd);
        if (trace_channel)
            for (int fd : *trace_channel)
                platform::close_fd(fd);
        if (proxy_bridge)
            for (int fd : *proxy_bridge)
                platform::close_fd(fd);
        return std::unexpected("cannot create worker network handshake pipes");
    }
    const auto ids = platform::user_ids();
    int pid = platform::fork_process();
    if (pid == 0) {
        if (ready) {
            platform::close_fd((*ready)[0]);
            platform::close_fd((*go)[1]);
            if (!platform::enter_private_network(ids.uid, ids.gid))
                platform::exit_now(125);
            if (proxy_bridge) {
                platform::close_fd((*proxy_bridge)[0]);
                if (!platform::network::enable_loopback())
                    platform::exit_now(125);
                auto listener = platform::network::listen_loopback(network.gateway_port);
                if (!listener)
                    platform::exit_now(125);
                int gateway = platform::fork_process();
                if (gateway == 0) {
                    for (int fd : {(*request)[0], (*request)[1], (*response)[0], (*response)[1],
                                   (*ready)[1], (*go)[0]})
                        platform::close_fd(fd);
                    if (trace_channel)
                        for (int fd : *trace_channel)
                            platform::close_fd(fd);
                    close_except({(*proxy_bridge)[1], *listener});
                    platform::new_session();
                    platform::exit_now(network.gateway((*proxy_bridge)[1], *listener));
                }
                platform::close_fd(*listener);
                platform::close_fd((*proxy_bridge)[1]);
                if (gateway < 0)
                    platform::exit_now(125);
            }
            const char byte = 1;
            if (!write_all((*ready)[1], &byte, 1))
                platform::exit_now(125);
            char proceed{};
            if (!read_all((*go)[0], &proceed, 1) || proceed != 1)
                platform::exit_now(125);
            platform::close_fd((*ready)[1]);
            platform::close_fd((*go)[0]);
        }
        int rd = ::fcntl((*request)[0], F_DUPFD_CLOEXEC, 6),
            wr = ::fcntl((*response)[1], F_DUPFD_CLOEXEC, 6);
        int notify = trace_channel ? ::fcntl((*trace_channel)[1], F_DUPFD_CLOEXEC, 6) : -1;
        if (trace_channel)
            for (int fd : *trace_channel)
                platform::close_fd(fd);
        for (int fd : {(*request)[0], (*request)[1], (*response)[0], (*response)[1]})
            platform::close_fd(fd);
        if (rd < 0 || wr < 0 || ::dup2(rd, 3) < 0 || ::dup2(wr, 4) < 0)
            platform::exit_now(125);
        if (trace_channel && (notify < 0 || ::dup2(notify, 5) < 0))
            platform::exit_now(125);
        platform::close_fd(rd);
        platform::close_fd(wr);
        platform::close_fd(notify);
        platform::new_session();
        platform::reset_signals();
        for (auto& a : argv) {
            if (a == kReadToken)
                a = "3";
            if (a == kWriteToken)
                a = "4";
            if (a == kTraceToken)
                a = "5";
        }
        (void)platform::exec_path(argv, env);
        platform::exit_now(125);
    }
    platform::close_fd((*request)[0]);
    platform::close_fd((*response)[1]);
    if (proxy_bridge)
        platform::close_fd((*proxy_bridge)[1]);
    if (pid < 0) {
        platform::close_fd((*request)[1]);
        platform::close_fd((*response)[0]);
        if (ready)
            for (int fd : *ready)
                platform::close_fd(fd);
        if (go)
            for (int fd : *go)
                platform::close_fd(fd);
        if (trace_channel)
            for (int fd : *trace_channel)
                platform::close_fd(fd);
        if (proxy_bridge)
            platform::close_fd((*proxy_bridge)[0]);
        return std::unexpected("cannot fork Lua worker");
    }
    state->pid = pid;
    state->channel = {(*response)[0], (*request)[1]};
    if (proxy_bridge)
        state->proxy_bridge = (*proxy_bridge)[0];
    if (ready) {
        platform::close_fd((*ready)[1]);
        platform::close_fd((*go)[0]);
        platform::PollFd pending{(*ready)[0]};
        char byte{};
        bool established = platform::poll_fds(std::span(&pending, 1), 10000) > 0 &&
                           read_all((*ready)[0], &byte, 1) && byte == 1;
        if (established && !network.proxy) {
            auto pasta = network.pasta;
            pasta.insert(pasta.end(), {"--pid", network.pid_file.string(), std::to_string(pid)});
            established = platform::run_argv_with_timeout(pasta, std::chrono::seconds(10)) == 0;
            if (established) {
                std::ifstream pid_file(network.pid_file);
                int pasta_pid{};
                established = static_cast<bool>(pid_file >> pasta_pid) && pasta_pid > 1 &&
                              pasta_pid != platform::get_pid();
#if defined(__linux__)
                if (established) {
                    state->pasta_pidfd = static_cast<int>(::syscall(434, pasta_pid, 0));
                    established = state->pasta_pidfd >= 0;
                }
#else
                established = false;
#endif
            }
        }
        const char proceed = established ? 1 : 0;
        write_all((*go)[1], &proceed, 1);
        platform::close_fd((*ready)[0]);
        platform::close_fd((*go)[1]);
        std::error_code ec;
        std::filesystem::remove(network.pid_file, ec);
        if (!established) {
            if (trace_channel)
                for (int fd : *trace_channel)
                    platform::close_fd(fd);
            return std::unexpected("pasta could not establish the Lua worker network");
        }
    }
    if (trace_channel) {
        platform::close_fd((*trace_channel)[1]);
        platform::PollFd ready_listener{(*trace_channel)[0]};
        const bool ready = platform::poll_fds(std::span(&ready_listener, 1), 10000) > 0;
        auto listener = ready ? platform::receive_message((*trace_channel)[0]) : std::nullopt;
        platform::close_fd((*trace_channel)[0]);
        if (!listener || listener->fds.size() != 2 || listener->data != "trace-listeners") {
            if (listener)
                for (int fd : listener->fds)
                    platform::close_fd(fd);
            return std::unexpected("Lua worker could not install its required exec audit filter");
        }
        state->exec_listener = listener->fds[0];
        state->net_listener = listener->fds[1];
    }
    if (trace_channel || proxy_bridge) {
        auto* owned = state.get();
        owned->observer =
            std::thread([owned, audit = trace.audit, net_audit = trace.net_audit, network] {
                while (!owned->stopping.load()) {
                    std::vector<platform::PollFd> pending;
                    if (owned->exec_listener >= 0)
                        pending.push_back({owned->exec_listener});
                    if (owned->net_listener >= 0)
                        pending.push_back({owned->net_listener});
                    if (owned->proxy_bridge >= 0)
                        pending.push_back({owned->proxy_bridge});
                    if (network.proxy) {
                        auto relay = network.relay_fds();
                        pending.insert(pending.end(), relay.begin(), relay.end());
                    }
                    platform::poll_fds(pending, 100);
                    for (const auto& event : pending) {
                        if (!event.readable)
                            continue;
                        if (event.fd == owned->exec_listener) {
                            auto notification = platform::seccomp::next_exec(owned->exec_listener);
                            if (!notification)
                                continue;
                            bool allowed = false;
                            try {
                                allowed = audit && audit(notification->pid, notification->path);
                            } catch (...) {
                            }
                            (void)platform::seccomp::complete_exec(owned->exec_listener,
                                                                   notification->id, allowed);
                            if (!allowed) {
                                platform::send_signal_group(owned->pid, platform::sig::terminate);
                                platform::send_signal(owned->pid, platform::sig::terminate);
                                owned->stopping.store(true);
                                break;
                            }
                        } else if (event.fd == owned->net_listener) {
                            auto notification = platform::net_notify::next(owned->net_listener);
                            if (!notification)
                                continue;
                            bool allowed = false;
                            try {
                                allowed = net_audit && net_audit(*notification);
                            } catch (...) {
                            }
                            (void)platform::net_notify::complete(owned->net_listener,
                                                                 notification->id, allowed);
                            if (!allowed) {
                                platform::send_signal_group(owned->pid, platform::sig::terminate);
                                platform::send_signal(owned->pid, platform::sig::terminate);
                                owned->stopping.store(true);
                                break;
                            }
                        } else if (event.fd == owned->proxy_bridge) {
                            auto client = platform::receive_message(owned->proxy_bridge);
                            if (!client)
                                continue;
                            if (client->data == "proxy-client" && client->fds.size() == 1) {
                                (void)network.accept_client(client->fds.front());
                            } else
                                for (int fd : client->fds)
                                    platform::close_fd(fd);
                        }
                    }
                    if (network.proxy)
                        network.relay_tick();
                }
            });
    }
#endif
    return Process(std::move(state));
}
std::expected<std::string, std::string> Process::exchange(std::string_view message) {
    if (!state_)
        return std::unexpected("Lua worker process unavailable");
    if (!send(state_->channel, message))
        return std::unexpected("cannot write Lua worker request");
    return receive(state_->channel);
}
} // namespace xlings::platform::worker
