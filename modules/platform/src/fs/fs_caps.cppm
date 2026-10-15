export module xlings.platform:fs_caps;
import std;
export namespace xlings::platform {

// What a filesystem does, measured where it is, not assumed from the platform
// (design 2026-10-15-filesystem-capabilities). A home lands on whatever the
// person has: ext4, APFS, NTFS -- and NFS, SMB, FAT, a FUSE mount, WSL's drvfs.
// Every primitive that depends on one of these asks this, once per filesystem,
// and takes the strategy that filesystem allows; none guesses from an errno.
struct FsCaps {
    std::string type;                 // "ext4", "nfs", "apfs", "NTFS", ... ("" unknown)
    bool rename_no_replace { true };  // a rename that refuses an existing name, atomically
    bool rename_exchange { true };    // two entries swapped atomically
    bool hard_links { true };
    bool symlinks { true };
    bool case_sensitive { true };
    bool measured { false };          // false: the probe could not run here; the platform's native answers
};

// The filesystem holding `dir` (an existing directory), measured once per
// filesystem per process: a scratch entry in `dir`, removed after.
FsCaps fs_caps(const std::filesystem::path& dir);

// Measured now, not cached (tests, `self doctor`).
FsCaps probe_fs_caps(const std::filesystem::path& dir);

// Publish `from` (a scratch on the same filesystem) under `to`, never over an
// existing entry, by what `caps` says the filesystem does:
//   a native rename that refuses replacement;
//   else a hard link for a file or a symbolic link (refused as atomically when
//   the name exists), the scratch name then removed;
//   else, for a directory, a rename onto a name that is not there -- the one
//   window, an empty directory made at that name in between, is the caller's
//   own name under its lock.
std::expected<void, std::string> publish_no_replace(const std::filesystem::path& from,
                                                    const std::filesystem::path& to, const FsCaps& caps);

}  // namespace xlings::platform

// What each system provides for the above (os/fs_linux.cpp, os/fs_macos.cpp,
// os/fs_windows.cpp, one per system): the generic code has no platform branch.
namespace xlings::platform_impl {
int rename_no_replace_native(const std::filesystem::path& from, const std::filesystem::path& to);  // 0, or the error
int exchange_native(const std::filesystem::path& a, const std::filesystem::path& b);               // 0, or the error
bool unsupported(int error);    // the error says the filesystem lacks the operation
std::uint64_t device_of(const std::filesystem::path& dir);   // 0: unknown
std::string filesystem_type(const std::filesystem::path& dir);
}  // namespace xlings::platform_impl
