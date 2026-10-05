module;

#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

export module xlings.subos.copy;

import std;

// `subos cp` (design §12.1): the owner copies into or out of an instance's
// tree -- a tree the instance writes. A link it planted there must not
// redirect the owner's copy: `home/user/x -> ~/.bashrc` would turn
// "copy a file in" into "overwrite the owner's .bashrc" (the shape of
// docker's CVE-2018-15664). So on Linux every path inside the instance is
// resolved beneath its root without following a link at any level, and a
// link found inside is copied as a link, never through.
//
// Elsewhere the sandbox redirects the home directory only and runs with the
// user's full access, so following a link there gives it nothing it lacks;
// the copy is std::filesystem's.
export namespace xlings::subos::copy {

namespace fs = std::filesystem;

// host `src` -> `root` / `rel` (a file, or a directory's contents).
std::expected<void, std::string> into(const fs::path& src, const fs::path& root, const fs::path& rel);

// `root` / `rel` -> host `dst`.
std::expected<void, std::string> out_of(const fs::path& root, const fs::path& rel, const fs::path& dst);

}  // namespace xlings::subos::copy

namespace xlings::subos::copy {

#if defined(__linux__)
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

std::expected<void, std::string> into(const fs::path& src, const fs::path& root, const fs::path& rel) {
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

std::expected<void, std::string> out_of(const fs::path& root, const fs::path& rel, const fs::path& dst) {
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

#else

std::expected<void, std::string> into(const fs::path& src, const fs::path& root, const fs::path& rel) {
    std::error_code ec;
    auto to = root / rel;
    if (fs::is_directory(to, ec) && !fs::is_directory(src, ec)) to /= src.filename();
    fs::create_directories(to.parent_path(), ec);
    fs::copy(src, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("copy failed: " + ec.message());
    return {};
}

std::expected<void, std::string> out_of(const fs::path& root, const fs::path& rel, const fs::path& dst) {
    std::error_code ec;
    fs::copy(root / rel, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing
                                  | fs::copy_options::copy_symlinks, ec);
    if (ec) return std::unexpected("copy failed: " + ec.message());
    return {};
}

#endif

}  // namespace xlings::subos::copy
