module;
#if defined(__APPLE__)
#include <cerrno>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
module xlings.platform;
import :fs_caps;
import std;

// macOS's side of fs_caps (fs/fs_caps.cppm).
#if defined(__APPLE__)
namespace xlings::platform_impl {

int rename_no_replace_native(const std::filesystem::path& from, const std::filesystem::path& to) {
    return ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0 ? 0 : errno;
}

int exchange_native(const std::filesystem::path& a, const std::filesystem::path& b) {
    return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0 ? 0 : errno;
}

bool unsupported(int error) { return error == EINVAL || error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS; }

std::uint64_t device_of(const std::filesystem::path& dir) {
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
}

std::string filesystem_type(const std::filesystem::path& dir) {
    struct statfs st {};
    return ::statfs(dir.c_str(), &st) == 0 ? std::string(st.f_fstypename) : std::string{};
}

bool remote(const std::filesystem::path& dir, std::string_view type) {
    struct statfs st {};
    return xlings::platform::remote_type(type) || (::statfs(dir.c_str(), &st) == 0 && (st.f_flags & MNT_LOCAL) == 0);
}

bool owner_only_dir(const std::filesystem::path& dir) {
    struct stat st {};
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0;
}

// macOS has no runtime directory of a user's; its temporary one is the user's own.
std::vector<std::filesystem::path> runtime_dirs() {
    std::error_code ec;
    return {std::filesystem::temp_directory_path(ec) / std::format("xlings-{}", ::getuid())};
}

}  // namespace xlings::platform_impl
#endif
