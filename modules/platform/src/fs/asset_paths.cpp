module;
#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <stdio.h>
#endif
module xlings.platform;
import :asset_paths;
import std;
namespace xlings::platform {
std::expected<bool, std::string> exchange_paths(const std::filesystem::path& first,
                                                const std::filesystem::path& second) {
#if defined(__linux__)
    constexpr unsigned EXCHANGE { 2 };
    if (::syscall(SYS_renameat2, AT_FDCWD, first.c_str(), AT_FDCWD, second.c_str(), EXCHANGE) == 0) return true;
#elif defined(__APPLE__)
    if (::renamex_np(first.c_str(), second.c_str(), RENAME_SWAP) == 0) return true;
#else
    return false;
#endif
#if defined(__linux__) || defined(__APPLE__)
    if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) return false;
    return std::unexpected("cannot exchange " + first.string() + " with " + second.string() + ": " +
        std::error_code(errno, std::generic_category()).message());
#endif
}
}
