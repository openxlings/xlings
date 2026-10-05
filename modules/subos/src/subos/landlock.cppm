module;

#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

export module xlings.subos.landlock;

import std;

// Landlock (design §17 FsGate, C18): the kernel restricts what this process
// and everything it starts may WRITE, without a namespace. Reading and
// executing stay open -- it is not a view, the host's files are visible --
// which is why it is a backend of its own (`--sandbox landlock`) and never a
// silent stand-in for bwrap.
//
// The ABI is defined here rather than taken from <linux/landlock.h>: the musl
// toolchain's kernel headers can predate it, and these numbers are the
// kernel's stable interface (the same syscall numbers on every architecture,
// 5.13 and later).
export namespace xlings::subos::landlock {

namespace fs = std::filesystem;

// What session-init reads to fence itself before it starts anything: the
// writable paths, one per line.
inline constexpr std::string_view kRwEnv = "XLINGS_SESSION_LANDLOCK_RW";

// 0 when the kernel has no Landlock, or it is disabled.
int abi();

// From here on, this process and its children may write only beneath `rw`.
// Paths that do not exist are skipped. Sets no_new_privs, as Landlock needs.
std::expected<void, std::string> restrict_writes(std::span<const fs::path> rw);

}  // namespace xlings::subos::landlock

namespace xlings::subos::landlock {

#if defined(__linux__)
namespace {

constexpr long kCreateRuleset = 444, kAddRule = 445, kRestrictSelf = 446;
constexpr std::uint32_t kCreateRulesetVersion = 1u << 0;
constexpr int kRulePathBeneath = 1;

// Every right that changes the filesystem, by the ABI that introduced it.
constexpr std::uint64_t kWriteAbi1 =
    (1ull << 1)  /* WRITE_FILE  */ | (1ull << 4)  /* REMOVE_DIR */ | (1ull << 5)  /* REMOVE_FILE */ |
    (1ull << 6)  /* MAKE_CHAR   */ | (1ull << 7)  /* MAKE_DIR   */ | (1ull << 8)  /* MAKE_REG    */ |
    (1ull << 9)  /* MAKE_SOCK   */ | (1ull << 10) /* MAKE_FIFO  */ | (1ull << 11) /* MAKE_BLOCK  */ |
    (1ull << 12) /* MAKE_SYM    */;
constexpr std::uint64_t kRefer = 1ull << 13;      // ABI 2: rename / link across directories
constexpr std::uint64_t kTruncate = 1ull << 14;   // ABI 3

struct RulesetAttr { std::uint64_t handled_access_fs; };
struct [[gnu::packed]] PathBeneathAttr { std::uint64_t allowed_access; std::int32_t parent_fd; };

}  // namespace

int abi() {
    const long v = ::syscall(kCreateRuleset, nullptr, 0, kCreateRulesetVersion);
    return v < 0 ? 0 : static_cast<int>(v);
}

std::expected<void, std::string> restrict_writes(std::span<const fs::path> rw) {
    const int version = abi();
    if (version < 1) return std::unexpected("Landlock is not available on this kernel");
    std::uint64_t handled = kWriteAbi1;
    if (version >= 2) handled |= kRefer;
    if (version >= 3) handled |= kTruncate;

    RulesetAttr attr{ handled };
    const int ruleset = static_cast<int>(::syscall(kCreateRuleset, &attr, sizeof attr, 0));
    if (ruleset < 0) return std::unexpected(std::format("landlock_create_ruleset: {}", std::strerror(errno)));
    for (const auto& p : rw) {
        const int fd = ::open(p.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) continue;   // a path that is not there grants nothing
        PathBeneathAttr rule{ handled, fd };
        // A file (not a directory) takes only the rights that apply to files.
        struct stat st{};
        if (::fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode))
            rule.allowed_access = handled & ((1ull << 1) | kTruncate);
        const long r = ::syscall(kAddRule, ruleset, kRulePathBeneath, &rule, 0);
        const int err = errno;
        ::close(fd);
        if (r < 0) {
            ::close(ruleset);
            return std::unexpected(std::format("landlock_add_rule {}: {}", p.string(), std::strerror(err)));
        }
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
        || ::syscall(kRestrictSelf, ruleset, 0) != 0) {
        const int err = errno;
        ::close(ruleset);
        return std::unexpected(std::format("landlock_restrict_self: {}", std::strerror(err)));
    }
    ::close(ruleset);
    return {};
}
#else
int abi() { return 0; }
std::expected<void, std::string> restrict_writes(std::span<const fs::path>) {
    return std::unexpected("Landlock is Linux-only");
}
#endif

}  // namespace xlings::subos::landlock
