module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/stat.h>
#include <sys/utsname.h>
#endif

module xlings.platform;

import std;

namespace xlings::platform {

namespace fs = std::filesystem;

#if defined(__linux__)

// ── namespaces ───────────────────────────────────────────────────────

bool enter_private_network(unsigned uid, unsigned gid) {
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) return false;
    auto put = [](const char* path, const std::string& text) {
        const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
        ::close(fd);
        return ok;
    };
    put("/proc/self/setgroups", "deny");
    return put("/proc/self/uid_map", std::format("{} {} 1\n", uid, uid))
        && put("/proc/self/gid_map", std::format("{} {} 1\n", gid, gid));
}

// ── Landlock ─────────────────────────────────────────────────────────

namespace landlock {

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

}  // namespace landlock

// ── seccomp ──────────────────────────────────────────────────────────

namespace seccomp {

namespace {

// <linux/filter.h>
constexpr std::uint16_t BPF_LD = 0x00, BPF_JMP = 0x05, BPF_RET = 0x06;
constexpr std::uint16_t BPF_W = 0x00, BPF_ABS = 0x20, BPF_JEQ = 0x10, BPF_K = 0x00;
// <linux/seccomp.h>
constexpr std::uint32_t SECCOMP_RET_ALLOW = 0x7fff0000U;
constexpr std::uint32_t SECCOMP_RET_ERRNO = 0x00050000U;
constexpr std::uint32_t EPERM_ = 1;
// struct seccomp_data { int nr; __u32 arch; __u64 ip; __u64 args[6]; }
constexpr std::uint32_t OFF_NR = 0, OFF_ARCH = 4, OFF_ARG1_LO = 16 + 8;   // little endian
// <linux/audit.h>
constexpr std::uint32_t AUDIT_ARCH_X86_64 = 0xC000003EU;
constexpr std::uint32_t AUDIT_ARCH_I386 = 0x40000003U;
constexpr std::uint32_t AUDIT_ARCH_AARCH64 = 0xC00000B7U;
// ioctl numbers per ABI; x86_64's x32 ABI shares the native one plus a bit.
constexpr std::uint32_t NR_IOCTL_X86_64 = 16, NR_IOCTL_I386 = 54, NR_IOCTL_AARCH64 = 29;
constexpr std::uint32_t X32_BIT = 0x40000000U;
// <asm-generic/ioctls.h>, identical on x86 and arm64
constexpr std::uint32_t kTiocsti = 0x5412, kTioclinux = 0x541C;

struct Insn {
    std::uint16_t code;
    std::uint8_t jt, jf;
    std::uint32_t k;
};

Insn stmt(std::uint16_t code, std::uint32_t k) { return {code, 0, 0, k}; }
Insn jump(std::uint16_t code, std::uint32_t k, std::uint8_t jt, std::uint8_t jf) {
    return {code, jt, jf, k};
}

std::vector<std::uint8_t> encode(const std::vector<Insn>& prog) {
    std::vector<std::uint8_t> out;
    out.reserve(prog.size() * 8);
    for (const auto& i : prog) {
        auto put16 = [&](std::uint16_t v) { out.push_back(v & 0xff); out.push_back(v >> 8); };
        auto put32 = [&](std::uint32_t v) {
            for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
        };
        put16(i.code);
        out.push_back(i.jt);
        out.push_back(i.jf);
        put32(i.k);
    }
    return out;
}

// The tail shared by every ABI: the syscall number is loaded; refuse the two
// commands on ioctl, allow everything else.
//   [0] nr == ioctl ? -> [1] : -> allow
std::vector<Insn> ioctl_check(std::uint32_t nr_ioctl) {
    const std::uint16_t LD_W_ABS = BPF_LD | BPF_W | BPF_ABS;
    const std::uint16_t JEQ_K = BPF_JMP | BPF_JEQ | BPF_K;
    const std::uint16_t RET_K = BPF_RET | BPF_K;
    return {
        jump(JEQ_K, nr_ioctl, 0, 5),                     // not ioctl -> allow
        stmt(LD_W_ABS, OFF_ARG1_LO),
        jump(JEQ_K, kTiocsti, 2, 0),
        jump(JEQ_K, kTioclinux, 1, 0),
        stmt(RET_K, SECCOMP_RET_ALLOW),
        stmt(RET_K, SECCOMP_RET_ERRNO | EPERM_),
        stmt(RET_K, SECCOMP_RET_ALLOW),
    };
}

}  // namespace

std::vector<std::uint8_t> block_terminal_injection() {
    const std::uint16_t LD_W_ABS = BPF_LD | BPF_W | BPF_ABS;
    const std::uint16_t JEQ_K = BPF_JMP | BPF_JEQ | BPF_K;
    std::vector<Insn> p;
#if defined(__x86_64__)
    // arch == x86_64 -> native block; arch == i386 -> compat block; else allow
    //   [0] ld arch
    //   [1] jeq x86_64 -> [3]
    //   [2] jeq i386   -> compat
    p.push_back(stmt(LD_W_ABS, OFF_ARCH));
    auto native = ioctl_check(NR_IOCTL_X86_64);
    auto x32 = ioctl_check(NR_IOCTL_X86_64 | X32_BIT);
    auto compat = ioctl_check(NR_IOCTL_I386);
    // Layout: [ld arch][jeq x86_64][jeq i386][ret allow]
    //         native: [ld nr][jeq x32? -> x32 block][native block...]
    //         x32 block, compat: [ld nr][compat block]
    const std::uint8_t native_len = static_cast<std::uint8_t>(2 + native.size());
    p.push_back(jump(JEQ_K, AUDIT_ARCH_X86_64, 2, 0));
    p.push_back(jump(JEQ_K, AUDIT_ARCH_I386, static_cast<std::uint8_t>(1 + native_len + x32.size()), 0));
    p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    p.push_back(jump(JEQ_K, NR_IOCTL_X86_64 | X32_BIT, static_cast<std::uint8_t>(native.size()), 0));
    p.insert(p.end(), native.begin(), native.end());
    p.insert(p.end(), x32.begin(), x32.end());
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    p.insert(p.end(), compat.begin(), compat.end());
#elif defined(__aarch64__)
    p.push_back(stmt(LD_W_ABS, OFF_ARCH));
    p.push_back(jump(JEQ_K, AUDIT_ARCH_AARCH64, 1, 0));
    p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    auto native = ioctl_check(NR_IOCTL_AARCH64);
    p.insert(p.end(), native.begin(), native.end());
#else
    return {};
#endif
    return encode(p);
}

std::size_t instruction_count(std::span<const std::uint8_t> program) {
    return program.size() / 8;
}

namespace {

// execve / execveat as user notifications.
struct SeccompData { std::int32_t nr; std::uint32_t arch; std::uint64_t ip; std::uint64_t args[6]; };
struct SeccompNotif { std::uint64_t id; std::uint32_t pid; std::uint32_t flags; SeccompData data; };
struct SeccompNotifResp { std::uint64_t id; std::int64_t val; std::int32_t error; std::uint32_t flags; };
constexpr unsigned long kNotifRecv = 0xC0502100UL;      // _IOWR('!', 0, seccomp_notif)
constexpr unsigned long kNotifSend = 0xC0182101UL;      // _IOWR('!', 1, seccomp_notif_resp)
constexpr unsigned long kNotifIdValid = 0x40082102UL;   // _IOW('!', 2, __u64)
constexpr std::uint32_t kRetUserNotif = 0x7fc00000U, kRetAllow = 0x7fff0000U;
constexpr unsigned kSetModeFilter = 1, kFlagNewListener = 1U << 3;
constexpr std::uint32_t kUserNotifContinue = 1;
#if defined(__x86_64__)
constexpr std::uint32_t kArch = 0xC000003EU, kExecve = 59, kExecveat = 322;
#elif defined(__aarch64__)
constexpr std::uint32_t kArch = 0xC00000B7U, kExecve = 221, kExecveat = 281;
#else
constexpr std::uint32_t kArch = 0, kExecve = 0, kExecveat = 0;
#endif

// A NUL-terminated string from another process's memory.
std::string read_remote_string(pid_t pid, std::uint64_t addr) {
    std::string out;
    char buf[256];
    for (int chunk = 0; chunk < 16 && addr; ++chunk) {
        iovec local{buf, sizeof(buf)};
        iovec remote{reinterpret_cast<void*>(addr + chunk * sizeof(buf)), sizeof(buf)};
        const auto n = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0) break;
        const auto len = ::strnlen(buf, static_cast<std::size_t>(n));
        out.append(buf, len);
        if (len < static_cast<std::size_t>(n)) break;
    }
    return out;
}

}  // namespace

