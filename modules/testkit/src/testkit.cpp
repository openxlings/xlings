module;

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define XTEST_ENVIRON (*_NSGetEnviron())
#else
// glibc and musl declare environ in <unistd.h> under _GNU_SOURCE
#define XTEST_ENVIRON environ
#endif
#endif

module xlings.testkit;

import std;
import xlings.libs.json;

namespace xlings::testkit {

namespace {

std::string env_or(const char* name, std::string fallback = {}) {
    if (const char* v = std::getenv(name); v && *v) return v;
    return fallback;
}

std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find(',', start);
        if (end == std::string_view::npos) end = s.size();
        auto item = s.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) out.emplace_back(item);
        start = end + 1;
    }
    return out;
}

// Never destroyed: the meta writer runs during static destruction, and on
// libc++ a function-local static destroyed before it turns the last write into
// `mutex lock failed: Invalid argument` (macOS).
std::map<std::string, Meta, std::less<>>& registry_mut() {
    static auto* r = new std::map<std::string, Meta, std::less<>>();
    return *r;
}

FailureProbe g_failed = nullptr;
NameProbe g_name = nullptr;

std::mutex& out_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

void append_line(const std::string& path, const std::string& line) {
    std::lock_guard lock(out_mutex());
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f << line << '\n';
}

// Written once, at exit, so a filtered run still lists every test it holds.
struct MetaWriter {
    ~MetaWriter() {
        auto path = env_or("XTEST_META_OUT");
        if (path.empty()) return;
        for (auto& [name, m] : registry_mut()) {
            nlohmann::json j;
            j["test"] = name;
            j["area"] = m.area;
            j["cost"] = std::string(to_string(m.cost));
            j["covers"] = m.covers;
            j["requires"] = m.requires_;
            j["resources"] = m.resources;
            j["proves"] = m.proves;
            append_line(path, j.dump());
        }
    }
};
MetaWriter g_meta_writer;

}  // namespace

std::string_view to_string(Cost c) {
    switch (c) {
    case Cost::Medium: return "medium";
    case Cost::Slow:   return "slow";
    default:           return "fast";
    }
}

bool register_meta(std::string_view test, Meta meta) {
    registry_mut().insert_or_assign(std::string(test), std::move(meta));
    return true;
}

const std::map<std::string, Meta, std::less<>>& registry() { return registry_mut(); }

// ── Capabilities ─────────────────────────────────────────────────────

