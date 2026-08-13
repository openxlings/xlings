module;

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/file.h>
#include <pwd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)   // <linux/fs.h>; defined here to avoid header clashes
#endif
#endif

module xlings.platform;

import std;

#if defined(__linux__) || defined(__APPLE__)

namespace xlings {

namespace platform_impl {

std::optional<bool> parse_terminal_bg_is_light(std::string_view s) {
        auto p = s.find("rgb:");
        if (p == std::string_view::npos || p + 4 + 14 > s.size()) return std::nullopt;
        auto hex4 = [&](std::size_t off) -> int {
            int v = 0;
            for (int i = 0; i < 4 && off + i < s.size(); ++i) {
                char c = s[off + i];
                int d = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0) return -1;
                v = v * 16 + d;
            }
            return v;  // 16-bit channel, 0..65535
        };
        int r = hex4(p + 4), g = hex4(p + 9), b = hex4(p + 14);
        if (r < 0 || g < 0 || b < 0) return std::nullopt;
        // Rec. 601 luma; threshold at half-bright.
        return (0.299 * r + 0.587 * g + 0.114 * b) > 32768.0;
    }

std::string read_terminal_query_reply(int fd,
                                                 std::chrono::milliseconds timeout) {
        std::string acc;
        acc.reserve(64);
        constexpr std::size_t kCap = 512;   // bounded buffer
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          deadline - now).count();
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv{
                static_cast<time_t>(us / 1000000),
                static_cast<suseconds_t>(us % 1000000)
            };
            int rv = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (rv < 0) { if (errno == EINTR) continue; break; }
            if (rv == 0) break;                     // deadline reached
            char buf[256];
            auto n = ::read(fd, buf, sizeof(buf));
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;                      // EOF
            acc.append(buf, static_cast<std::size_t>(n));
            if (acc.find('R') != std::string::npos) break;   // CPR fence
            if (acc.size() >= kCap) break;
        }
        return acc;
    }

    // Overall deadline for the terminal background query. Default 500 ms,
    // overridable via XLINGS_TERM_QUERY_TIMEOUT_MS (clamped to [50, 5000]).
    // Responsive terminals hit the CPR fence in a few ms and return early;
    // the deadline only bites on a tty that answers neither OSC-11 nor DSR.
    
std::chrono::milliseconds term_query_timeout_() {
        long ms = 500;
        if (const char* v = std::getenv("XLINGS_TERM_QUERY_TIMEOUT_MS")) {
            char* end = nullptr;
            long parsed = std::strtol(v, &end, 10);
            if (end != v && parsed > 0) ms = parsed;
        }
        if (ms < 50)   ms = 50;
        if (ms > 5000) ms = 5000;
        return std::chrono::milliseconds{ms};
    }

std::optional<bool> query_terminal_is_light() {
        // If our own stdout AND stderr are not terminals, we're a child whose
        // output is captured by a parent (e.g. mcpp driving `xlings interface`
        // over a pipe). Do NOT poke /dev/tty then: the OSC-11 query races with
        // the parent's rendering and the raw reply (\e]11;rgb:...) leaks into
        // the visible terminal and desyncs its input on some terminals (Termux),
        // which is what makes the user have to press Enter. Fall back to
        // env/default detection in that case.
        if (!::isatty(STDOUT_FILENO) && !::isatty(STDERR_FILENO)) return std::nullopt;

        int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd < 0) return std::nullopt;
        struct CloseGuard { int fd; ~CloseGuard() { ::close(fd); } } _g{fd};

        struct termios saved{};
        if (::tcgetattr(fd, &saved) != 0) return std::nullopt;
        struct termios raw = saved;
        raw.c_lflag &= ~(static_cast<tcflag_t>(ICANON) | static_cast<tcflag_t>(ECHO));
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(fd, TCSANOW, &raw) != 0) return std::nullopt;
        struct RestoreGuard {
            int fd; struct termios s;
            ~RestoreGuard() { ::tcsetattr(fd, TCSANOW, &s); }
        } _r{fd, saved};

        // OSC-11 background query, immediately followed by a DSR cursor
        // position request. The CPR reply to the DSR is our fence (see
        // read_terminal_query_reply): once it arrives we know the OSC-11
        // reply, if any, has fully arrived and been consumed — so ECHO is
        // never restored with terminal bytes still pending in the input.
        static constexpr char query[] = "\033]11;?\033\\\033[6n";
        if (::write(fd, query, sizeof(query) - 1) < 0) return std::nullopt;

        std::string reply = read_terminal_query_reply(fd, term_query_timeout_());
        return parse_terminal_bg_is_light(reply);
    }

