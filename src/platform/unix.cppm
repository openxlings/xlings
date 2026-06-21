module;

#include <cstdio>
#include <cstdlib>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pwd.h>
#endif

export module xlings.platform:unix;

import std;

#if defined(__linux__) || defined(__APPLE__)

namespace xlings {
namespace platform_impl {

    // Query the controlling terminal for its background color via the
    // OSC-11 sequence (xterm spec, supported by xterm / iTerm2 / Alacritty
    // / Kitty / WezTerm / modern Windows Terminal). Returns std::nullopt
    // if there's no controlling tty, the terminal doesn't respond within
    // ~50 ms, or the reply is malformed.
    //
    // Lives in the shared `unix` partition because the implementation
    // (open /dev/tty + termios raw mode + select-with-timeout + parse the
    // hex reply) is identical across Linux and macOS — both inherit the
    // POSIX terminal API. windows.cppm provides its own stub. Putting it
    // here means linux.cppm and macos.cppm carry no copy of this code.
    export std::optional<bool> query_terminal_is_light() {
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

        static constexpr char query[] = "\033]11;?\033\\";
        if (::write(fd, query, sizeof(query) - 1) < 0) return std::nullopt;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv{0, 50 * 1000};  // 50 ms
        if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) return std::nullopt;

        char buf[64]{};
        auto n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) return std::nullopt;

        // Reply: \e]11;rgb:RRRR/GGGG/BBBB\e\\ (or \a terminator).
        std::string_view s{buf, static_cast<std::size_t>(n)};
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

    // Atomically replace `dst` with the contents of `src`, even when `dst`
    // is the currently running executable.
    //
    // POSIX semantics: rename(2) is atomic and the kernel keeps the old inode
    // alive for the running process's executable mapping. New invocations
    // resolve the path to the new inode; the running process continues to
    // execute from the old one until it exits. This is the canonical
    // self-update pattern used by apt / brew / rustup / et al.
    //
    // Steps:
    //   1. copy src -> "<dst>.xlings.new" (staged on the same FS as dst so
    //      the rename below is intra-FS and atomic)
    //   2. chmod 0755
    //   3. rename(tmp, dst) — atomic
    //
    // Returns true on success.
    export bool atomic_replace_executable(const std::filesystem::path& src,
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

    // ── Execution identity primitives (root / sudo awareness) ───────
    // Only the OS primitives live here; all policy (sudo parsing, priv
    // prefix, chown-back) is cross-platform in platform.cppm. See
    // .agents/docs/2026-06-21-root-privilege-identity-design.md.

    export bool is_root() {
        return ::geteuid() == 0;
    }

    // Single-path lchown (don't follow symlinks). Best-effort: ownership
    // restoration must never abort an otherwise-successful operation, so
    // the return value is intentionally ignored.
    export void lchown_path_(const std::filesystem::path& p,
                             unsigned int uid, unsigned int gid) {
        ::lchown(p.c_str(), static_cast<uid_t>(uid), static_cast<gid_t>(gid));
    }

    // Resolve a user's home dir from the passwd db. Empty on lookup miss.
    export std::string home_for_user_(const std::string& name) {
        if (name.empty()) return {};
        if (struct passwd* pw = ::getpwnam(name.c_str()); pw && pw->pw_dir)
            return std::string{pw->pw_dir};
        return {};
    }

} // namespace platform_impl
} // namespace xlings

#endif // defined(__linux__) || defined(__APPLE__)