namespace {

constexpr std::array<std::string_view, 14> kCapabilities {
    "linux", "macos", "windows", "posix",
    "xlings-bin",   // a built binary to drive
    "pty",          // pseudo-terminals (agent contract scan)
    "userns",       // unprivileged user namespaces
    "bwrap",        // a bwrap that can create a sandbox here
    "sandbox",      // `subos use --sandbox` can enter (any backend)
    "network",      // the lane allows network access (opt-in)
    "root",
    "sudo",         // non-interactive sudo
    "pasta",        // pasta (passt) and /dev/net/tun: net=nat can run
    "landlock",     // the kernel answers landlock_create_ruleset (ABI >= 1)
};

// Run a probe command quietly; true when it exits 0.
bool quiet_ok(const std::vector<std::string>& argv) {
    RunOptions o;
    o.argv = argv;
    o.env = { {"PATH", "/usr/local/bin:/usr/bin:/bin"} };
    o.timeout = std::chrono::seconds(20);
    auto r = run(o);
    return r.exit_code == 0;
}

std::optional<std::string> probe_uncached(std::string_view cap) {
    if (cap == "linux") {
        if constexpr (is_linux) return std::nullopt;
        else return "not Linux";
    }
    if (cap == "macos") {
        if constexpr (is_macos) return std::nullopt;
        else return "not macOS";
    }
    if (cap == "windows") {
        if constexpr (is_windows) return std::nullopt;
        else return "not Windows";
    }
    if (cap == "posix" || cap == "pty") {
        if constexpr (is_posix) return std::nullopt;
        else return "not POSIX";
    }
    if (cap == "xlings-bin") {
        if (xlings_binary().empty())
            return "no xlings binary (set XLINGS_BIN or run `mcpp build`)";
        return std::nullopt;
    }
    if (cap == "network") {
        if (env_or("XDEV_NETWORK") == "1" || lane_declares("network"))
            return std::nullopt;
        return "network is opt-in (XDEV_NETWORK=1); runs are hermetic by default";
    }
#if defined(_WIN32)
    return "not available on Windows";
#else
    if (cap == "root") {
        if (::geteuid() == 0) return std::nullopt;
        return "not running as root";
    }
    if (cap == "sudo") {
        if (::geteuid() == 0 || quiet_ok({"sudo", "-n", "true"})) return std::nullopt;
        return "no non-interactive sudo";
    }
    if (cap == "userns") {
        if constexpr (!is_linux) return "user namespaces are Linux-only";
        if (quiet_ok({"unshare", "-Ur", "true"})) return std::nullopt;
        return "unprivileged user namespaces are not available "
               "(kernel.apparmor_restrict_unprivileged_userns or "
               "kernel.unprivileged_userns_clone)";
    }
    if (cap == "bwrap") {
        if constexpr (!is_linux) return "bwrap is Linux-only";
        for (const auto& candidate : {env_or("XDEV_BWRAP"),
                                      std::string("/usr/lib/xlings/bwrap"),
                                      std::string("/usr/bin/bwrap"),
                                      std::string("/usr/local/bin/bwrap")}) {
            if (candidate.empty() || !fs::exists(candidate)) continue;
            if (quiet_ok({candidate, "--ro-bind", "/", "/", "--", "/bin/true"}))
                return std::nullopt;
        }
        return "no bwrap that can create a sandbox on this host";
    }
    if (cap == "pasta") {
        if constexpr (!is_linux) return "pasta is Linux-only";
        std::error_code ec;
        if (!fs::exists("/dev/net/tun", ec)) return "/dev/net/tun is missing";
        for (auto p : {"/usr/bin/pasta", "/usr/local/bin/pasta"})
            if (fs::exists(p, ec)) return std::nullopt;
        return "pasta (passt) is not installed";
    }
    if (cap == "landlock") {
        if constexpr (!is_linux) return "Landlock is Linux-only";
#if defined(__linux__)
        // landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)
        if (::syscall(444, nullptr, 0, 1u) >= 1) return std::nullopt;
#endif
        return "the kernel has no Landlock, or it is not in the LSM list";
    }
    if (cap == "sandbox") {
        // Elsewhere home-redirect is always available.
        if constexpr (is_linux) return probe("bwrap");
        else return std::nullopt;
    }
    return "unknown capability";
#endif
}

}  // namespace

std::span<const std::string_view> capability_names() { return kCapabilities; }

std::optional<std::string> probe(std::string_view capability) {
    static std::mutex m;
    static std::map<std::string, std::optional<std::string>, std::less<>> cache;
    {
        std::lock_guard lock(m);
        if (auto it = cache.find(capability); it != cache.end()) return it->second;
    }
    auto result = probe_uncached(capability);
    std::lock_guard lock(m);
    cache.insert_or_assign(std::string(capability), result);
    return result;
}

bool lane_declares(std::string_view capability) {
    static const auto caps = split_csv(env_or("XDEV_LANE_CAPS"));
    return std::ranges::find(caps, capability) != caps.end();
}

std::optional<Verdict> check_requirements(const Meta& meta) {
    static const auto caps = split_csv(env_or("XDEV_LANE_CAPS"));
    return check_requirements(meta, caps);
}

std::optional<Verdict> check_requirements(const Meta& meta,
                                          std::span<const std::string> lane_caps) {
    auto declared = [&](std::string_view cap) {
        return std::ranges::find(lane_caps, cap) != lane_caps.end();
    };
    for (const auto& cap : meta.requires_) {
        if (std::ranges::find(kCapabilities, std::string_view(cap)) == kCapabilities.end()) {
            return Verdict{ .fail = true,
                            .reason = "unknown capability '" + cap + "' in requires" };
        }
        if (auto why = probe(cap)) {
            if (declared(cap)) {
                return Verdict{ .fail = true,
                                .reason = "this lane declares '" + cap
                                          + "' (XDEV_LANE_CAPS) but it is missing: " + *why };
            }
            return Verdict{ .fail = false, .reason = "requires " + cap + ": " + *why };
        }
    }
    return std::nullopt;
}

// ── Processes ────────────────────────────────────────────────────────