bool displace_locked_file(const std::filesystem::path& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(path, ec) && !fs::is_symlink(path, ec)) return true;
        ec.clear();
        fs::remove(path, ec);
        ec.clear();
        return !fs::exists(path, ec) && !fs::is_symlink(path, ec);
    }

bool atomic_replace_executable(const std::filesystem::path& src,
                                          const std::filesystem::path& dst) {
        namespace fs = std::filesystem;
        std::error_code ec;

        if (!fs::exists(src, ec) || ec) return false;

        fs::create_directories(dst.parent_path(), ec);
        ec.clear();

        auto tmp = dst;
        tmp += ".xlings.new";

        // Pre-clean any leftover from a previous interrupted run.
        if (fs::exists(tmp, ec)) {
            fs::remove(tmp, ec);
            ec.clear();
        }

        fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::error_code rmec;
            fs::remove(tmp, rmec);
            return false;
        }

        fs::permissions(tmp,
            fs::perms::owner_all
          | fs::perms::group_read | fs::perms::group_exec
          | fs::perms::others_read | fs::perms::others_exec,
            fs::perm_options::replace, ec);
        ec.clear();

        fs::rename(tmp, dst, ec);
        if (!ec) return true;

        // Cross-filesystem fallback (rare: ec == EXDEV). copy + remove tmp.
        if (ec == std::errc::cross_device_link) {
            ec.clear();
            fs::copy_file(tmp, dst, fs::copy_options::overwrite_existing, ec);
            std::error_code rmec;
            fs::remove(tmp, rmec);
            return !ec;
        }

        std::error_code rmec;
        fs::remove(tmp, rmec);
        return false;
    }

bool atomic_swap_paths(const std::filesystem::path& a,
                                  const std::filesystem::path& b) {
#if defined(__linux__)
        long rc = ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(),
                            AT_FDCWD, b.c_str(),
                            static_cast<unsigned int>(RENAME_EXCHANGE));
        return rc == 0;
#else
        // macOS has renamex_np(.., RENAME_SWAP) but it is not wired up yet;
        // returning false keeps the portable manual-swap fallback in charge.
        (void)a; (void)b;
        return false;
#endif
    }

bool is_root() {
        return ::geteuid() == 0;
    }

void lchown_path_(const std::filesystem::path& p,
                             unsigned int uid, unsigned int gid) {
        ::lchown(p.c_str(), static_cast<uid_t>(uid), static_cast<gid_t>(gid));
    }

std::string home_for_user_(const std::string& name) {
        if (name.empty()) return {};
        if (struct passwd* pw = ::getpwnam(name.c_str()); pw && pw->pw_dir)
            return std::string{pw->pw_dir};
        return {};
    }

}

}

 // namespace xlings

#endif // defined(__linux__) || defined(__APPLE__)


// ── out-of-line class members ──────────────────────────────────

#if (defined(__linux__) || defined(__APPLE__))

namespace xlings::platform_impl {

FileLock::FileLock(FileLock&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

FileLock::~FileLock() { release(); }

bool FileLock::acquire(const std::filesystem::path& path,
                     std::chrono::milliseconds timeout,
                     const std::function<bool()>& cancelled,
                     std::string& error) {
    release();
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        error = std::format("failed to open lock {}: {}",
                            path.string(), std::strerror(errno));
        return false;
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            error = std::format("failed to lock {}: {}",
                                path.string(), std::strerror(errno));
            release();
            return false;
        }
        if (cancelled && cancelled()) {
            error = "cancelled while waiting for cache lock";
            release();
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = std::format("timed out waiting for cache lock {}",
                                path.string());
            release();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return true;
}

void FileLock::release() {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

} // namespace xlings::platform_impl

#endif
