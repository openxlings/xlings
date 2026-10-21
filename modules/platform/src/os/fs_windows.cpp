module;
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
module xlings.platform;
import :fs_caps;
import std;

// Windows's side of fs_caps (fs/fs_caps.cppm).
#if defined(_WIN32)
namespace xlings::platform_impl {

// MoveFileExW without MOVEFILE_REPLACE_EXISTING refuses an existing name on
// every Windows filesystem (NTFS, ReFS, FAT, SMB).
int rename_no_replace_native(const std::filesystem::path& from, const std::filesystem::path& to) {
    return ::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_WRITE_THROUGH)
        ? 0 : static_cast<int>(::GetLastError());
}

// Windows has no atomic exchange of two names.
int exchange_native(const std::filesystem::path&, const std::filesystem::path&) { return ERROR_NOT_SUPPORTED; }

bool unsupported(int error) { return error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION; }

// One cache entry per volume: its serial number.
std::uint64_t device_of(const std::filesystem::path& dir) {
    wchar_t root[MAX_PATH] {};
    DWORD serial = 0;
    if (!::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH)) return 0;
    if (!::GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) return 0;
    return serial;
}

std::string filesystem_type(const std::filesystem::path& dir) {
    wchar_t root[MAX_PATH] {}, name[MAX_PATH] {};
    if (!::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH)) return {};
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH)) return {};
    return std::filesystem::path(name).string();
}

bool remote(const std::filesystem::path& dir, std::string_view) {
    wchar_t root[MAX_PATH] {};
    return ::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH) && ::GetDriveTypeW(root) == DRIVE_REMOTE;
}

// The user's temporary directory is the user's own (its ACL).
bool owner_only_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) && !std::filesystem::is_symlink(dir, ec);
}

std::vector<std::filesystem::path> runtime_dirs() {
    std::error_code ec;
    return {std::filesystem::temp_directory_path(ec) / "xlings"};
}

}  // namespace xlings::platform_impl
#endif