std::vector<nlohmann::json> RunResult::json_lines() const {
    std::vector<nlohmann::json> lines;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() != '{') continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_discarded() && j.is_object()) lines.push_back(std::move(j));
    }
    return lines;
}

std::string RunResult::transcript() const {
    std::string s = std::format("exit={} signal={} timed_out={} elapsed={}ms\n",
                                exit_code, signal, timed_out, elapsed.count());
    s += "--- stdout\n" + out;
    if (!out.empty() && out.back() != '\n') s += '\n';
    s += "--- stderr\n" + err;
    return s;
}

#if !defined(_WIN32)

namespace {

std::string resolve_program(const std::string& prog,
                            const std::map<std::string, std::string>& env) {
    if (prog.find('/') != std::string::npos) return prog;
    std::string path = "/usr/bin:/bin";
    if (auto it = env.find("PATH"); it != env.end()) path = it->second;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        auto dir = path.substr(start, end - start);
        if (!dir.empty()) {
            auto candidate = dir + "/" + prog;
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        start = end + 1;
    }
    return prog;
}

void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

RunResult run(const RunOptions& o) {
    RunResult r;
    if (o.argv.empty()) { r.err = "testkit::run: empty argv"; return r; }

    auto program = resolve_program(o.argv[0], o.env);
    std::vector<std::string> env_strings;
    for (auto& [k, v] : o.env) env_strings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> argv_copy = o.argv;
    std::vector<char*> argv;
    for (auto& s : argv_copy) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto cwd = o.cwd.empty() ? fs::temp_directory_path() : o.cwd;
    auto cwd_s = cwd.string();

    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    int master = -1;
    std::string slave_name;
    if (o.pty) {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            r.err = std::format("testkit::run: no pty: {}", std::strerror(errno));
            if (master >= 0) ::close(master);
            return r;
        }
        slave_name = ::ptsname(master);
        ::fcntl(master, F_SETFD, FD_CLOEXEC);
    } else {
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            r.err = std::format("testkit::run: pipe: {}", std::strerror(errno));
            return r;
        }
        for (int fd : {in_pipe[1], out_pipe[0], err_pipe[0]})
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    const auto started = std::chrono::steady_clock::now();
    pid_t pid = ::fork();
    if (pid < 0) {
        r.err = std::format("testkit::run: fork: {}", std::strerror(errno));
        return r;
    }
    if (pid == 0) {
        if (o.pty) {
            ::setsid();
            int slave = ::open(slave_name.c_str(), O_RDWR);
            if (slave < 0) ::_exit(126);
            ::ioctl(slave, TIOCSCTTY, 0);
            ::dup2(slave, 0); ::dup2(slave, 1); ::dup2(slave, 2);
            if (slave > 2) ::close(slave);
        } else {
            ::setpgid(0, 0);
            ::dup2(in_pipe[0], 0);
            ::dup2(out_pipe[1], 1);
            ::dup2(err_pipe[1], 2);
            for (int fd : {in_pipe[0], out_pipe[1], err_pipe[1]}) if (fd > 2) ::close(fd);
        }
        if (::chdir(cwd_s.c_str()) != 0) ::_exit(126);
        ::execve(program.c_str(), argv.data(), envp.data());
        std::fprintf(stderr, "testkit: exec %s: %s\n", program.c_str(), std::strerror(errno));
        ::_exit(127);
    }

    std::vector<int> readers;
    if (o.pty) {
        if (!o.stdin_data.empty())
            (void)::write(master, o.stdin_data.data(), o.stdin_data.size());
        set_nonblock(master);
        readers = {master};
    } else {
        ::close(in_pipe[0]); ::close(out_pipe[1]); ::close(err_pipe[1]);
        if (!o.stdin_data.empty())
            (void)::write(in_pipe[1], o.stdin_data.data(), o.stdin_data.size());
        ::close(in_pipe[1]);
        set_nonblock(out_pipe[0]);
        set_nonblock(err_pipe[0]);
        readers = {out_pipe[0], err_pipe[0]};
    }

    const auto deadline = started + o.timeout;
    std::optional<std::chrono::steady_clock::time_point> drain_until;
    int status = 0;
    bool reaped = false;
    std::array<char, 8192> buf{};
    auto open_count = [&] {
        return std::ranges::count_if(readers, [](int fd) { return fd >= 0; });
    };