int exec_listener() {
    if (kArch == 0) return -1;
    Insn prog[] = {
        {0x20, 0, 0, 4},                    // ld [arch]
        {0x15, 1, 0, kArch},                // jeq arch, +1
        {0x06, 0, 0, kRetAllow},            // foreign ABI: allow
        {0x20, 0, 0, 0},                    // ld [nr]
        {0x15, 2, 0, kExecve},              // jeq execve -> notify
        {0x15, 1, 0, kExecveat},            // jeq execveat -> notify
        {0x06, 0, 0, kRetAllow},
        {0x06, 0, 0, kRetUserNotif},
    };
    struct Prog { unsigned short len; Insn* filter; } p{
        static_cast<unsigned short>(sizeof(prog) / sizeof(prog[0])), prog };
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    const long fd = ::syscall(SYS_seccomp, kSetModeFilter, kFlagNewListener, &p);
    return fd < 0 ? -1 : static_cast<int>(fd);
}

std::optional<ExecNotice> next_exec(int listener) {
    SeccompNotif req{};
    if (::ioctl(listener, kNotifRecv, &req) != 0) return std::nullopt;
    const auto addr = req.data.nr == static_cast<std::int32_t>(kExecveat) ? req.data.args[1] : req.data.args[0];
    auto path = read_remote_string(static_cast<pid_t>(req.pid), addr);
    // The read is only trusted while the request is still live.
    if (::ioctl(listener, kNotifIdValid, &req.id) != 0) path.clear();
    SeccompNotifResp resp{ .id = req.id, .val = 0, .error = 0, .flags = kUserNotifContinue };
    (void)::ioctl(listener, kNotifSend, &resp);
    return ExecNotice{ static_cast<int>(req.pid), std::move(path) };
}

}  // namespace seccomp

