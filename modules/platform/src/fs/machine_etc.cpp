module;
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#endif
module xlings.platform;
import std;
import :machine_etc;

namespace xlings::platform::machine_etc {
namespace {
namespace fs = std::filesystem;
#if !defined(_WIN32)
struct Fd {
    int value { -1 };
    explicit Fd(int fd) : value{fd} {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { if (value >= 0) ::close(value); }
    int release() { return std::exchange(value, -1); }
};
std::string failure(std::string_view action, const fs::path& path) {
    return std::format("{} {}: {} (machine /etc paths must not traverse symlinks)",
                       action, path.string(), std::error_code(errno, std::generic_category()).message());
}
std::expected<std::vector<std::string>, std::string> components(const fs::path& path) {
    if (path.is_absolute()) return std::unexpected("machine /etc entry must be relative: " + path.string());
    std::vector<std::string> out;
    for (const auto& part : path) {
        const auto name = part.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") return std::unexpected("machine /etc entry cannot contain '..': " + path.string());
        out.push_back(name);
    }
    return out;
}
std::expected<int, std::string> walk(int root, const fs::path& path, bool create,
                                    std::optional<unsigned> mode = {}) {
    auto parts = components(path);
    if (!parts) return std::unexpected(parts.error());
    Fd current{::fcntl(root, F_DUPFD_CLOEXEC, 0)};
    if (current.value < 0) return std::unexpected(failure("cannot duplicate directory", path));
    for (std::size_t index { 0 }; index < parts->size(); ++index) {
        const auto& name = (*parts)[index];
        bool created { false };
        int next = ::openat(current.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(current.value, name.c_str(), 0755) == 0) created = true;
            else if (errno != EEXIST) return std::unexpected(failure("cannot create directory", path));
            next = ::openat(current.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) return std::unexpected(failure("cannot open directory", path));
        Fd opened{next};
        if (created && mode && index + 1 == parts->size() && ::fchmod(next, static_cast<mode_t>(*mode)) < 0)
            return std::unexpected(failure("cannot set directory mode", path));
        ::close(current.value);
        current.value = opened.release();
    }
    return current.release();
}
std::expected<int, std::string> parent(int root, const fs::path& relative, bool create) {
    auto parts = components(relative);
    if (!parts) return std::unexpected(parts.error());
    if (parts->empty()) return std::unexpected("machine /etc file name is empty");
    return walk(root, relative.parent_path(), create);
}
std::expected<std::string, std::string> read_bytes(int fd, const fs::path& path, bool singleLink) {
    struct stat status {};
    if (::fstat(fd, &status) < 0) return std::unexpected(failure("cannot inspect file", path));
    if (!S_ISREG(status.st_mode) || (singleLink && status.st_nlink != 1))
        return std::unexpected("machine /etc file must be a regular, unshared file: " + path.string());
    if (::lseek(fd, 0, SEEK_SET) < 0) return std::unexpected(failure("cannot seek file", path));
    std::string out;
    std::array<char, 4096> buffer;
    for (;;) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::unexpected(failure("cannot read file", path));
        if (count == 0) break;
        out.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return out;
}
std::expected<void, std::string> write_bytes(int fd, std::string_view bytes, const fs::path& path) {
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::unexpected(failure("cannot write file", path));
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    if (::fsync(fd) < 0) return std::unexpected(failure("cannot sync file", path));
    return {};
}
#endif
}

Directory::Directory(int descriptor) : descriptor_{descriptor} {}
Directory::Directory(Directory&& other) noexcept : descriptor_{std::exchange(other.descriptor_, -1)} {}
Directory& Directory::operator=(Directory&& other) noexcept {
    if (this != &other) {
#if !defined(_WIN32)
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}
Directory::~Directory() {
#if !defined(_WIN32)
    if (descriptor_ >= 0) ::close(descriptor_);
#endif
}
std::expected<Directory, std::string> Directory::open(const fs::path& root, bool create) {
#if !defined(_WIN32)
    std::error_code ec;
    auto absolute = fs::absolute(root, ec);
    if (ec) return std::unexpected("cannot resolve machine directory: " + ec.message());
#if defined(__APPLE__)
    // These fixed OS aliases precede the caller-owned tree, never a user alias.
    if (absolute == "/tmp" || absolute.string().starts_with("/tmp/")) absolute = "/private" / absolute.relative_path();
    if (absolute == "/var" || absolute.string().starts_with("/var/")) absolute = "/private" / absolute.relative_path();
#endif
    Fd anchor{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (anchor.value < 0) return std::unexpected(failure("cannot open filesystem root", root));
    auto fd = walk(anchor.value, absolute.relative_path(), create);
    if (!fd) return std::unexpected(fd.error());
    return Directory{*fd};
#else
    return std::unexpected("anchored machine /etc writes are unsupported on Windows");
#endif
}
std::expected<bool, std::string> Directory::ensure_directory(const fs::path& relative,
                                                            std::optional<unsigned> mode) {
#if !defined(_WIN32)
    auto parts = components(relative);
    if (!parts) return std::unexpected(parts.error());
    if (parts->empty()) return true;
    auto fd = parent(descriptor_, relative, true);
    if (!fd) return std::unexpected(fd.error());
    Fd holder{*fd};
    struct stat status {};
    const auto name = relative.filename().string();
    if (::fstatat(holder.value, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISREG(status.st_mode)) return false;
        if (!S_ISDIR(status.st_mode)) return std::unexpected("machine directory is not a real directory: " + relative.string());
    } else if (errno != ENOENT) return std::unexpected(failure("cannot inspect directory", relative));
    auto opened = walk(descriptor_, relative, true, mode);
    if (!opened) return std::unexpected(opened.error());
    Fd child{*opened};
    return true;
#else
    return std::unexpected("anchored machine /etc writes are unsupported on Windows");
#endif
}
std::expected<bool, std::string> Directory::link_missing(const fs::path& relative, const fs::path& target) {
#if !defined(_WIN32)
    auto fd = parent(descriptor_, relative, true);
    if (!fd) return std::unexpected(fd.error());
    Fd holder{*fd};
    if (::symlinkat(target.c_str(), holder.value, relative.filename().c_str()) == 0) return true;
    if (errno == EEXIST) return false;
    return std::unexpected(failure("cannot create factory link", relative));
#else
    return std::unexpected("anchored machine /etc writes are unsupported on Windows");
#endif
}
std::expected<bool, std::string> Directory::write_missing(const fs::path& relative, std::string_view bytes) {
#if !defined(_WIN32)
    auto fd = parent(descriptor_, relative, true);
    if (!fd) return std::unexpected(fd.error());
    Fd holder{*fd};
    Fd file{::openat(holder.value, relative.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644)};
    if (file.value < 0) {
        if (errno == EEXIST) return false;
        return std::unexpected(failure("cannot create machine file", relative));
    }
    auto written = write_bytes(file.value, bytes, relative);
    if (!written) return std::unexpected(written.error());
    return true;
#else
    return std::unexpected("anchored machine /etc writes are unsupported on Windows");
#endif
}
std::expected<std::optional<std::string>, std::string>
Directory::read_regular(const fs::path& relative, bool singleLink) const {
#if !defined(_WIN32)
    auto fd = parent(descriptor_, relative, false);
    if (!fd) return std::unexpected(fd.error());
    Fd holder{*fd};
    Fd file{::openat(holder.value, relative.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    if (file.value < 0) {
        if (errno == ENOENT) return std::optional<std::string>{};
        return std::unexpected(failure("cannot open machine file", relative));
    }
    auto bytes = read_bytes(file.value, relative, singleLink);
    if (!bytes) return std::unexpected(bytes.error());
    return std::optional<std::string>{std::move(*bytes)};
#else
    return std::unexpected("anchored machine /etc reads are unsupported on Windows");
#endif
}
std::expected<std::vector<std::string>, std::string>
Directory::append_named(const fs::path& relative, std::span<const std::pair<std::string, std::string>> rows) {
#if !defined(_WIN32)
    auto fd = parent(descriptor_, relative, false);
    if (!fd) return std::unexpected(fd.error());
    Fd holder{*fd};
    Fd file{::openat(holder.value, relative.filename().c_str(), O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644)};
    if (file.value < 0) return std::unexpected(failure("cannot open machine database", relative));
    if (::flock(file.value, LOCK_EX | LOCK_NB) < 0) return std::unexpected(failure("cannot lock machine database", relative));
    auto bytes = read_bytes(file.value, relative, true);
    if (!bytes) return std::unexpected(bytes.error());
    std::set<std::string> names;
    std::istringstream stream{*bytes};
    for (std::string line; std::getline(stream, line);)
        if (const auto colon = line.find(':'); colon != std::string::npos && colon > 0) names.insert(line.substr(0, colon));
    std::vector<std::string> added;
    std::string extra;
    for (const auto& [name, text] : rows) {
        if (!names.insert(name).second) continue;
        if (extra.empty() && !bytes->empty() && bytes->back() != '\n') extra += '\n';
        extra += text;
        added.push_back(name);
    }
    if (!extra.empty()) {
        auto written = write_bytes(file.value, extra, relative);
        if (!written) return std::unexpected(written.error());
    }
    return added;
#else
    return std::unexpected("anchored machine /etc writes are unsupported on Windows");
#endif
}
}
