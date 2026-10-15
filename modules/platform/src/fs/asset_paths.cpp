module;
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <stdio.h>
#include <sys/clonefile.h>
#endif
module xlings.platform;
import :asset_paths;
import :fs_caps;
import std;
namespace xlings::platform {
std::expected<bool, std::string> exchange_paths(const std::filesystem::path& first,
                                                const std::filesystem::path& second) {
    // Only where the filesystem has it (fs_caps); false: the caller's
    // non-overwriting guarded moves.
    if (!fs_caps(second.parent_path().empty() ? std::filesystem::path(".") : second.parent_path()).rename_exchange)
        return false;
    const int error = platform_impl::exchange_native(first, second);
    if (error == 0) return true;
    if (platform_impl::unsupported(error)) return false;
    return std::unexpected("cannot exchange " + first.string() + " with " + second.string() + ": " +
        std::error_code(error, is_windows ? std::system_category() : std::generic_category()).message());
}

namespace {

// One regular file: shared blocks where the filesystem can, else a copy.
bool clone_file_(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__linux__)
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) { ::close(in); return false; }
    const bool cloned = ::ioctl(out, FICLONE, in) == 0;
    ::close(out);
    ::close(in);
    if (!cloned) { std::error_code ec; std::filesystem::remove(to, ec); }
    return cloned;
#elif defined(__APPLE__)
    return ::clonefile(from.c_str(), to.c_str(), CLONE_NOFOLLOW) == 0;
#else
    (void)from; (void)to;
    return false;
#endif
}

}  // namespace

std::expected<std::size_t, std::string> copy_tree(const std::filesystem::path& from,
                                                  const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return std::unexpected(from.string() + " is not a directory");
    fs::create_directories(to, ec);
    if (ec) return std::unexpected("cannot create " + to.string() + ": " + ec.message());
    std::size_t cloned = 0;
    // A directory's mode is applied after its contents are in (a read-only
    // one would refuse them).
    std::vector<std::pair<fs::path, fs::perms>> dirs;
    auto fail = [](const fs::path& p, const std::error_code& e) {
        return std::unexpected("cannot copy " + p.string() + ": " + e.message());
    };
    // Sentinel iteration: a range-for over a directory iterator through a
    // BMI fails to link on the musl cross toolchain (see caps.cpp).
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::none, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        const auto& entry = *it;
        // Lexically: fs::relative resolves links, and a link is copied as one.
        const auto target = to / entry.path().lexically_relative(from);
        const auto status = entry.symlink_status(ec);
        if (ec) return fail(entry.path(), ec);
        if (fs::is_symlink(status)) {
            if (fs::exists(fs::symlink_status(target, ec)) || fs::is_symlink(fs::symlink_status(target, ec)))
                fs::remove(target, ec);
            fs::copy_symlink(entry.path(), target, ec);
            if (ec) return fail(entry.path(), ec);
            continue;
        }
        if (fs::is_directory(status)) {
            fs::create_directories(target, ec);
            if (ec) return fail(entry.path(), ec);
            dirs.emplace_back(target, status.permissions());
            continue;
        }
        if (!fs::is_regular_file(status)) continue;   // a socket or a device is not content
        if (fs::exists(fs::symlink_status(target, ec))) fs::remove(target, ec);
        if (clone_file_(entry.path(), target)) {
            ++cloned;
        } else if (!fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec) || ec) {
            return fail(entry.path(), ec);
        }
        fs::permissions(target, status.permissions(), ec);
        if (auto t = fs::last_write_time(entry.path(), ec); !ec) fs::last_write_time(target, t, ec);
    }
    if (ec) return std::unexpected("cannot read " + from.string() + ": " + ec.message());
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) fs::permissions(d->first, d->second, ec);
    return cloned;
}
}