    while (true) {
        if (!reaped) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                reaped = true;
                // Grandchildren may hold the pipes; read what is there, then stop.
                drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (!reaped && now >= deadline) {
            r.timed_out = true;
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            reaped = true;
            drain_until = now + std::chrono::milliseconds(200);
        }
        if (open_count() == 0 && reaped) break;
        if (drain_until && now >= *drain_until) break;

        std::vector<pollfd> pfds;
        for (int fd : readers) if (fd >= 0) pfds.push_back({fd, POLLIN, 0});
        if (pfds.empty()) {
            ::usleep(10 * 1000);
            continue;
        }
        ::poll(pfds.data(), pfds.size(), 50);
        for (auto& p : pfds) {
            if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = ::read(p.fd, buf.data(), buf.size());
            auto& sink = (o.pty || p.fd == out_pipe[0]) ? r.out : r.err;
            if (n > 0) {
                sink.append(buf.data(), static_cast<std::size_t>(n));
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                // EOF, or EIO on a pty whose last slave closed.
                for (auto& fd : readers) if (fd == p.fd) { ::close(fd); fd = -1; }
            }
        }
    }
    for (int fd : readers) if (fd >= 0) ::close(fd);

    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!r.timed_out) {
        if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) r.signal = WTERMSIG(status);
    }
    return r;
}

#else  // _WIN32

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The MSVC CRT's argv rules: backslashes are literal unless they precede a
// quote, where they double.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring q = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') q.append(backslashes * 2 + 1, L'\\');
        else q.append(backslashes, L'\\');
        backslashes = 0;
        q.push_back(c);
    }
    q.append(backslashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}

void drain(HANDLE h, std::string& sink) {
    std::array<char, 8192> buf{};
    DWORD n = 0;
    while (::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
        sink.append(buf.data(), n);
}

}  // namespace

RunResult run(const RunOptions& o) {
    RunResult r;
    if (o.argv.empty()) { r.err = "testkit::run: empty argv"; return r; }
    if (o.pty) { r.err = "testkit::run: pty is POSIX-only"; return r; }

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE in_r{}, in_w{}, out_r{}, out_w{}, err_r{}, err_w{};
    ::CreatePipe(&in_r, &in_w, &sa, 0);
    ::CreatePipe(&out_r, &out_w, &sa, 0);
    ::CreatePipe(&err_r, &err_w, &sa, 0);
    ::SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline;
    for (auto& a : o.argv) {
        if (!cmdline.empty()) cmdline.push_back(L' ');
        cmdline += quote_arg(widen(a));
    }
    std::wstring env_block;
    for (auto& [k, v] : o.env) {
        env_block += widen(k + "=" + v);
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');
    auto cwd = (o.cwd.empty() ? fs::temp_directory_path() : o.cwd).wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const auto started = std::chrono::steady_clock::now();
    BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                               CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                               env_block.data(), cwd.c_str(), &si, &pi);
    ::CloseHandle(in_r); ::CloseHandle(out_w); ::CloseHandle(err_w);
    if (!ok) {
        r.err = std::format("testkit::run: CreateProcess failed ({})", ::GetLastError());
        ::CloseHandle(in_w); ::CloseHandle(out_r); ::CloseHandle(err_r); ::CloseHandle(job);
        return r;
    }
    ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);

    std::thread t_out([&] { drain(out_r, r.out); });
    std::thread t_err([&] { drain(err_r, r.err); });
    if (!o.stdin_data.empty()) {
        DWORD written = 0;
        ::WriteFile(in_w, o.stdin_data.data(), static_cast<DWORD>(o.stdin_data.size()), &written, nullptr);
    }
    ::CloseHandle(in_w);

    auto wait = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(o.timeout.count()));
    if (wait == WAIT_TIMEOUT) {
        r.timed_out = true;
        ::TerminateJobObject(job, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(job);   // kills any grandchild still holding the pipes
    t_out.join();
    t_err.join();
    ::CloseHandle(out_r); ::CloseHandle(err_r);
    ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread);

    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!r.timed_out) r.exit_code = static_cast<int>(code);
    return r;
}

#endif