// ── the filesystem, beneath a root ───────────────────────────────────

namespace {

struct Fd {
    int fd { -1 };
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(Fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
    Fd& operator=(Fd&& o) noexcept { if (this != &o) { reset(); fd = std::exchange(o.fd, -1); } return *this; }
    ~Fd() { reset(); }
    void reset() { if (fd >= 0) ::close(fd); fd = -1; }
    explicit operator bool() const { return fd >= 0; }
};

std::string err(std::string_view what, const fs::path& p) {
    return std::format("{} {}: {}", what, p.string(), std::strerror(errno));
}

// openat2(2) RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS; on a kernel without it
// (before 5.6), the same walk by hand, one component at a time.
Fd open_beneath(int root, const fs::path& rel, int flags, unsigned mode = 0) {
    struct OpenHow { std::uint64_t flags, mode, resolve; };
    constexpr long kOpenat2 = 437;
    constexpr std::uint64_t kNoSymlinks = 0x04, kBeneath = 0x08;
    const auto r = rel.relative_path().lexically_normal();
    const std::string path = r.empty() || r == "." ? std::string(".") : r.generic_string();
    OpenHow how{ static_cast<std::uint64_t>(flags | O_CLOEXEC), mode, kNoSymlinks | kBeneath };
    const long fd = ::syscall(kOpenat2, root, path.c_str(), &how, sizeof how);
    if (fd >= 0 || errno != ENOSYS) return Fd(static_cast<int>(fd));

    Fd cur(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    std::vector<std::string> parts;
    for (auto& c : r) if (c != "." && !c.empty()) parts.push_back(c.string());
    if (parts.empty()) return Fd(::openat(cur.fd, ".", flags | O_CLOEXEC | O_NOFOLLOW, mode));
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "..") { errno = EXDEV; return {}; }
        Fd next(::openat(cur.fd, parts[i].c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) return {};
        cur = std::move(next);
    }
    if (parts.back() == "..") { errno = EXDEV; return {}; }
    return Fd(::openat(cur.fd, parts.back().c_str(), flags | O_CLOEXEC | O_NOFOLLOW, mode));
}

// Every directory of `rel` beneath `root`, made where missing.
std::expected<void, std::string> mkdirs_beneath(int root, const fs::path& rel) {
    fs::path sofar;
    for (auto& c : rel.relative_path().lexically_normal()) {
        if (c == "." || c.empty()) continue;
        Fd parent = open_beneath(root, sofar, O_PATH | O_DIRECTORY);
        if (!parent) return std::unexpected(err("cannot open", sofar));
        if (::mkdirat(parent.fd, c.c_str(), 0755) != 0 && errno != EEXIST)
            return std::unexpected(err("cannot create", sofar / c));
        sofar /= c;
        if (!open_beneath(root, sofar, O_PATH | O_DIRECTORY))
            return std::unexpected(std::format("{} is not a directory of the instance's own (a link?)",
                                               sofar.string()));
    }
    return {};
}

std::expected<void, std::string> write_all(int to, int from, const fs::path& what) {
    std::array<char, 1 << 16> buf;
    for (;;) {
        const auto n = ::read(from, buf.data(), buf.size());
        if (n == 0) return {};
        if (n < 0) { if (errno == EINTR) continue; return std::unexpected(err("cannot read", what)); }
        for (ssize_t off = 0; off < n;) {
            const auto w = ::write(to, buf.data() + off, static_cast<std::size_t>(n - off));
            if (w < 0) { if (errno == EINTR) continue; return std::unexpected(err("cannot write", what)); }
            off += w;
        }
    }
}

std::expected<void, std::string> file_into(const fs::path& src, int root, const fs::path& rel) {
    if (auto m = mkdirs_beneath(root, rel.parent_path()); !m) return m;
    std::error_code ec;
    const auto st = fs::symlink_status(src, ec);
    Fd parent = open_beneath(root, rel.parent_path(), O_PATH | O_DIRECTORY);
    if (!parent) return std::unexpected(err("cannot open", rel.parent_path()));
    const auto name = rel.filename();
    // What is there now goes, unless it is a directory: a link the instance
    // made is replaced, never written through.
    struct stat cur{};
    if (::fstatat(parent.fd, name.c_str(), &cur, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(cur.st_mode))
        (void)::unlinkat(parent.fd, name.c_str(), 0);
    if (fs::is_symlink(st)) {
        const auto target = fs::read_symlink(src, ec);
        if (ec || ::symlinkat(target.c_str(), parent.fd, name.c_str()) != 0)
            return std::unexpected(err("cannot link", rel));
        return {};
    }
    Fd in(::open(src.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in) return std::unexpected(err("cannot read", src));
    struct stat sst{};
    ::fstat(in.fd, &sst);
    Fd out(::openat(parent.fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    sst.st_mode & 0777));
    if (!out) return std::unexpected(err("cannot write", rel));
    return write_all(out.fd, in.fd, rel);
}

std::expected<void, std::string> file_out(int dir, const char* name, const fs::path& dst) {
    struct stat st{};
    if (::fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return std::unexpected(err("cannot read", name));
    std::error_code ec;
    fs::remove(dst, ec);
    if (S_ISLNK(st.st_mode)) {
        std::array<char, 4096> buf{};
        const auto n = ::readlinkat(dir, name, buf.data(), buf.size() - 1);
        if (n < 0) return std::unexpected(err("cannot read", name));
        fs::create_symlink(std::string(buf.data(), static_cast<std::size_t>(n)), dst, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", dst.string(), ec.message()));
        return {};
    }
    if (!S_ISREG(st.st_mode)) return {};    // devices, fifos, sockets: not copied
    Fd in(::openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!in) return std::unexpected(err("cannot read", name));
    Fd out(::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777));
    if (!out) return std::unexpected(err("cannot write", dst));
    return write_all(out.fd, in.fd, dst);
}

std::expected<void, std::string> dir_out(int dir, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", dst.string(), ec.message()));
    Fd copy(::fcntl(dir, F_DUPFD_CLOEXEC, 0));
    Fd listed(::openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    DIR* d = listed ? ::fdopendir(listed.fd) : nullptr;
    if (!d) return std::unexpected(err("cannot list", dst));
    listed.fd = -1;   // owned by d now
    std::vector<std::string> names;
    while (auto* e = ::readdir(d))
        if (std::string_view n = e->d_name; n != "." && n != "..") names.emplace_back(n);
    ::closedir(d);
    for (auto& n : names) {
        struct stat st{};
        if (::fstatat(copy.fd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            Fd sub(::openat(copy.fd, n.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!sub) return std::unexpected(err("cannot open", n));
            if (auto r = dir_out(sub.fd, dst / n); !r) return r;
        } else if (auto r = file_out(copy.fd, n.c_str(), dst / n); !r) {
            return r;
        }
    }
    return {};
}

}  // namespace

std::expected<void, std::string> copy_into_beneath(const fs::path& src, const fs::path& root, const fs::path& rel) {
    Fd rootfd(::open(root.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!rootfd) return std::unexpected(err("cannot open", root));
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(src, ec))) {
        // Onto a directory of the instance's own, or a path that ends in
        // `/`: into it, as cp does.
        if (!rel.has_filename()) {
            if (auto m = mkdirs_beneath(rootfd.fd, rel); !m) return m;
            return file_into(src, rootfd.fd, rel.parent_path() / src.filename());
        }
        if (open_beneath(rootfd.fd, rel, O_PATH | O_DIRECTORY))
            return file_into(src, rootfd.fd, rel / src.filename());
        return file_into(src, rootfd.fd, rel);
    }
    if (auto m = mkdirs_beneath(rootfd.fd, rel); !m) return m;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != std::default_sentinel;
         it.increment(ec)) {
        const auto sub = rel / fs::relative(it->path(), src, ec);
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            if (auto m = mkdirs_beneath(rootfd.fd, sub); !m) return m;
        } else if (auto r = file_into(it->path(), rootfd.fd, sub); !r) {
            return r;
        }
    }
    if (ec) return std::unexpected(std::format("cannot read {}: {}", src.string(), ec.message()));
    return {};
}

std::expected<void, std::string> copy_out_of_beneath(const fs::path& root, const fs::path& rel, const fs::path& dst) {
    Fd rootfd(::open(root.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!rootfd) return std::unexpected(err("cannot open", root));
    Fd parent = open_beneath(rootfd.fd, rel.parent_path(), O_PATH | O_DIRECTORY);
    if (!parent) return std::unexpected(err("cannot open", rel.parent_path()));
    const auto name = rel.filename();
    struct stat st{};
    if (::fstatat(parent.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return std::unexpected(err("not found:", rel));
    if (S_ISDIR(st.st_mode)) {
        Fd dir(::openat(parent.fd, name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!dir) return std::unexpected(err("cannot open", rel));
        return dir_out(dir.fd, dst);
    }
    return file_out(parent.fd, name.c_str(), dst);
}

// ── facts ────────────────────────────────────────────────────────────

std::optional<FileOwnership> file_ownership(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    return FileOwnership{ static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_mode) };
}

std::string kernel_release() {
    struct utsname u{};
    return ::uname(&u) == 0 ? std::string(u.release) : std::string{};
}

bool mount_kernel_fs(std::string_view type, const fs::path& target) {
    std::error_code ec;
    fs::create_directories(target, ec);
    const std::string t(type);
    unsigned long flags = MS_NOSUID;
    if (t != "devtmpfs") flags |= MS_NODEV;
    if (t == "proc" || t == "sysfs") flags |= MS_NOEXEC;
    if (::mount(t.c_str(), target.c_str(), t.c_str(), flags, t == "tmpfs" ? "mode=0755" : nullptr) == 0)
        return true;
    return errno == EBUSY;
}

bool remount_root_rw() {
    return ::mount("", "/", "", MS_REMOUNT, nullptr) == 0;
}

bool is_pid1() { return ::getpid() == 1; }

#else

bool enter_private_network(unsigned, unsigned) { return false; }

namespace landlock {
int abi() { return 0; }
std::expected<void, std::string> restrict_writes(std::span<const fs::path>) {
    return std::unexpected("Landlock is Linux-only");
}
}  // namespace landlock

namespace seccomp {
std::vector<std::uint8_t> block_terminal_injection() { return {}; }
std::size_t instruction_count(std::span<const std::uint8_t> program) { return program.size() / 8; }
int exec_listener() { return -1; }
std::optional<ExecNotice> next_exec(int) { return std::nullopt; }
}  // namespace seccomp

std::expected<void, std::string> copy_into_beneath(const fs::path& src, const fs::path& root, const fs::path& rel) {
    std::error_code ec;
    auto to = root / rel;
    if (fs::is_directory(to, ec) && !fs::is_directory(src, ec)) to /= src.filename();
    fs::create_directories(to.parent_path(), ec);
    fs::copy(src, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("copy failed: " + ec.message());
    return {};
}

std::expected<void, std::string> copy_out_of_beneath(const fs::path& root, const fs::path& rel, const fs::path& dst) {
    std::error_code ec;
    fs::copy(root / rel, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing
                                  | fs::copy_options::copy_symlinks, ec);
    if (ec) return std::unexpected("copy failed: " + ec.message());
    return {};
}

std::optional<FileOwnership> file_ownership(const fs::path& path) {
#if defined(__APPLE__)
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    return FileOwnership{ static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_mode) };
#else
    (void)path;
    return std::nullopt;
#endif
}

bool mount_kernel_fs(std::string_view, const fs::path&) { return false; }
bool remount_root_rw() { return false; }
bool is_pid1() { return false; }

std::string kernel_release() {
#if defined(__APPLE__)
    struct utsname u{};
    return ::uname(&u) == 0 ? std::string(u.release) : std::string{};
#else
    return {};
#endif
}

#endif

}  // namespace xlings::platform
