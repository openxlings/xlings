module;
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif
module xlings.platform;
import :fs_caps;
import std;

// Linux's side of fs_caps (fs/fs_caps.cppm).
#if defined(__linux__)
namespace xlings::platform_impl {

int rename_no_replace_native(const std::filesystem::path& from, const std::filesystem::path& to) {
    constexpr unsigned NO_REPLACE = 1;   // renameat2 RENAME_NOREPLACE
    return ::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), NO_REPLACE) == 0 ? 0 : errno;
}

int exchange_native(const std::filesystem::path& a, const std::filesystem::path& b) {
    constexpr unsigned EXCHANGE = 2;     // renameat2 RENAME_EXCHANGE
    return ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), EXCHANGE) == 0 ? 0 : errno;
}

bool unsupported(int error) { return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP; }

std::uint64_t device_of(const std::filesystem::path& dir) {
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
}

// The mount table's name first (fs_caps.cppm: statfs over 9p names the far
// side's filesystem); statfs's magic number when there is no mount table.
std::string filesystem_type(const std::filesystem::path& dir) {
    struct stat at {};
    if (::stat(dir.c_str(), &at) == 0) {
        std::ifstream in("/proc/self/mountinfo");
        const std::string table{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto type = xlings::platform::mount_table_type(table, major(at.st_dev), minor(at.st_dev));
        if (!type.empty()) return type;
    }
    struct statfs st {};
    if (::statfs(dir.c_str(), &st) != 0) return {};
    switch (static_cast<unsigned long>(st.f_type)) {
        case 0xEF53: return "ext4";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        case 0x01021994: return "tmpfs";
        case 0x794C7630: return "overlay";
        case 0x6969: return "nfs";
        case 0xFF534D42: return "cifs";
        case 0xFE534D42: return "smb2";
        case 0x4D44: return "vfat";
        case 0x2011BAB0: return "exfat";
        case 0x7366746E: return "ntfs3";
        case 0x5346544E: return "ntfs";
        case 0x65735546: return "fuse";
        case 0x01021997: return "9p";
        case 0x6A656A63: return "virtiofs";
        case 0x2FC12FC1: return "zfs";
        case 0x00C36400: return "ceph";
        default: return std::format("0x{:x}", static_cast<unsigned long>(st.f_type));
    }
}

bool remote(const std::filesystem::path&, std::string_view type) { return xlings::platform::remote_type(type); }

bool owner_only_dir(const std::filesystem::path& dir) {
    struct stat st {};
    return ::lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0;
}

std::vector<std::filesystem::path> runtime_dirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) dirs.emplace_back(runtime);
    dirs.emplace_back(std::format("/run/user/{}", ::getuid()));
    std::error_code ec;
    dirs.push_back(std::filesystem::temp_directory_path(ec) / std::format("xlings-{}", ::getuid()));
    return dirs;
}

}  // namespace xlings::platform_impl
#endif