std::map<std::string, std::string> inherited_env() {
    std::map<std::string, std::string> e;
#if defined(_WIN32)
    wchar_t* block = ::GetEnvironmentStringsW();
    for (wchar_t* p = block; p && *p; p += std::wcslen(p) + 1) {
        std::wstring w(p);
        auto eq = w.find(L'=', 1);   // "=C:=C:\" entries start with '='
        if (eq == std::wstring::npos) continue;
        auto narrow = [](const std::wstring& ws) {
            int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                          nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                  out.data(), n, nullptr, nullptr);
            return out;
        };
        e[narrow(w.substr(0, eq))] = narrow(w.substr(eq + 1));
    }
    if (block) ::FreeEnvironmentStringsW(block);
#else
    for (char** p = XTEST_ENVIRON; p && *p; ++p) {
        std::string_view kv(*p);
        auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        e[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
    }
#endif
    return e;
}

fs::path xlings_binary() {
    static const fs::path cached = [] {
        // Only a FILE. The xlings shell profile exports XLINGS_BIN as the
        // installed home's bin DIRECTORY, so in a developer's shell the name
        // already means "the xlings you use", which is not the one under test.
        if (auto e = env_or("XLINGS_BIN"); !e.empty()) {
            std::error_code ec;
            if (fs::is_regular_file(e, ec)) {
                auto abs = fs::absolute(e, ec);
                return ec ? fs::path(e) : abs;
            }
        }
        const fs::path exe = is_windows ? "xlings.exe" : "xlings";
        fs::path newest;
        fs::file_time_type newest_time{};
        std::error_code ec;
        if (fs::exists("target", ec)) {
            for (auto it = fs::recursive_directory_iterator("target", ec);
                 !ec && it != std::default_sentinel; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (it->path().filename() != exe) continue;
                if (it->path().parent_path().filename() != "bin") continue;
                auto t = fs::last_write_time(it->path(), ec);
                if (ec) continue;
                if (newest.empty() || t > newest_time) {
                    newest = it->path();
                    newest_time = t;
                }
            }
        }
        if (newest.empty()) return fs::path{};
        return fs::absolute(newest, ec);
    }();
    return cached;
}

// ── Homes ────────────────────────────────────────────────────────────

std::string current_user() {
#if defined(_WIN32)
    return env_or("USERNAME", "user");
#else
    if (auto u = env_or("USER"); !u.empty()) return u;
    if (auto* pw = ::getpwuid(::getuid()); pw && pw->pw_name) return pw->pw_name;
    return "user";
#endif
}

Home Home::isolated(std::string_view name) {
    static std::atomic<int> counter{0};
    Home h;
#if defined(_WIN32)
    auto pid = static_cast<long>(::GetCurrentProcessId());
#else
    auto pid = static_cast<long>(::getpid());
#endif
    h.root_ = fs::temp_directory_path()
              / std::format("xtest-{}-{}-{}", name, pid, counter++);
    std::error_code ec;
    fs::remove_all(h.root_, ec);
    h.dir_ = h.root_ / ".xlings";
    fs::create_directories(h.dir_);
    fs::create_directories(h.root_ / "tmp");
    // The mirror the shell suite uses, for the same reason (AGENTS.md: an
    // unreachable mirror looks like the command under test hanging).
    nlohmann::json cfg;
    cfg["mirror"] = env_or("XLINGS_TEST_MIRROR", "GLOBAL");
    write_file(h.dir_ / ".xlings.json", cfg.dump(2));
    return h;
}

Home::Home(Home&& o) noexcept
    : root_(std::move(o.root_)), dir_(std::move(o.dir_)), keep_(o.keep_) {
    o.root_.clear();
}

Home& Home::operator=(Home&& o) noexcept {
    if (this != &o) {
        root_ = std::move(o.root_);
        dir_ = std::move(o.dir_);
        keep_ = o.keep_;
        o.root_.clear();
    }
    return *this;
}

Home::~Home() {
    if (root_.empty()) return;
    const bool failed = g_failed && g_failed();
    std::error_code ec;
    if (failed) {
        // Keep what explains the failure: the home's config, logs and state.
        // Payloads are left out -- they are large and say nothing new.
        auto test = g_name ? g_name() : std::string("unknown");
        auto dest = artifacts_dir() / test;
        fs::create_directories(dest, ec);
        for (auto rel : {".xlings.json", "logs", "state", "config"}) {
            auto src = dir_ / rel;
            if (fs::exists(src, ec))
                fs::copy(src, dest / rel, fs::copy_options::recursive
                                          | fs::copy_options::overwrite_existing, ec);
        }
        std::cerr << "[testkit] home kept for the failed test: " << root_.string()
                  << "\n[testkit] artefacts: " << dest.string() << "\n";
        return;
    }
    if (keep_ || env_or("XTEST_KEEP") == "1") {
        std::cerr << "[testkit] home kept: " << root_.string() << "\n";
        return;
    }
    fs::remove_all(root_, ec);
}

