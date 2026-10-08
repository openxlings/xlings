module;
#if !defined(_WIN32)
#include <sys/statvfs.h>
#include <cerrno>
#include <cstring>
#endif
module xlings.platform;
import :domain_mount;
import std;
namespace xlings::platform {
std::expected<bool, std::string> read_only_mount(const std::filesystem::path& path) {
#if !defined(_WIN32)
    struct statvfs status {};
    if (::statvfs(path.c_str(), &status) != 0)
        return std::unexpected(path.string() + ": cannot observe mount flags: " + std::strerror(errno));
    return (status.f_flag & ST_RDONLY) != 0;
#else
    return std::unexpected(path.string() + ": read-only namespace mount proof is unavailable on Windows");
#endif
}
}
