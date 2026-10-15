module;
#if defined(__APPLE__)
#include <cerrno>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
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

}  // namespace xlings::platform_impl
#endif