std::map<std::string, std::string> Home::env() const {
    std::map<std::string, std::string> e;
    e["XLINGS_HOME"] = dir_.string();
    if constexpr (is_windows) {
        // Windows cannot start much without these; none of them names a home.
        for (auto name : {"SystemRoot", "SystemDrive", "WINDIR", "COMSPEC", "PATHEXT",
                          "PATH", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "OS",
                          "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "ProgramData",
                          "CommonProgramFiles", "PSModulePath", "ALLUSERSPROFILE", "PUBLIC"}) {
            if (auto v = env_or(name); !v.empty()) e[name] = v;
        }
        e["USERPROFILE"] = root_.string();
        e["USERNAME"] = current_user();
        e["TEMP"] = (root_ / "tmp").string();
        e["TMP"] = (root_ / "tmp").string();
    } else {
        e["HOME"] = root_.string();
        e["USER"] = current_user();
        e["LOGNAME"] = current_user();
        e["PATH"] = "/usr/local/bin:/usr/bin:/bin";
        e["SHELL"] = "/bin/sh";
        e["LANG"] = "C.UTF-8";
        e["TMPDIR"] = (root_ / "tmp").string();
    }
    // The lane's mirror and network choices pass through; nothing else does.
    for (auto name : {"XLINGS_RELEASE_MIRROR", "XLINGS_TEST_MIRROR"}) {
        if (auto v = env_or(name); !v.empty()) e[name] = v;
    }
    return e;
}

RunResult Home::xlings(std::vector<std::string> args,
                       std::map<std::string, std::string> extra_env,
                       std::chrono::milliseconds timeout) const {
    RunOptions o;
    o.argv = std::move(args);
    o.env = std::move(extra_env);
    o.timeout = timeout;
    return xlings(std::move(o));
}

RunResult Home::xlings(RunOptions o) const {
    auto bin = xlings_binary();
    o.argv.insert(o.argv.begin(), bin.string());
    auto base = env();
    for (auto& [k, v] : o.env) base[k] = v;
    o.env = std::move(base);
    if (o.cwd.empty()) o.cwd = root_;
    return run(o);
}

bool Home::seed_sandbox_backend() const {
    // Elsewhere the sandbox is home-redirect and needs no binary.
    if constexpr (!is_linux) return true;
    for (const auto& candidate : {env_or("XDEV_BWRAP"),
                                  std::string("/usr/lib/xlings/bwrap"),
                                  std::string("/usr/bin/bwrap"),
                                  std::string("/usr/local/bin/bwrap")}) {
        if (candidate.empty() || !fs::exists(candidate)) continue;
        auto dest = dir_ / "data" / "xpkgs" / "xim-x-bwrap" / "0.0.0-host" / "bin";
        std::error_code ec;
        fs::create_directories(dest, ec);
        fs::copy_file(candidate, dest / "bwrap", fs::copy_options::overwrite_existing, ec);
        if (!ec) return true;
    }
    return false;
}

// ── Failure capture and results ──────────────────────────────────────

void set_probes(FailureProbe failed, NameProbe name) {
    g_failed = failed;
    g_name = name;
}

fs::path artifacts_dir() {
    if (auto e = env_or("XTEST_ARTIFACTS"); !e.empty()) return e;
    std::error_code ec;
    return fs::absolute("target/xtest-artifacts", ec);
}

void record_result(std::string_view test, std::string_view status,
                   long long elapsed_ms, std::string_view message) {
    auto path = env_or("XTEST_RESULTS_OUT");
    if (path.empty()) return;
    nlohmann::json j;
    j["test"] = test;
    j["status"] = status;
    j["ms"] = elapsed_ms;
    if (!message.empty()) j["message"] = message;
    if (auto it = registry_mut().find(test); it != registry_mut().end()) {
        j["area"] = it->second.area;
        j["covers"] = it->second.covers;
        j["proves"] = it->second.proves;
    }
    append_line(path, j.dump());
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace xlings::testkit
